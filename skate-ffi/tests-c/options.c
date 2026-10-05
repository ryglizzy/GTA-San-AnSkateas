/* Skate 3's options, changed while a session runs, must act at once and as
 * Skate 3 intends: looser trucks turn the board faster, the Low camera sits
 * lower than High, and a difficulty set live behaves like a session started
 * on it (compared by ollie height). Usage: options.exe <skate-data\assets> */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../include/skate_ffi.h"

#define BUTTON_A 0x1000

static SkSession* s;
static float floor_tris[18] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0};

static int step(SkControls* c) {
    if (sk_step(s, c) < 0) {
        printf("step: %s\n", sk_last_error());
        return 1;
    }
    return 0;
}

static float heading(void) {
    SkPose p;
    sk_get_pose(s, &p);
    return atan2f(-p.root[5], -p.root[4]);
}

static float speed(void) {
    SkPose p;
    sk_get_pose(s, &p);
    return sqrtf(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1]);
}

static int start(void) {
    float spawn[3] = {0, 0, 0.1f};
    SkControls c;
    int i;
    if (sk_activate(s, spawn, 0.f) != 0) {
        printf("activate: %s\n", sk_last_error());
        return 1;
    }
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 60; i++) if (step(&c)) return 1;
    return 0;
}

/* Pushes up to speed, then steers full left for 1.5 s: degrees turned per second. */
static float turn_rate(float tightness, float stick, float* at_speed) {
    SkControls c;
    float h0, h1, turned;
    int i;
    if (start()) return -1.f;
    if (sk_set_trucks(s, tightness) != 0) printf("set_trucks: %s\n", sk_last_error());
    for (i = 0; i < 180; i++) {
        memset(&c, 0, sizeof(c));
        c.buttons = BUTTON_A; /* held: up to top push speed */
        if (step(&c)) return -1.f;
    }
    *at_speed = speed();
    h0 = heading();
    turned = 0.f;
    for (i = 0; i < 90; i++) {
        memset(&c, 0, sizeof(c));
        c.left[0] = (short)(-32767 * stick);
        if (step(&c)) return -1.f;
        h1 = heading();
        float d = h1 - h0;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        turned += d;
        h0 = h1;
    }
    return fabsf(turned) * 57.29578f / 1.5f;
}

/* An ollie from a roll: the highest the skater root rises (m). */
static float ollie_height(void) {
    SkControls c;
    SkPose p;
    float z0, top = 0.f;
    int i;
    if (start()) return -1.f;
    for (i = 0; i < 90; i++) {
        memset(&c, 0, sizeof(c));
        if (i % 30 < 4) c.buttons = BUTTON_A;
        if (step(&c)) return -1.f;
    }
    sk_get_pose(s, &p);
    z0 = p.root[14];
    for (i = 0; i < 90; i++) {
        memset(&c, 0, sizeof(c));
        if (i < 10) c.right[1] = -32767;
        else if (i < 15) c.right[1] = 32767;
        if (step(&c)) return -1.f;
        sk_get_pose(s, &p);
        if (p.root[14] - z0 > top) top = p.root[14] - z0;
    }
    return top;
}

/* Where the camera sits from the skater after settling: height and distance. */
static void camera_spot(int low, float* up, float* back) {
    SkControls c;
    SkPose p;
    int i;
    sk_set_camera(s, low);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 180; i++) step(&c);
    sk_get_pose(s, &p);
    *up = p.camera_position[2] - p.root[14];
    *back = sqrtf((p.camera_position[0] - p.root[12]) * (p.camera_position[0] - p.root[12]) +
                  (p.camera_position[1] - p.root[13]) * (p.camera_position[1] - p.root[13]));
}

int main(int argc, char** argv) {
    const char* modes[] = {"easy", "normal", "hardcore"};
    float fresh[3], live[3], v, up, back;
    int m, b;
    if (argc < 2) {
        printf("usage: options.exe <skate-data\\assets>\n");
        return 2;
    }
    for (m = 0; m < 3; m++) {
        s = sk_session_new_mode(argv[1], floor_tris, 2, NULL, NULL, 0, 1.0f, modes[m]);
        if (!s) {
            printf("session %s: %s\n", modes[m], sk_last_error());
            return 1;
        }
        fresh[m] = ollie_height();
        if (m == 0) { /* the easy session also does the live checks */
            for (b = 0; b < 3; b++) {
                if (sk_set_difficulty(s, modes[b]) != 0) printf("set_difficulty: %s\n", sk_last_error());
                live[b] = ollie_height();
            }
            sk_set_difficulty(s, "easy");
            printf("trucks (10 bars, bar k = (k-1)/9; Skate 3 stock 0.7):\n");
            for (b = 1; b <= 10; b++) {
                float t = (b - 1) / 9.f, full = turn_rate(t, 1.f, &v), half = turn_rate(t, 0.5f, &v);
                printf("  bar %2d (%.2f): full lock %5.1f deg/s, half stick %5.1f deg/s, at %.1f m/s\n", b, t, full, half, v);
            }
            if (start() == 0) {
                camera_spot(0, &up, &back);
                printf("camera High: %.2f m up, %.2f m away\n", up, back);
                camera_spot(1, &up, &back);
                printf("camera Low:  %.2f m up, %.2f m away\n", up, back);
            }
        }
        sk_session_free(s);
    }
    for (m = 0; m < 3; m++) printf("ollie on %-8s: fresh session %.3f m, set live %.3f m\n", modes[m], fresh[m], live[m]);
    return 0;
}
