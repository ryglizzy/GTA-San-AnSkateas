// Simulates 16 ms game frames through StickCombo and checks what the game
// sees: a crouch is each frame L3 becomes pressed in the game's view (that is
// how SA's CPad::GetDuck reads it); look behind is every frame R3 is visible.
#include "../src/stick_combo.h"

#include <cstdio>
#include <functional>

namespace {

using Input = std::function<bool(uint64_t)>;

struct Result {
    int crouches = 0, lookFrames = 0, combos = 0;
    uint64_t firstCrouch = 0, firstLook = 0;
};

Input Held(uint64_t from, uint64_t to) {
    return [=](uint64_t t) { return t >= from && t < to; };
}
Input Never() {
    return [](uint64_t) { return false; };
}

Result Run(Input left, Input right, bool onFoot = true, bool skating = false) {
    StickCombo combo;
    Result r;
    short previousL3 = 0;
    for (uint64_t t = 16; t <= 2000; t += 16) {
        bool l = left(t), rt = right(t);
        if (combo.Update(l, rt, onFoot, skating, t, 400)) r.combos++;
        short l3 = l ? 255 : 0, r3 = rt ? 255 : 0; // what GInput writes into the pad
        if (onFoot) combo.Apply(l3, r3, t);
        if (l3 && !previousL3 && r.crouches++ == 0) r.firstCrouch = t;
        previousL3 = l3;
        if (r3 && r.lookFrames++ == 0) r.firstLook = t;
    }
    return r;
}

int failures = 0;

void Expect(const char* name, bool ok, const Result& r) {
    std::printf("%s  %-52s crouches=%d (first %llu ms) look=%d frames (first %llu ms) combos=%d\n",
                ok ? "PASS" : "FAIL", name, r.crouches, (unsigned long long)r.firstCrouch, r.lookFrames,
                (unsigned long long)r.firstLook, r.combos);
    if (!ok) failures++;
}

} // namespace

int main() {
    Result r;

    r = Run(Held(16, 112), Never());
    Expect("L3 tap: one late crouch at ~400 ms", r.crouches == 1 && r.firstCrouch >= 400 && r.firstCrouch <= 432 && r.combos == 0, r);

    r = Run(Held(16, 1000), Never());
    Expect("L3 held: crouch when the window ends", r.crouches == 1 && r.firstCrouch >= 400 && r.firstCrouch <= 432, r);

    r = Run(Held(16, 600), Held(200, 600));
    Expect("L3 then R3 within window: combo, no crouch/look", r.combos == 1 && r.crouches == 0 && r.lookFrames == 0, r);

    r = Run(Held(300, 500), Held(16, 500));
    Expect("R3 then L3 within window: combo, no crouch/look", r.combos == 1 && r.crouches == 0 && r.lookFrames == 0, r);

    r = Run(Held(16, 600), Held(16, 600));
    Expect("both on the same frame: exactly one combo", r.combos == 1 && r.crouches == 0 && r.lookFrames == 0, r);

    r = Run(Held(16, 112), Held(300, 500));
    Expect("L3 tapped, R3 inside window: combo eats the tap", r.combos == 1 && r.crouches == 0 && r.lookFrames == 0, r);

    r = Run(Never(), Held(16, 1000));
    Expect("R3 held: look behind starts at ~400 ms", r.lookFrames >= 35 && r.lookFrames <= 39 && r.firstLook >= 400 && r.firstLook <= 432 && r.crouches == 0, r);

    r = Run(Never(), Held(16, 176));
    Expect("R3 tap: replayed late for as long as it was held", r.lookFrames >= 9 && r.lookFrames <= 12 && r.firstLook >= 400 && r.firstLook <= 432, r);

    r = Run(Held(16, 1000), Held(600, 800));
    Expect("L3 held past window, then R3: crouch, then combo", r.crouches == 1 && r.combos == 1 && r.lookFrames == 0, r);

    r = Run(Held(16, 112), Held(800, 900));
    Expect("L3 tap, R3 much later: crouch and look, no combo", r.crouches == 1 && r.combos == 0 && r.lookFrames > 0, r);

    r = Run(Held(16, 112), Held(16, 112), false);
    Expect("in a vehicle: no delay, no combo", r.crouches == 1 && r.firstCrouch == 16 && r.combos == 0 && r.firstLook == 16, r);

    r = Run(Held(16, 600), Held(200, 600), true, true);
    Expect("skating: L3 held then R3 is a combo", r.combos == 1, r);

    r = Run(Held(16, 112), Never(), true, true);
    Expect("skating: a lone L3 isn't held back for later", r.crouches == 1 && r.firstCrouch == 16 && r.combos == 0, r);

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
