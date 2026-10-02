// Checks the -sktest script parser: commands, pad input encoding, shot
// settings, and that mistakes name their line.
#include "../src/test_script.h"

#include <cstdio>

namespace {

int failures = 0;

void Expect(const char* name, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

bool Parse(const char* text, std::vector<TestCommand>& out, std::string& error) {
    error.clear();
    return ParseTestScript(text, out, error);
}

} // namespace

int main() {
    using Kind = TestCommand::Kind;
    std::vector<TestCommand> c;
    std::string error;

    bool ok = Parse("# setup\nload 2\nplace 1640 -2493 ground 270\nclock 12 0  # noon\nweather 1\n"
                    "tuning 0.5 0.25\nwait 60\nskate\nidle 30\npad 20 A rs=0,-1 lt=1\n"
                    "shoot push angles=0,90,180 dist=2.5 fov=40\nunskate\nquit\n",
                    c, error);
    Expect("a full script parses", ok && c.size() == 12);
    if (ok && c.size() == 12) {
        Expect("load keeps its slot", c[0].kind == Kind::Load && c[0].count == 2);
        Expect("place on the ground", c[1].kind == Kind::Place && c[1].ground && c[1].args[0] == 1640.f && c[1].args[3] == 270.f);
        Expect("clock ignores the comment", c[2].kind == Kind::Clock && c[2].args[0] == 12.f && c[2].args[1] == 0.f);
        Expect("tuning", c[4].kind == Kind::Tuning && c[4].args[1] == 0.25f);
        Expect("idle is a pad with nothing held", c[7].kind == Kind::Pad && c[7].count == 30 && c[7].buttons == 0);
        Expect("pad: A, right stick down, full left trigger",
               c[8].count == 20 && c[8].buttons == 0x1000 && c[8].right[0] == 0 && c[8].right[1] == -32767 &&
                   c[8].triggers[0] == 255 && c[8].triggers[1] == 0);
        Expect("shoot settings", c[9].kind == Kind::Shoot && c[9].name == "push" && c[9].angles.size() == 3 &&
                                     c[9].angles[2] == 180.f && c[9].distance == 2.5f && c[9].fov == 40.f &&
                                     c[9].height == 0.3f);
        Expect("unskate and quit", c[10].kind == Kind::Unskate && c[11].kind == Kind::Quit);
    }

    Expect("restart", Parse("restart\n", c, error) && c.size() == 1 && c[0].kind == Kind::Restart);
    Expect("restart takes nothing", !Parse("restart now\n", c, error));
    Expect("shoot defaults to four angles", Parse("shoot idle\n", c, error) && c[0].angles.size() == 4);
    Expect("unknown command names its line", !Parse("load 1\n\njump\n", c, error) && error.rfind("line 3", 0) == 0);
    Expect("bad pad input is refused", !Parse("pad 10 Z\n", c, error));
    Expect("a stick needs two numbers", !Parse("pad 10 ls=1\n", c, error));
    Expect("load needs a real slot", !Parse("load 9\n", c, error));
    Expect("shoot needs a name", !Parse("shoot dist=3\n", c, error));
    Expect("at most six angles", !Parse("shoot x angles=1,2,3,4,5,6,7\n", c, error));

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
