/* Grinds in a world dumped from GTA (test-worlds\*.tris) and prints how it
 * goes, step by step, then a RESULT line: metres ground, where and how the
 * grind ended. With SK_TRACE_WIPEOUT=1 the engine also names every bail
 * check that fires.
 * Usage: grind.exe <skate-data\assets> <world.tris> x y z yaw speed
 *                  [ollie_after_steps] [steer -1..1] [difficulty] [pitch]
 * ollie_after_steps < 0: no ollie; the skater starts in the air at x y z
 * moving along yaw and pitch (radians, up positive), to drop onto a rail. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/skate_ffi.h"

static float* load_tris(const char* path, uint32_t* count) {
    FILE* f = fopen(path, "rb");
    char magic[8];
    float centre[3];
    float* tris;
    if (!f) return NULL;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SKTRIS01", 8) != 0 || fread(centre, 4, 3, f) != 3 ||
        fread(count, 4, 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    tris = (float*)malloc(*count * 36);
    if (fread(tris, 36, *count, f) != *count) {
        fclose(f);
        free(tris);
        return NULL;
    }
    fclose(f);
    return tris;
}

static SkSession* s;
static int quiet; /* batch runs print only their RESULT lines */

static int run(const float* at, float yaw, float pitch, float speed, int ollie_at, float steer);

int main(int argc, char** argv) {
    SkWorldInfo info;
    uint32_t n;
    float* tris;
    float at[3], yaw, speed, steer = 0.f, pitch = 0.f;
    int ollie_at = 60;
    if (argc < 5) {
        printf("usage: grind.exe <assets> <world.tris> x y z yaw speed [ollie_after_steps] [steer] [difficulty] [pitch]\n"
               "       grind.exe <assets> <world.tris> --batch <file of: x y z yaw pitch speed> [difficulty]\n");
        return 2;
    }
    tris = load_tris(argv[2], &n);
    if (!tris) {
        printf("can't read %s\n", argv[2]);
        return 1;
    }
    quiet = strcmp(argv[3], "--batch") == 0;
    s = sk_session_new_mode(argv[1], tris, n, NULL, NULL, 0, 1.0f,
                            quiet ? (argc > 5 ? argv[5] : "easy") : (argc > 10 ? argv[10] : "easy"));
    if (!s || sk_install_world(s, tris, n) < 0) {
        printf("session: %s\n", sk_last_error());
        return 1;
    }
    if (sk_world_info(s, &info) >= 0) {
        fprintf(stderr, "world: %u triangles, %u rails, built in %.1f ms\n", info.triangles, info.rails, info.build_ms);
    }
    if (quiet) {
        FILE* f = fopen(argv[4], "r");
        int k = 0;
        if (!f) {
            printf("can't read %s\n", argv[4]);
            return 1;
        }
        while (fscanf(f, "%f %f %f %f %f %f", &at[0], &at[1], &at[2], &yaw, &pitch, &speed) == 6) {
            printf("RUN %d %.2f %.2f %.2f: ", k, at[0], at[1], at[2]);
            fprintf(stderr, "RUN %d\n", k++);
            fflush(stdout);
            if (run(at, yaw, pitch, speed, -1, 0.f)) return 1;
        }
        fclose(f);
    } else {
        if (argc < 8) {
            printf("usage: see above\n");
            return 2;
        }
        at[0] = (float)atof(argv[3]);
        at[1] = (float)atof(argv[4]);
        at[2] = (float)atof(argv[5]);
        yaw = (float)atof(argv[6]);
        speed = (float)atof(argv[7]);
        if (argc > 8) ollie_at = atoi(argv[8]);
        if (argc > 9) steer = (float)atof(argv[9]);
        if (argc > 11) pitch = (float)atof(argv[11]);
        if (run(at, yaw, pitch, speed, ollie_at, steer)) return 1;
    }
    sk_session_free(s);
    free(tris);
    return 0;
}

static int run(const float* at, float yaw, float pitch, float speed, int ollie_at, float steer) {
    SkControls c;
    SkPose p;
    SkFeedback fb;
    float vel[3], prev[3], ground = 0.f, end[3] = {0, 0, 0};
    int i, wiped = -1, grinding = 0, ended = -1, started = -1;
    char last[64] = "", after[64] = "-";
    if (sk_activate(s, at, yaw) != 0) {
        printf("activate: %s\n", sk_last_error());
        return 1;
    }
    vel[0] = speed * cosf(pitch) * cosf(yaw);
    vel[1] = speed * cosf(pitch) * sinf(yaw);
    vel[2] = speed * sinf(pitch);
    sk_set_velocity(s, vel);
    for (i = 0; i < 1200; i++) {
        const char* state;
        int k = i - ollie_at;
        memset(&c, 0, sizeof(c));
        if (ollie_at >= 0) {
            if (k >= 0 && k < 10) c.right[1] = -32767;      /* crouch */
            else if (k >= 10 && k < 15) c.right[1] = 32767; /* flick: ollie */
            if (k >= 0 && k < 40) c.left[0] = (short)(32767 * steer);
        }
        if (sk_step(s, &c) < 0) {
            printf("step %d: %s\n", i, sk_last_error());
            return 1;
        }
        sk_get_pose(s, &p);
        sk_feedback(s, &fb);
        state = sk_state(s);
        if (!quiet && (strcmp(state, last) != 0 || i % 6 == 0)) {
            float v = sqrtf(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
            fprintf(stderr, "%4d %-22s at %9.3f %8.3f %7.3f  %5.2f m/s  wheels %u  board v %6.2f %6.2f %6.2f\n", i, state,
                    p.root[12], p.root[13], p.root[14], v, fb.wheels, fb.board_velocity[0], fb.board_velocity[1],
                    fb.board_velocity[2]);
            strncpy(last, state, sizeof(last) - 1);
        }
        if (strncmp(state, "Grind", 5) == 0 || (grinding && strcmp(state, "Nonspecific") == 0)) {
            if (grinding) {
                ground += sqrtf((p.root[12] - prev[0]) * (p.root[12] - prev[0]) + (p.root[13] - prev[1]) * (p.root[13] - prev[1]) +
                                (p.root[14] - prev[2]) * (p.root[14] - prev[2]));
            } else if (ended < 0) {
                started = i;
            }
            grinding = ended < 0;
            memcpy(prev, &p.root[12], sizeof(prev));
        } else if (grinding) {
            grinding = 0;
            ended = i;
            memcpy(end, &p.root[12], sizeof(end));
            strncpy(after, state, sizeof(after) - 1);
        }
        if (strstr(state, "Wipeout") && wiped < 0) wiped = i;
        if (wiped >= 0 && i > wiped + 30) break;
        if (ended >= 0 && i > ended + 90) break;
    }
    if (started < 0) {
        printf("RESULT no grind\n");
    } else {
        printf("RESULT ground %.1f m from step %d, ended %s at %.2f %.2f %.2f -> %s, wipeout %s", ground, started,
               ended < 0 ? "never" : "", end[0], end[1], end[2], after,
               wiped < 0 ? "none" : (ended >= 0 && wiped <= ended + 60 ? "right after" : "later"));
        if (wiped >= 0 && sk_feedback(s, &fb) == 0) {
            printf(" (first check %d, checks %08x%08x)", (int)fb.bail_first_check, fb.bail_checks[1], fb.bail_checks[0]);
        }
        printf("\n");
    }
    fflush(stdout);
    return 0;
}
