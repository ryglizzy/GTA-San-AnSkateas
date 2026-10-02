/* A car hit on a flat floor: knocks the standing skater (sk_knock with the
 * plugin's roll-over numbers) and prints, step by step, how high his root,
 * hips and head are, how far he has tumbled and where the board is, against
 * the plain thrown arc. Usage: knock.exe <skate-data\assets> [vx vz spin lift] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/skate_ffi.h"

/* A bone's translation (Skate skater space, Y up) in host world space. */
static void place(const float* root, const float* bone, float* out) {
    float b[3] = {bone[12], -bone[14], bone[13]};
    int i;
    for (i = 0; i < 3; i++) out[i] = root[i] * b[0] + root[4 + i] * b[1] + root[8 + i] * b[2] + root[12 + i];
}

static int find(SkSession* s, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++) {
        const char* b = sk_bone_name(s, (unsigned)i);
        if (b && strcmp(b, name) == 0) return i;
    }
    return -1;
}

int main(int argc, char** argv) {
    float floor[18] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0};
    float vx = argc > 2 ? (float)atof(argv[2]) : 7.f, vz = argc > 3 ? (float)atof(argv[3]) : 5.9f;
    float spin = argc > 4 ? (float)atof(argv[4]) : 9.f, lift = argc > 5 ? (float)atof(argv[5]) : 1.26f;
    float spawn[3] = {0, 0, 0.1f}, bones[64 * 16], v[3], w[3], hips[3], head[3], z0;
    SkControls idle;
    SkPose pose;
    SkSession* s;
    int i, n, ih, id;
    if (argc < 2) {
        printf("usage: knock.exe <skate-data\\assets> [vx vz spin lift]\n");
        return 2;
    }
    s = sk_session_new_mode(argv[1], floor, 2, NULL, NULL, 0, 1.0f, "normal");
    if (!s || sk_activate(s, spawn, 0.0f) != 0) {
        printf("start: %s\n", sk_last_error());
        return 1;
    }
    memset(&idle, 0, sizeof(idle));
    for (i = 0; i < 120; i++) sk_step(s, &idle);
    sk_get_pose(s, &pose);
    n = sk_get_bones(s, bones, 64);
    ih = find(s, n, "HIPS");
    id = find(s, n, "HEAD");
    if (ih < 0 || id < 0) {
        printf("no HIPS/HEAD bone (%d bones)\n", n);
        return 1;
    }
    z0 = pose.root[14] + lift;
    printf("standing: root z %.2f, state %s; knock (%.1f, 0, %.1f) m/s, spin %.1f rad/s, lift %.2f m\n", pose.root[14],
           sk_state(s), vx, vz, spin, lift);
    /* The car comes along +X: he goes up and along +X, tumbling back (head toward -X). */
    v[0] = vx; v[1] = 0; v[2] = vz;
    w[0] = 0; w[1] = -spin; w[2] = 0;
    if (argc > 6) { /* spin about another host axis: x, y or z */
        w[0] = w[1] = w[2] = 0;
        w[argv[6][0] - 'x'] = -spin;
    }
    if (sk_knock(s, v, w, lift, NULL) != 0) {
        printf("knock: %s\n", sk_last_error());
        return 1;
    }
    {
        SkBoardSurface surfaces[8];
        if (sk_board_mesh(s, surfaces, 8) <= 0) {
            printf("board: %s\n", sk_last_error());
            return 1;
        }
    }
    printf(" step   t     state            root z  arc z   hips z  head z  tilt  hips vx vz   board vx vz   board x  low z\n");
    for (i = 1; i <= 90; i++) {
        float t = i / 60.f, dx, dz, tilt, last[3], centre = 0.f, low = 1e9f;
        static float verts[4096 * 6];
        int nv, k;
        memcpy(last, hips, sizeof(last));
        if (sk_step(s, &idle) < 0) {
            printf("step: %s\n", sk_last_error());
            return 1;
        }
        sk_get_pose(s, &pose);
        n = sk_get_bones(s, bones, 64);
        place(pose.root, bones + ih * 16, hips);
        place(pose.root, bones + id * 16, head);
        dx = head[0] - hips[0];
        dz = head[2] - hips[2];
        tilt = atan2f(-dx, dz) * 57.29578f; /* backwards (head toward -X) is positive */
        nv = sk_board_pose(s, verts, 4096);
        for (k = 0; k < nv; k++) {
            centre += verts[k * 6] / (float)nv;
            if (verts[k * 6 + 2] < low) low = verts[k * 6 + 2];
        }
        if (i <= 12 || i % 3 == 0) {
            printf("%4d  %.2f  %-16s %6.2f  %6.2f  %6.2f  %6.2f  %5.0f  %5.1f %5.1f  %5.1f %5.1f  %6.2f %6.2f\n", i, t,
                   sk_state(s), pose.root[14], z0 + vz * t - 4.9f * t * t, hips[2], head[2], tilt,
                   (hips[0] - last[0]) * 60.f, (hips[2] - last[2]) * 60.f, pose.velocity[0], pose.velocity[2], centre, low);
        }
    }
    sk_session_free(s);
    return 0;
}
