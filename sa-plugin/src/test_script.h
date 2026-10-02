// The automated test's script (gta-sa.exe -sktest <file>): what to set up,
// which pad input the skater gets step by step, and when to photograph CJ.
// One command per line; '#' starts a comment.
//
//   load 2                    save slot loaded from the main menu (1-8)
//   place x y z|ground deg    teleport CJ (SA heading: 0 faces north)
//   clock 12 0                game time, then held there
//   weather 1                 forced weather type
//   tuning 0.5 0.5            ArmElbowFollow ShoulderFollow
//   wait 60                   game frames, e.g. for streaming
//   skate / unskate           get on / off the board
//   respawn x y z deg         the skater is put there as Skate's respawn does
//                             (no world built for him first)
//   car 400 15 0 12           a driverless car of that model 15 m ahead of the
//                             skater and 0 m to his left, rolling at him at 12 m/s
//                             (held until it hits him)
//   idle 30                   engine steps (60 a second of game time) with no input
//   pad 30 A rs=0,-1 lt=1     engine steps holding buttons, sticks (-1..1), triggers (0..1)
//   shoot name angles=0,90 dist=3 height=0.3 fov=45 target=1.1
//                             freeze and photograph CJ from those angles
//                             (degrees round him, 0 = ahead of the board);
//                             aim=24 looks at that SA bone instead (24/34: right/left hand)
//   env SK_HELPER 0.5         set an environment variable (skate_ffi reads
//                             SK_* rig constants from them each frame)
//   quit
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

struct TestCommand {
    enum class Kind { Load, Place, Clock, Weather, Tuning, Wait, Skate, Unskate, Respawn, Car, Pad, Shoot, Env, Quit };
    Kind kind = Kind::Quit;
    int line = 0;
    int count = 0;       // Load: slot; Wait: frames; Pad: engine steps
    float args[4] = {};  // Place, Respawn: x y z heading; Car: model ahead left speed; Clock: hours minutes; Weather: type; Tuning: elbow shoulder
    bool ground = false; // Place: z found on the ground
    uint16_t buttons = 0;
    uint8_t triggers[2] = {};
    int16_t left[2] = {}, right[2] = {};
    std::string name; // Shoot; Env: the variable (its value is args[0])
    std::vector<float> angles;
    float distance = 3.f, height = 0.3f, fov = 45.f, target = 1.1f;
    int aim = 0; // Shoot: SA bone ID to look at, 0 = his body
};

namespace test_script {

inline bool Number(const std::string& s, float& out) {
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && end != s.c_str() && *end == '\0' && std::isfinite(out);
}

inline bool Numbers(const std::string& s, std::vector<float>& out) {
    out.clear();
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ',')) {
        float v;
        if (!Number(part, v)) return false;
        out.push_back(v);
    }
    return !out.empty();
}

inline int16_t Axis(float v) {
    v = std::fmax(-1.f, std::fmin(1.f, v));
    return static_cast<int16_t>(std::lround(v * 32767.f));
}

inline uint16_t Button(const std::string& name) {
    static const struct {
        const char* name;
        uint16_t bit;
    } kButtons[] = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
        {"BACK", 0x0020}, {"LS", 0x0040},  {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
        {"A", 0x1000},  {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
    };
    for (const auto& b : kButtons) {
        if (name == b.name) return b.bit;
    }
    return 0;
}

} // namespace test_script

