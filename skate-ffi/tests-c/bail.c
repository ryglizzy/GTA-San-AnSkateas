/* How a bail moves the body: a normal bail (riding into a wall) against car
 * knocks (sk_knock as the plugin calls it). Prints, every few steps, the
 * bends of the left elbow, shoulder (arm from the body), hip and knee, then
 * how far each swung over the bail: a stiff bail barely moves them.
 * Usage: bail.exe <skate-data\assets> [wall|slow|fast]... */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/skate_ffi.h"

static SkSession* s;
static int ids[16];
static const char* names[] = {"HIPS", "SPINE2", "LEFTARM", "LEFTFOREARM", "LEFTHAND", "LEFTUPLEG", "LEFTLEG", "LEFTFOOT",
                              "RIGHTARM", "RIGHTFOREARM", "RIGHTHAND", "RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT"};
enum { HIPS, SPINE2, LARM, LFORE, LHAND, LUPLEG, LLEG, LFOOT, RARM, RFORE, RHAND, RUPLEG, RLEG, RFOOT, NBONES };

static void at(const float* bones, int b, float* out) {
    memcpy(out, bones + ids[b] * 16 + 12, 3 * sizeof(float));
}
static float angle(const float* a0, const float* a1, const float* b0, const float* b1) {
    float u[3], v[3], d = 0, lu = 0, lv = 0;
    int i;
    for (i = 0; i < 3; i++) {
        u[i] = a1[i] - a0[i];
        v[i] = b1[i] - b0[i];
        d += u[i] * v[i];
        lu += u[i] * u[i];
        lv += v[i] * v[i];
    }
    d /= sqrtf(lu * lv) + 1e-9f;
    return acosf(d < -1 ? -1 : d > 1 ? 1 : d) * 57.29578f;
}

/* Elbow, shoulder, hip and knee bends (degrees) for both sides. */
static void bends(float* out) {
    float bones[64 * 16], p[NBONES][3];
    int i;
    sk_get_bones(s, bones, 64);
    for (i = 0; i < NBONES; i++) at(bones, i, p[i]);
    for (i = 0; i < 2; i++) {
        int arm = i ? RARM : LARM, fore = i ? RFORE : LFORE, hand = i ? RHAND : LHAND;
        int up = i ? RUPLEG : LUPLEG, leg = i ? RLEG : LLEG, foot = i ? RFOOT : LFOOT;
        out[i * 4 + 0] = angle(p[arm], p[fore], p[fore], p[hand]);       /* elbow: 0 straight */
        out[i * 4 + 1] = angle(p[SPINE2], p[HIPS], p[arm], p[fore]);     /* shoulder: 0 arm down the body */
        out[i * 4 + 2] = angle(p[SPINE2], p[HIPS], p[up], p[leg]);       /* hip: 0 standing straight */
        out[i * 4 + 3] = angle(p[up], p[leg], p[leg], p[foot]);          /* knee: 0 straight */
    }
}

