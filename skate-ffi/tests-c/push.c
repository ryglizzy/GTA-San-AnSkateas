/* Measures how fast pushing gets the board in each Skate difficulty:
 * taps and holds on the two push buttons (A and X), 15 s each, on a flat
 * floor. Usage: push.exe <skate-data\assets> */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../include/skate_ffi.h"

#define BUTTON_A 0x1000
#define BUTTON_X 0x4000

typedef struct {
    const char* name;
    unsigned short button;
    int down, period; /* steps held, steps per cycle (60 steps = 1 s) */
} Pattern;

static float speed_of(SkSession* s) {
    SkPose p;
    if (sk_get_pose(s, &p) != 0) return -1.f;
    return sqrtf(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1]);
}

int main(int argc, char** argv) {
    const char* modes[] = {"easy", "normal", "hardcore"};
    const Pattern patterns[] = {
        {"tap A every 0.5 s", BUTTON_A, 4, 30},
        {"tap A every 0.25 s", BUTTON_A, 3, 15},
        {"hold A", BUTTON_A, 900, 900},
        {"tap X every 0.5 s", BUTTON_X, 4, 30},
        {"hold X", BUTTON_X, 900, 900},
    };
    float floor[18] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0};
    int m, k, i;
    if (argc < 2) {
        printf("usage: push.exe <skate-data\\assets>\n");
        return 2;
    }
    for (m = 0; m < 3; m++) {
        SkSession* s = sk_session_new_mode(argv[1], floor, 2, NULL, NULL, 0, 1.0f, modes[m]);
        if (!s) {
            printf("%s: %s\n", modes[m], sk_last_error());
            return 1;
        }
        for (k = 0; k < (int)(sizeof(patterns) / sizeof(patterns[0])); k++) {
            const Pattern* p = &patterns[k];
            float spawn[3] = {0, 0, 0.1f}, top = 0.f, at5 = 0.f, v;
            if (sk_activate(s, spawn, 0.0f) != 0) {
                printf("activate: %s\n", sk_last_error());
                return 1;
            }
            for (i = 0; i < 900; i++) {
                SkControls c;
                memset(&c, 0, sizeof(c));
                if (i % p->period < p->down) c.buttons = p->button;
                if (sk_step(s, &c) < 0) {
                    printf("step: %s\n", sk_last_error());
                    return 1;
                }
                v = speed_of(s);
                if (v > top) top = v;
                if (i == 299) at5 = v;
            }
            printf("%-9s %-20s speed after 5 s %5.2f m/s, top %5.2f m/s, end %5.2f m/s, state %s\n", modes[m], p->name,
                   at5, top, speed_of(s), sk_state(s));
        }
        sk_session_free(s);
    }
    return 0;
}
