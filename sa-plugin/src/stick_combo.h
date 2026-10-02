// L3 + R3 chord detection. Pure logic (no game or XInput calls) so it can be
// unit tested: see tests/stick_combo_test.cpp.
//
// On foot, a lone L3 (crouch) or R3 (look behind) is held back from the game
// for up to `window` ms while we wait for the other stick. If it comes, that's
// the combo and neither stick's own action happens. If not, the click goes
// through late: immediately if the stick is still held, otherwise replayed for
// as long as it was held. In vehicles the sticks (horn, sub-missions) pass
// untouched; while skating the game can't see the pad, so nothing waits.
#pragma once

#include <cstdint>
#include <initializer_list>

class StickCombo {
public:
    // Feed the physical stick-click state once per frame. Returns true on the
    // frame the combo completes.
    bool Update(bool left, bool right, bool onFoot, bool skating, uint64_t now, uint64_t window) {
        bool fired = false;
        Edge(l3_, r3_, left, onFoot, skating, now, fired);
        Edge(r3_, l3_, right, onFoot, skating, now, fired);
        for (Stick* s : {&l3_, &r3_}) {
            if (!onFoot) {
                s->pending = s->replay = false;
            } else if (s->pending && now - s->pressedAt >= window) {
                s->pending = false; // the other stick never came: let this click through
                if (!s->held) {
                    s->replay = true;
                    s->replayUntil = now + (s->releasedAt - s->pressedAt);
                }
            }
        }
        return fired;
    }

    // Rewrites the game's view of the two stick buttons for this frame.
    void Apply(short& l3, short& r3, uint64_t now) {
        Apply(l3_, l3, now);
        Apply(r3_, r3, now);
    }

private:
    struct Stick {
        bool held = false;    // physically down on some pad
        bool pending = false; // hidden, waiting for the other stick
        bool combo = false;   // part of a combo: hidden until released
        bool replay = false;  // delivering a late click to the game
        uint64_t pressedAt = 0, releasedAt = 0, replayUntil = 0;
    };

    static void Edge(Stick& s, Stick& other, bool down, bool onFoot, bool skating, uint64_t now, bool& fired) {
        if (down && !s.held) {
            s.held = true;
            s.pressedAt = now;
            s.replay = false;
            if (onFoot && (other.held || other.pending)) { // pressed together
                fired = true;
                s.combo = true;
                s.pending = false;
                other.combo = other.held;
                other.pending = false;
                other.replay = false;
            } else {
                s.pending = onFoot && !skating;
            }
        } else if (!down && s.held) {
            s.held = false;
            s.combo = false;
            s.releasedAt = now;
        }
    }

    static void Apply(Stick& s, short& button, uint64_t now) {
        if (s.pending || s.combo) {
            button = 0;
        } else if (s.replay) {
            button = 255;
            if (now >= s.replayUntil) s.replay = false; // after at least one frame
        }
    }

    Stick l3_, r3_;
};
