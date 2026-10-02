/* Loads the 32-bit DLL and calls across the C boundary.
 * No argument: sk_session_new should fail with a readable error.
 * With a skate-data\assets path: starts a real session and reports the
 * memory and time it takes. Linked large-address-aware, like a 4 GB
 * patched gta_sa.exe. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include "../include/skate_ffi.h"

static double now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

static void memory(const char* when) {
    PROCESS_MEMORY_COUNTERS_EX pm = {sizeof(pm)};
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    GlobalMemoryStatusEx(&ms);
    printf("  %-12s private %5.0f MB (peak %5.0f) | address space used %5.0f of %5.0f MB\n", when,
           pm.PrivateUsage / 1048576.0, pm.PeakPagefileUsage / 1048576.0,
           (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0, ms.ullTotalVirtual / 1048576.0);
}

int main(int argc, char** argv) {
    const char* assets = argc > 1 ? argv[1] : "C:\\no-skate-data-here";
    float floor[18] = {-200, -200, 0, 200, -200, 0, 200, 200, 0, -200, -200, 0, 200, 200, 0, -200, 200, 0};
    float spawn[3] = {0, 0, 1};
    SkControls idle;
    SkPose pose;
    SkSession* s;
    double t;
    int i, steps = 600;

    printf("pointer size: %u bits\n", (unsigned)(sizeof(void*) * 8));
    memory("start");
    t = now_ms();
    s = sk_session_new(assets, floor, 2, NULL, NULL, 0, 1.0f);
    if (!s) {
        printf("sk_session_new failed:\n  %s\n", sk_last_error());
        return argc > 1;
    }
    printf("session started in %.0f ms; step period %.4f s\n", now_ms() - t, sk_period(s));
    memory("after load");
    if (sk_activate(s, spawn, 0.0f) != 0) {
        printf("activate failed: %s\n", sk_last_error());
        return 1;
    }
    memset(&idle, 0, sizeof(idle));
    t = now_ms();
    for (i = 0; i < steps; i++) {
        if (sk_step(s, &idle) < 0) {
            printf("step %d failed: %s\n", i, sk_last_error());
            return 1;
        }
    }
    t = now_ms() - t;
    sk_get_pose(s, &pose);
    printf("%d steps: %.3f ms each; %u bones, state %s, root at %.2f %.2f %.2f\n", steps, t / steps,
           pose.bone_count, sk_state(s), pose.root[12], pose.root[13], pose.root[14]);
    memory("after steps");

    /* Momentum onto the board: start at 8 m/s along +X and coast for 2 s. */
    {
        float start[3] = {0, 0, 0.1f}, push[3] = {8, 0, 0};
        int coast = 120;
        if (sk_activate(s, start, 0.0f) != 0 || sk_set_velocity(s, push) != 0) {
            printf("momentum setup failed: %s\n", sk_last_error());
            return 1;
        }
        for (i = 0; i < coast; i++) {
            if (sk_step(s, &idle) < 0) {
                printf("momentum step %d failed: %s\n", i, sk_last_error());
                return 1;
            }
            if (i == 0 || i == 29 || i == coast - 1) {
                sk_get_pose(s, &pose);
                printf("  coast %3d: x=%6.2f y=%6.2f z=%5.2f speed=%5.2f state=%s\n", i + 1, pose.root[12],
                       pose.root[13], pose.root[14],
                       sqrt(pose.velocity[0] * pose.velocity[0] + pose.velocity[1] * pose.velocity[1]), sk_state(s));
            }
        }
    }
    /* A long drop: from 400 m he must land, not be reset (Skate's air reset is
     * 25 s here, not 5). */
    {
        float high[3] = {0, 0, 400}, last = 400.f;
        int k, landed = -1, reset = -1;
        if (sk_activate(s, high, 0.0f) != 0) {
            printf("drop setup failed: %s\n", sk_last_error());
            return 1;
        }
        for (k = 0; k < 1500 && landed < 0 && reset < 0; k++) {
            if (sk_step(s, &idle) < 0 || sk_get_pose(s, &pose) != 0) {
                printf("drop step %d failed: %s\n", k, sk_last_error());
                return 1;
            }
            if (strcmp(sk_state(s), "Teleporting") == 0 || pose.root[14] > last + 5.f) reset = k;
            else if (pose.root[14] < 1.0f) landed = k;
            last = pose.root[14];
        }
        printf("drop from 400 m: %s after %.1f s (state %s)\n", landed >= 0 ? "landed" : reset >= 0 ? "RESET" : "STILL FALLING",
               (landed >= 0 ? landed : reset >= 0 ? reset : k) / 60.f, sk_state(s));
        if (landed < 0) return 1;
    }
    /* Bone space: spawn 100 m out and compare the HIPS bone with the root. */
    {
        float away[3] = {100, 50, 0.1f};
        static float bones[64 * 16];
        int count, k;
        if (sk_activate(s, away, 0.0f) == 0 && sk_get_pose(s, &pose) == 0) {
            count = sk_get_bones(s, bones, 64);
            for (k = 0; k < count; k++) {
                const char* name = sk_bone_name(s, (uint32_t)k);
                if (name && strcmp(name, "HIPS") == 0) {
                    printf("bone space: root at %.2f %.2f %.2f (host); HIPS raw translation %.2f %.2f %.2f (Skate space)\n",
                           pose.root[12], pose.root[13], pose.root[14], bones[k * 16 + 12], bones[k * 16 + 13],
                           bones[k * 16 + 14]);
                }
            }
        }
    }

    /* Rig: CJ's skeleton as logged in game (bind positions; rotations left as
     * identity), fitted to the skater standing on the board. */
    {
        static const struct { const char *skate, *child; int parent, childIndex; float x, y, z; } cj[] = {
            {0, 0, -1, -1, 0, 0, 0},                       {"HIPS", "SPINE", 0, 3, 0, 0, 0},
            {"HIPS", "SPINE", 1, 3, 0, -.001f, -.001f},    {"SPINE", "SPINE2", 2, 4, 0, -.001f, .189f},
            {"SPINE2", "NECK", 3, 5, 0, -.001f, .479f},    {0, 0, 4, -1, .024f, -.001f, .591f},
            {0, 0, 5, -1, .039f, -.005f, .602f},           {0, 0, 5, -1, .007f, .019f, .674f},
            {0, 0, 5, -1, .006f, -.025f, .674f},           {"LEFTSHOULDER", "LEFTARM", 4, 10, 0, -.001f, .479f},
            {"LEFTARM", "LEFTFOREARM", 9, 11, 0, .191f, .449f}, {"LEFTFOREARM", "LEFTHAND", 10, 12, .011f, .330f, .212f},
            {"LEFTHAND", 0, 11, -1, .026f, .471f, -.028f}, {0, 0, 12, -1, .031f, .512f, -.099f},
            {0, 0, 13, -1, .032f, .520f, -.140f},          {"RIGHTSHOULDER", "RIGHTARM", 4, 16, 0, -.001f, .479f},
            {"RIGHTARM", "RIGHTFOREARM", 15, 17, 0, -.193f, .449f}, {"RIGHTFOREARM", "RIGHTHAND", 16, 18, .011f, -.332f, .212f},
            {"RIGHTHAND", 0, 17, -1, .026f, -.473f, -.028f}, {0, 0, 18, -1, .031f, -.514f, -.099f},
            {0, 0, 19, -1, .032f, -.522f, -.140f},         {0, 0, 3, -1, 0, .191f, .449f},
            {0, 0, 3, -1, 0, -.193f, .449f},               {0, 0, 2, -1, 0, 0, .059f},
            {"LEFTUPLEG", "LEFTLEG", 1, 25, 0, .112f, 0},  {"LEFTLEG", "LEFTFOOT", 24, 26, .036f, .140f, -.453f},
            {"LEFTFOOT", "LEFTTOEBASE", 25, 27, -.002f, .170f, -.934f}, {"LEFTTOEBASE", 0, 26, -1, .152f, .170f, -1.036f},
            {"RIGHTUPLEG", "RIGHTLEG", 1, 29, 0, -.112f, 0}, {"RIGHTLEG", "RIGHTFOOT", 28, 30, .036f, -.140f, -.453f},
            {"RIGHTFOOT", "RIGHTTOEBASE", 29, 31, -.002f, -.170f, -.934f}, {"RIGHTTOEBASE", 0, 30, -1, .152f, -.170f, -1.036f},
        };
        static SkRigBone rig[32];
        static float posed[32 * 16];
        float start[3] = {0, 0, 0.1f}, q[4];
        int b, n = 32;
        for (b = 0; b < n; b++) {
            memset(&rig[b], 0, sizeof(rig[b]));
            rig[b].skate = cj[b].skate;
            rig[b].skate_child = cj[b].child;
            rig[b].parent = cj[b].parent;
            rig[b].child = cj[b].childIndex;
            rig[b].bind[0] = rig[b].bind[5] = rig[b].bind[10] = rig[b].bind[15] = 1.f;
            rig[b].bind[12] = cj[b].x;
            rig[b].bind[13] = cj[b].y;
            rig[b].bind[14] = cj[b].z;
        }
        if (sk_rig_setup(s, rig, (uint32_t)n, NAN) != n || sk_rig_facing(s, q) != 0 || sk_activate(s, start, 0.0f) != 0) {
            printf("rig setup failed: %s\n", sk_last_error());
            return 1;
        }
        for (i = 0; i < 120; i++) sk_step(s, &idle);
        sk_get_pose(s, &pose);
        if (sk_rig_pose(s, posed, (uint32_t)n) != n) {
            printf("rig pose failed: %s\n", sk_last_error());
            return 1;
        }
#define P(k, a) (posed[(k) * 16 + 12 + (a)] - pose.root[12 + (a)])
#define LEN(i, j) sqrtf((P(i, 0) - P(j, 0)) * (P(i, 0) - P(j, 0)) + (P(i, 1) - P(j, 1)) * (P(i, 1) - P(j, 1)) + (P(i, 2) - P(j, 2)) * (P(i, 2) - P(j, 2)))
        printf("rig: facing %.3f %.3f %.3f %.3f; above the board: hips %.2f, head %.2f, toes %.2f / %.2f\n", q[0], q[1],
               q[2], q[3], P(2, 2), P(8, 2), P(27, 2), P(31, 2));
        printf("rig: upper arm %.3f (CJ %.3f), shin %.3f (CJ %.3f), clavicle to chest %.3f\n", LEN(10, 11),
               sqrtf(.011f * .011f + .139f * .139f + .237f * .237f), LEN(25, 26),
               sqrtf(.038f * .038f + .03f * .03f + .481f * .481f), LEN(9, 4));

        /* The board: under the feet, about 0.8 m long, wheels on the ground. */
        {
            SkBoardSurface parts[8];
            int count = sk_board_mesh(s, parts, 8), total = 0, k;
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f}, *verts;
            for (k = 0; k < count && k < 8; k++) total += (int)parts[k].vertex_count;
            verts = (float*)malloc(sizeof(float) * 6 * (total > 0 ? total : 1));
            if (count <= 0 || sk_board_pose(s, verts, (uint32_t)total) != total) {
                printf("board failed: %s\n", sk_last_error());
                return 1;
            }
            for (k = 0; k < total; k++) {
                int a;
                for (a = 0; a < 3; a++) {
                    float v = verts[k * 6 + a] - pose.root[12 + a];
                    lo[a] = fminf(lo[a], v);
                    hi[a] = fmaxf(hi[a], v);
                }
            }
            printf("board: %d surfaces, %d vertices, textures %ux%u %ux%u; around the feet x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n",
                   count, total, parts[0].width, parts[0].height, count > 1 ? parts[1].width : 0, count > 1 ? parts[1].height : 0,
                   lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            free(verts);
        }

        /* Kickflip (flick the right stick down, then up-right) and measure the
         * arms: scale/shear in the output matrices, and forearm twist against
         * the upper arm (angle between their side axes about the forearm). */
        {
            static const int arms[4] = {10, 11, 16, 17}; /* L upper, L fore, R upper, R fore */
            float worstScale = 0.f, minDet = 10.f, maxTwist[2] = {0.f, 0.f};
            char states[256] = "";
            for (i = 0; i < 150; i++) {
                SkControls c;
                memset(&c, 0, sizeof(c));
                if (i >= 30 && i < 40) c.right[1] = -32767;
                if (i >= 40 && i < 45) { c.right[0] = 23000; c.right[1] = 23000; }
                if (sk_step(s, &c) < 0 || sk_rig_pose(s, posed, (uint32_t)n) != n) break;
                if (!strstr(states, sk_state(s)) && strlen(states) + strlen(sk_state(s)) < 200) {
                    strcat(states, sk_state(s));
                    strcat(states, " ");
                }
                for (b = 0; b < 4; b++) {
                    const float* m = &posed[arms[b] * 16];
                    float cx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                    float cy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                    float cz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                                m[8] * (m[1] * m[6] - m[2] * m[5]);
                    float dev = fmaxf(fabsf(cx - 1), fmaxf(fabsf(cy - 1), fabsf(cz - 1)));
                    if (dev > worstScale) worstScale = dev;
                    if (det < minDet) minDet = det;
                }
                for (b = 0; b < 2; b++) { /* twist: upper vs fore side (y) axes, about the fore (x) axis */
                    const float *u = &posed[arms[b * 2] * 16], *f = &posed[arms[b * 2 + 1] * 16];
                    float ax[3] = {f[0], f[1], f[2]}, uy[3] = {u[4], u[5], u[6]}, fy[3] = {f[4], f[5], f[6]};
                    float al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), d1, d2, cosang;
                    int k;
                    for (k = 0; k < 3; k++) ax[k] /= al;
                    d1 = uy[0] * ax[0] + uy[1] * ax[1] + uy[2] * ax[2];
                    d2 = fy[0] * ax[0] + fy[1] * ax[1] + fy[2] * ax[2];
                    for (k = 0; k < 3; k++) { uy[k] -= d1 * ax[k]; fy[k] -= d2 * ax[k]; }
                    cosang = (uy[0] * fy[0] + uy[1] * fy[1] + uy[2] * fy[2]) /
                             (sqrtf(uy[0] * uy[0] + uy[1] * uy[1] + uy[2] * uy[2]) * sqrtf(fy[0] * fy[0] + fy[1] * fy[1] + fy[2] * fy[2]) + 1e-9f);
                    cosang = fmaxf(-1.f, fminf(1.f, cosang));
                    if (acosf(cosang) * 57.2958f > maxTwist[b]) maxTwist[b] = acosf(cosang) * 57.2958f;
                }
            }
            printf("flip: states %s\n", states);
            printf("flip: arm matrices worst scale deviation %.3f, smallest determinant %.3f; forearm twist max L %.0f, R %.0f deg\n",
                   worstScale, minDet, maxTwist[0], maxTwist[1]);
        }
    }

    /* World building: a 120 m street of 2 m ground cells, a 15 cm curb line,
     * a 3-step staircase and a 4 m block, built directly and in the background. */
    {
        static float world[60000 * 9];
        int n = 0, gx, gy, generation;
        SkWorldInfo info;
#define TRI(ax, ay, az, bx, by, bz, cx, cy, cz)                                                      \
    do {                                                                                             \
        float* t_ = &world[n++ * 9];                                                                 \
        t_[0] = ax; t_[1] = ay; t_[2] = az; t_[3] = bx; t_[4] = by; t_[5] = bz; t_[6] = cx; t_[7] = cy; t_[8] = cz; \
    } while (0)
#define QUAD_UP(x0, y0, x1, y1, z)                                                                   \
    do {                                                                                             \
        TRI(x0, y0, z, x1, y0, z, x1, y1, z);                                                        \
        TRI(x0, y0, z, x1, y1, z, x0, y1, z);                                                        \
    } while (0)
        for (gy = -60; gy < 60; gy++)
            for (gx = -60; gx < 60; gx++) QUAD_UP(gx * 1.f, gy * 1.f, gx * 1.f + 1, gy * 1.f + 1, 0.f);
        QUAD_UP(-60.f, 8.f, 60.f, 10.f, 0.15f); /* curb top */
        TRI(-60, 8, 0, 60, 8, 0, 60, 8, 0.15f); TRI(-60, 8, 0, 60, 8, 0.15f, -60, 8, 0.15f); /* curb face */
        for (gx = 0; gx < 3; gx++) QUAD_UP(20.f, 20.f + gx * 0.4f, 24.f, 20.4f + gx * 0.4f, 0.17f * (gx + 1));
        QUAD_UP(-30.f, -30.f, -20.f, -20.f, 4.f); /* block roof */
        t = now_ms();
        generation = sk_install_world(s, world, (uint32_t)n);
        if (generation < 0 || sk_world_info(s, &info) != 0) {
            printf("install_world failed: %s\n", sk_last_error());
            return 1;
        }
        printf("install_world: %d triangles in, %u kept, %u rails, built in %.0f ms (call %.0f ms)\n", n, info.triangles,
               info.rails, info.build_ms, now_ms() - t);
        t = now_ms();
        generation = sk_queue_world(s, world, (uint32_t)n);
        for (i = 0; i < 600; i++) {
            if (sk_step(s, &idle) < 0 || sk_world_info(s, &info) != 0) {
                printf("queued world step failed: %s\n", sk_last_error());
                return 1;
            }
            if ((int)info.generation >= generation) break;
        }
        printf("queue_world: installed after %d steps (%.0f ms), generation %u, failures %u\n", i + 1, now_ms() - t,
               info.generation, info.failures);
        memory("after worlds");
    }
    sk_session_free(s);
    return 0;
}
