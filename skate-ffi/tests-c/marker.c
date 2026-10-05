/* A session marker set on foot must bring the skater back facing the way he
 * faced when it was set. Steps off the board (Y), walks to face a new way,
 * sets a marker (LB + D-pad down), walks off another way, holds LB + D-pad
 * up to return, and compares the headings (as the plugin turns CJ: the
 * root's -Y axis). Usage: marker.exe <skate-data\assets> */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../include/skate_ffi.h"

#define DPAD_UP 0x0001
#define DPAD_DOWN 0x0002
#define BUTTON_LB 0x0100
#define BUTTON_Y 0x8000

static SkSession* s;

static int run(int steps, unsigned short buttons, float lx, float ly) {
    SkControls c;
    int i;
    memset(&c, 0, sizeof(c));
    c.buttons = buttons;
    c.left[0] = (short)(lx * 32767.f);
    c.left[1] = (short)(ly * 32767.f);
    for (i = 0; i < steps; i++) {
        if (sk_step(s, &c) < 0) {
            printf("step: %s\n", sk_last_error());
            return 1;
        }
    }
    return 0;
}

static float bones[256 * 16];

/* The root's heading; also prints the body's turn within the root (the
 * pelvis's sideways axis in skater space, Y up) and the camera's heading. */
static float heading(float* at) {
    SkPose p;
    int n, i, pelvis = -1;
    float deg = 180.f / 3.14159265f, root;
    sk_get_pose(s, &p);
    if (at) memcpy(at, &p.root[12], 3 * sizeof(float));
    root = (atan2f(-p.root[5], -p.root[4]) - 3.14159265f * 0.5f) * deg;
    n = sk_get_bones(s, bones, 256);
    for (i = 0; i < n && pelvis < 0; i++) {
        const char* name = sk_bone_name(s, (unsigned)i);
        if (name && strcmp(name, "HIPS") == 0) pelvis = i;
    }
    if (pelvis >= 0) {
        const float* b = &bones[pelvis * 16];
        printf("    body: hips turned %.1f degrees within the root\n", atan2f(b[10], b[8]) * deg);
    }
    if (p.has_camera) printf("    camera looks along heading %.1f\n", (atan2f(p.camera_forward[1], p.camera_forward[0]) - 3.14159265f * 0.5f) * deg);
    return root;
}

static float wrap(float d) {
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}

int main(int argc, char** argv) {
    float floor[18] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0};
    float spawn[3] = {0, 0, 0.1f}, at[3], set_at[3];
    float set_heading, away_heading, back_heading;
    int walking;
    SkMarker m;
    if (argc < 2) {
        printf("usage: marker.exe <skate-data\\assets>\n");
        return 2;
    }
    s = sk_session_new_mode(argv[1], floor, 2, NULL, NULL, 0, 1.0f, "normal");
    if (!s || sk_activate(s, spawn, 0.3f) != 0) {
        printf("session: %s\n", sk_last_error());
        return 1;
    }
    for (walking = 0; walking < 2; walking++) {
        if (run(60, 0, 0, 0)) return 1;
        if (walking == 0 && (run(10, BUTTON_Y, 0, 0) || run(120, 0, 0, 0))) return 1;
        printf("--- %s: state %s\n", walking ? "standing still" : "after Y", sk_state(s));
        if (walking == 0 && (run(70, 0, 0.7f, 0.7f) || run(60, 0, 0, 0))) return 1; /* walk to face a new way */
        set_heading = heading(set_at);
        if (run(10, BUTTON_LB, 0, 0) || run(10, BUTTON_LB | DPAD_DOWN, 0, 0) || run(10, BUTTON_LB, 0, 0) || run(30, 0, 0, 0)) return 1;
        sk_marker(s, &m);
        printf("marker set at %.2f %.2f facing %.1f (state %s; can return %d)\n", set_at[0], set_at[1], set_heading, sk_state(s),
               m.can_return);
        if (run(90, 0, -0.7f, -0.2f) || run(60, 0, 0, 0)) return 1; /* walk off another way */
        away_heading = heading(at);
        printf("walked to %.2f %.2f facing %.1f\n", at[0], at[1], away_heading);
        if (run(240, BUTTON_LB | DPAD_UP, 0, 0) || run(120, 0, 0, 0)) return 1;
        back_heading = heading(at);
        printf("returned to %.2f %.2f facing %.1f (state %s): off by %.1f degrees, %.2f m\n", at[0], at[1], back_heading, sk_state(s),
               wrap(back_heading - set_heading), sqrtf((at[0] - set_at[0]) * (at[0] - set_at[0]) + (at[1] - set_at[1]) * (at[1] - set_at[1])));
    }
    sk_session_free(s);
    return 0;
}