inline bool ParseTestScript(const std::string& text, std::vector<TestCommand>& out, std::string& error) {
    using namespace test_script;
    using Kind = TestCommand::Kind;
    out.clear();
    std::stringstream lines(text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text); // skip a UTF-8 BOM
    std::string raw;
    int number = 0;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(number) + ": " + why;
        return false;
    };
    while (std::getline(lines, raw)) {
        number++;
        raw = raw.substr(0, raw.find('#'));
        std::stringstream words(raw);
        std::vector<std::string> w;
        for (std::string word; words >> word;) w.push_back(word);
        if (w.empty()) continue;
        TestCommand c;
        c.line = number;
        const std::string& verb = w[0];
        auto args = [&](size_t n) {
            if (w.size() != n + 1) return false;
            for (size_t i = 0; i < n; i++) {
                if (!Number(w[i + 1], c.args[i])) return false;
            }
            return true;
        };
        auto steps = [&]() {
            float v;
            if (w.size() < 2 || !Number(w[1], v) || v < 0.f || v > 100000.f) return false;
            c.count = static_cast<int>(v);
            return true;
        };
        if (verb == "load") {
            c.kind = Kind::Load;
            if (!steps() || w.size() != 2 || c.count < 1 || c.count > 8) return fail("load takes a save slot, 1-8");
        } else if (verb == "place") {
            c.kind = Kind::Place;
            if (w.size() == 5 && w[3] == "ground") {
                c.ground = true;
                w[3] = "0";
            }
            if (!args(4)) return fail("place takes x y z|ground heading");
        } else if (verb == "respawn") {
            c.kind = Kind::Respawn;
            if (!args(4)) return fail("respawn takes x y z heading");
        } else if (verb == "car") {
            c.kind = Kind::Car;
            if (!args(4)) return fail("car takes a model, metres ahead and to the left, and a speed");
        } else if (verb == "clock") {
            c.kind = Kind::Clock;
            if (!args(2)) return fail("clock takes hours minutes");
        } else if (verb == "weather") {
            c.kind = Kind::Weather;
            if (!args(1)) return fail("weather takes a weather type");
        } else if (verb == "tuning") {
            c.kind = Kind::Tuning;
            if (!args(2)) return fail("tuning takes elbow and shoulder follow");
        } else if (verb == "env") {
            c.kind = Kind::Env;
            if (w.size() != 3 || !Number(w[2], c.args[0])) return fail("env takes a name and a number");
            c.name = w[1];
        } else if (verb == "wait") {
            c.kind = Kind::Wait;
            if (!steps() || w.size() != 2) return fail("wait takes a number of frames");
        } else if (verb == "skate" || verb == "unskate" || verb == "quit") {
            c.kind = verb == "skate" ? Kind::Skate : verb == "unskate" ? Kind::Unskate : Kind::Quit;
            if (w.size() != 1) return fail(verb + " takes nothing");
        } else if (verb == "idle" || verb == "pad") {
            c.kind = Kind::Pad;
            if (!steps()) return fail(verb + " takes a number of engine steps first");
            if (verb == "idle" && w.size() != 2) return fail("idle takes only a number of steps");
            for (size_t i = 2; i < w.size(); i++) {
                const std::string& t = w[i];
                std::vector<float> v;
                if (uint16_t bit = Button(t)) {
                    c.buttons |= bit;
                } else if ((t.rfind("ls=", 0) == 0 || t.rfind("rs=", 0) == 0) && Numbers(t.substr(3), v) && v.size() == 2) {
                    int16_t* stick = t[0] == 'l' ? c.left : c.right;
                    stick[0] = Axis(v[0]);
                    stick[1] = Axis(v[1]);
                } else if ((t.rfind("lt=", 0) == 0 || t.rfind("rt=", 0) == 0) && Numbers(t.substr(3), v) && v.size() == 1) {
                    c.triggers[t[0] == 'l' ? 0 : 1] = static_cast<uint8_t>(std::lround(std::fmax(0.f, std::fmin(1.f, v[0])) * 255.f));
                } else {
                    return fail("unknown pad input '" + t + "'");
                }
            }
        } else if (verb == "shoot") {
            c.kind = Kind::Shoot;
            if (w.size() < 2 || w[1].find('=') != std::string::npos) return fail("shoot takes a name first");
            c.name = w[1];
            c.angles = {0.f, 90.f, 180.f, 270.f};
            for (size_t i = 2; i < w.size(); i++) {
                size_t eq = w[i].find('=');
                std::string key = w[i].substr(0, eq), value = eq == std::string::npos ? "" : w[i].substr(eq + 1);
                std::vector<float> v;
                if (!Numbers(value, v)) return fail("bad value in '" + w[i] + "'");
                if (key == "angles" && v.size() <= 6) c.angles = v;
                else if (key == "dist" && v.size() == 1) c.distance = v[0];
                else if (key == "height" && v.size() == 1) c.height = v[0];
                else if (key == "fov" && v.size() == 1) c.fov = v[0];
                else if (key == "target" && v.size() == 1) c.target = v[0];
                else if (key == "aim" && v.size() == 1) c.aim = static_cast<int>(v[0]);
                else return fail("unknown shoot setting '" + w[i] + "' (angles takes at most 6)");
            }
        } else {
            return fail("unknown command '" + verb + "'");
        }
        out.push_back(c);
    }
    return true;
}