static int run(const char* kind) {
    float spawn[3] = {0, 0, 0.1f}, lo[8], hi[8], b[8], v[3], w[3];
    SkControls c;
    int i, k, bailed = -1;
    if (sk_activate(s, spawn, 0.f) != 0) return 1;
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 60; i++) sk_step(s, &c);
    if (strcmp(kind, "drop") == 0) { /* a hard landing: dropped from 5 m while rolling */
        float high[3] = {0, 0, 5.f};
        sk_set_difficulty(s, "hardcore"); /* easy shrugs off the drop */
        sk_activate(s, high, 0.f);
        v[0] = 6.f; v[1] = 0; v[2] = -4.f;
        sk_set_velocity(s, v);
        for (i = 0; i < 300 && bailed < 0; i++) {
            memset(&c, 0, sizeof(c));
            sk_step(s, &c);
            if (strstr(sk_state(s), "Wipeout") || strstr(sk_state(s), "Biped")) bailed = i;
        }
        if (bailed < 0) { printf("drop: no bail (state %s)\n", sk_state(s)); return 0; }
    } else if (strcmp(kind, "wall") == 0) {
        v[0] = 10.f; v[1] = 0; v[2] = 0;
        sk_set_velocity(s, v);
        for (i = 0; i < 400 && bailed < 0; i++) { /* push into the block at x = 40 */
            memset(&c, 0, sizeof(c));
            if (i % 30 < 4) c.buttons = 0x1000;
            sk_step(s, &c);
            if (strstr(sk_state(s), "Wipeout") || strstr(sk_state(s), "Biped")) bailed = i;
        }
        if (bailed < 0) {
            SkPose p;
            sk_get_pose(s, &p);
            printf("wall: no bail (state %s at %.2f %.2f %.2f, %.1f m/s)\n", sk_state(s), p.root[12], p.root[13], p.root[14],
                   p.velocity[0]);
            return 0;
        }
    } else {
        for (i = 0; i < 120; i++) { /* rolling at push speed, then the car */
            memset(&c, 0, sizeof(c));
            if (i % 30 < 4) c.buttons = 0x1000;
            sk_step(s, &c);
        }
        if (strcmp(kind, "slow") == 0) { /* 7 m/s car from the side: thrown aside, Skate's default tumble */
            v[0] = 0; v[1] = 7.f; v[2] = 2.5f;
            if (sk_knock(s, v, NULL, 0.f, NULL) != 0) return 1;
        } else { /* 15 m/s car from behind: the plugin's roll-over numbers */
            v[0] = 7.5f; v[1] = 0; v[2] = 5.9f;
            w[0] = 0; w[1] = -6.f; w[2] = 0;
            if (sk_knock(s, v, w, 1.26f, NULL) != 0) return 1;
        }
    }
    memset(&c, 0, sizeof(c));
    for (k = 0; k < 8; k++) { lo[k] = 1e9f; hi[k] = -1e9f; }
    printf("%s bail (state %s)\n  step state            L elbow shoulder hip knee | R elbow shoulder hip knee\n", kind, sk_state(s));
    for (i = 0; i <= 90; i++) {
        if (i) sk_step(s, &c);
        bends(b);
        for (k = 0; k < 8; k++) {
            if (b[k] < lo[k]) lo[k] = b[k];
            if (b[k] > hi[k]) hi[k] = b[k];
        }
        if (i % 6 == 0)
            printf("  %4d %-16s %5.0f %5.0f %5.0f %5.0f | %5.0f %5.0f %5.0f %5.0f\n", i, sk_state(s), b[0], b[1], b[2], b[3], b[4],
                   b[5], b[6], b[7]);
    }
    printf("  swing over 1.5 s:      %5.0f %5.0f %5.0f %5.0f | %5.0f %5.0f %5.0f %5.0f\n", hi[0] - lo[0], hi[1] - lo[1],
           hi[2] - lo[2], hi[3] - lo[3], hi[4] - lo[4], hi[5] - lo[5], hi[6] - lo[6], hi[7] - lo[7]);
    for (i = 0; i < 400 && !strstr(sk_state(s), "Physics"); i++) sk_step(s, &c); /* Skate's respawn */
    sk_set_difficulty(s, "easy");
    return 0;
}

int main(int argc, char** argv) {
    /* A floor, and a 2 m block from x = 40 (its face toward -x, and its top). */
    float world[54] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0,
                       40, -50, 0, 40, 50, 2.f, 40, 50, 0, 40, -50, 0, 40, -50, 2.f, 40, 50, 2.f,
                       40, -50, 2.f, 48, -50, 2.f, 48, 50, 2.f, 40, -50, 2.f, 48, 50, 2.f, 40, 50, 2.f};
    int i, n, a;
    if (argc < 2) {
        printf("usage: bail.exe <skate-data\\assets> [wall|slow|fast]...\n");
        return 2;
    }
    s = sk_session_new(argv[1], world, 6, NULL, NULL, 0, 1.0f);
    if (!s) {
        printf("session: %s\n", sk_last_error());
        return 1;
    }
    {
        float spawn[3] = {0, 0, 0.1f};
        sk_activate(s, spawn, 0.f);
    }
    n = 64;
    for (i = 0; i < NBONES; i++) {
        int k;
        ids[i] = -1;
        for (k = 0; k < n; k++) {
            const char* b = sk_bone_name(s, (unsigned)k);
            if (b && strcmp(b, names[i]) == 0) ids[i] = k;
        }
        if (ids[i] < 0) {
            printf("no bone %s\n", names[i]);
            return 1;
        }
    }
    for (a = 2; a < argc || a == 2; a++) {
        if (run(argc > 2 ? argv[a] : "wall")) {
            printf("failed: %s\n", sk_last_error());
            return 1;
        }
        if (argc <= 2) break;
    }
    sk_session_free(s);
    return 0;
}
