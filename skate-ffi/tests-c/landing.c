/* When does a landed trick's sequence end? Prints, step by step after a
 * kickflip on a flat floor, the state, wheels on the ground, whether Skate's
 * sequence is still running and the points landed. Usage: landing.exe <assets> */
#include <stdio.h>
#include <string.h>
#include "../include/skate_ffi.h"

static float floor_tris[18] = {-2000, -2000, 0, 2000, -2000, 0, 2000, 2000, 0, -2000, -2000, 0, 2000, 2000, 0, -2000, 2000, 0};

int main(int argc, char** argv) {
    SkSession* s;
    SkControls c;
    SkFeedback f;
    SkScore sc;
    float spawn[3] = {0, 0, 0.1f};
    int i, landed = -1, last_active = -1, last_wheels = -1;
    if (argc < 2) return 2;
    s = sk_session_new(argv[1], floor_tris, 2, NULL, NULL, 0, 1.0f);
    if (!s || sk_activate(s, spawn, 0.f) != 0) {
        printf("start: %s\n", sk_last_error());
        return 1;
    }
    for (i = 0; i < 400; i++) {
        memset(&c, 0, sizeof(c));
        if (i < 120 && i % 30 < 4) c.buttons = 0x1000; /* push up to speed */
        if (i >= 150 && i < 160) c.right[1] = -32767;  /* crouch */
        else if (i >= 160 && i < 164) { c.right[1] = 32767; c.right[0] = -20000; } /* flick: kickflip */
        if (sk_step(s, &c) < 0) return 1;
        sk_feedback(s, &f);
        sk_score(s, &sc);
        if (i >= 150 && (sc.sequence_active != last_active || (int)f.wheels != last_wheels || i % 10 == 0)) {
            printf("%4d %-18s state %3u wheels %u seq %d (%.0f) total %.0f grade %u\n", i, sk_state(s), f.state, f.wheels,
                   sc.sequence_active, sc.sequence, sc.total, f.landing_grade);
            last_active = sc.sequence_active;
            last_wheels = f.wheels;
        }
        if (landed < 0 && i > 165 && f.state >= 100 && f.state < 200) landed = i;
    }
    sk_session_free(s);
    return 0;
}
