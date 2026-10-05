// skate-audio: makes SanAnskateas' skateboard sounds, SK8-FM's songs and the
// trick names from the player's own Skate 3 (Xbox 360) game files.
//
//   skate-audio.exe --skate <folder with data\audio> --out <folder> --vgmstream <vgmstream-cli.exe>
//                   [--no-music] [--candidates]
//
// Skate 3 keeps its audio in EA "EB" archives as EA-XMA (Xbox only); the
// free vgmstream decodes that to WAV. Sounds are written as 16-bit WAV
// (loops get a crossfaded seam), songs as AAC (.m4a) through Windows' own
// Media Foundation encoder, and trick names from Skate 3's English text
// table. Nothing from the game is bundled with the mod.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::wstring g_skate, g_out, g_vgm, g_temp;
bool g_music = true;
bool g_candidates = false;

void Say(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
    fflush(stdout);
}

std::string Narrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool WriteFile(const std::wstring& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Be16(const uint8_t* p) {
    return uint16_t(p[0] << 8 | p[1]);
}
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// --------------------------------------------------------------- archives

// EA RefPack (QFS) decompression, used by Skate 3's compressed archive entries.
bool RefPack(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    if (n < 5 || src[1] != 0xFB) return false;
    size_t p = 2;
    int width = (src[0] & 0x80) ? 4 : 3;
    size_t size = 0;
    for (int i = 0; i < width; i++) size = size << 8 | src[p++];
    if (src[0] & 0x01) p += width;
    out.clear();
    out.reserve(size);
    auto literal = [&](size_t count) {
        if (p + count > n) return false;
        out.insert(out.end(), src + p, src + p + count);
        p += count;
        return true;
    };
    while (p < n) {
        uint8_t b0 = src[p];
        size_t lit, len, dist;
        if (b0 < 0x80) {
            if (p + 2 > n) return false;
            uint8_t b1 = src[p + 1];
            p += 2;
            lit = b0 & 3;
            len = ((b0 & 0x1C) >> 2) + 3;
            dist = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            if (p + 3 > n) return false;
            uint8_t b1 = src[p + 1], b2 = src[p + 2];
            p += 3;
            lit = (b1 >> 6) & 3;
            len = (b0 & 0x3F) + 4;
            dist = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            if (p + 4 > n) return false;
            uint8_t b1 = src[p + 1], b2 = src[p + 2], b3 = src[p + 3];
            p += 4;
            lit = b0 & 3;
            len = ((b0 & 0x0C) << 6) + b3 + 5;
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            p++;
            if (!literal(((b0 & 0x1F) << 2) + 4)) return false;
            continue;
        } else {
            p++;
            literal(b0 & 3);
            break;
        }
        if (!literal(lit) || dist > out.size()) return false;
        size_t start = out.size() - dist;
        for (size_t i = 0; i < len; i++) out.push_back(out[start + i]);
    }
    if (out.size() > size) out.resize(size);
    return true;
}

// An EA "EB" v3 archive (Skate 3's .big): the entries' names, offsets and sizes.
struct Archive {
    std::wstring path;
    struct Entry {
        std::string name;
        uint64_t offset;
        uint32_t stored, size; // stored: compressed size, 0 = stored plain
    };
    std::vector<Entry> entries;

    bool Open(const std::wstring& p) {
        path = p;
        std::ifstream f(p, std::ios::binary);
        uint8_t h[0x30];
        if (!f.read(reinterpret_cast<char*>(h), sizeof h)) return false;
        if (h[0] != 'E' || h[1] != 'B' || Be16(h + 2) != 3) return false;
        uint32_t count = Be32(h + 4), namesAt = Be32(h + 12), namesSize = Be32(h + 16);
        int align = h[10], nameLen = h[20];
        std::vector<uint8_t> table(size_t(count) * 16), names(namesSize);
        f.seekg(0x30);
        f.read(reinterpret_cast<char*>(table.data()), table.size());
        f.seekg(namesAt);
        f.read(reinterpret_cast<char*>(names.data()), names.size());
        if (!f || nameLen < 3) return false;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = &table[size_t(i) * 16];
            size_t at = size_t(i) * nameLen + 2;
            if (at + nameLen - 2 > names.size()) return false;
            std::string name(reinterpret_cast<const char*>(&names[at]), strnlen(reinterpret_cast<const char*>(&names[at]), nameLen - 2));
            entries.push_back({name, uint64_t(Be32(e)) << align, Be32(e + 4), Be32(e + 8)});
        }
        return true;
    }

    bool Read(const std::string& name, std::vector<uint8_t>& out) const {
        for (const Entry& e : entries) {
            if (_stricmp(e.name.c_str(), name.c_str()) != 0) continue;
            std::ifstream f(path, std::ios::binary);
            std::vector<uint8_t> raw(e.stored ? e.stored : e.size);
            f.seekg(static_cast<std::streamoff>(e.offset));
            if (!f.read(reinterpret_cast<char*>(raw.data()), raw.size())) return false;
            if (!e.stored) {
                out = std::move(raw);
                return true;
            }
            return RefPack(raw.data(), raw.size(), out);
        }
        return false;
    }
};

// ----------------------------------------------------------------- vgmstream

bool Run(std::wstring cmd) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Decodes `input` (subsong `sub`, 1-based, 0 = the only one) to a 16-bit WAV.
bool Decode(const std::wstring& input, int sub, const std::wstring& wav) {
    DeleteFileW(wav.c_str());
    std::wstring cmd = L"\"" + g_vgm + L"\" -i";
    if (sub > 0) cmd += L" -s " + std::to_wstring(sub);
    cmd += L" -o \"" + wav + L"\" \"" + input + L"\"";
    return Run(cmd) && Exists(wav);
}

// --------------------------------------------------------------------- WAV

struct Wav {
    int rate = 0, channels = 0;
    std::vector<int16_t> samples; // interleaved
};

bool LoadWav(const std::wstring& path, Wav& w) {
    std::vector<uint8_t> d;
    if (!ReadFile(path, d) || d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t p = 12;
    bool fmt = false;
    while (p + 8 <= d.size()) {
        uint32_t size = Le32(&d[p + 4]);
        if (!memcmp(&d[p], "fmt ", 4) && size >= 16) {
            if ((d[p + 8] | d[p + 9] << 8) != 1 || (d[p + 22] | d[p + 23] << 8) != 16) return false; // PCM16 only
            w.channels = d[p + 10] | d[p + 11] << 8;
            w.rate = static_cast<int>(Le32(&d[p + 12]));
            fmt = true;
        } else if (!memcmp(&d[p], "data", 4) && fmt) {
            size_t n = std::min<size_t>(size, d.size() - p - 8) / 2;
            w.samples.resize(n);
            memcpy(w.samples.data(), &d[p + 8], n * 2);
            return w.channels > 0;
        }
        p += 8 + size + (size & 1);
    }
    return false;
}

bool SaveWav(const std::wstring& path, const Wav& w) {
    uint32_t bytes = static_cast<uint32_t>(w.samples.size() * 2);
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0};
    auto put32 = [&](int at, uint32_t v) { memcpy(h + at, &v, 4); };
    auto put16 = [&](int at, uint16_t v) { memcpy(h + at, &v, 2); };
    put32(4, 36 + bytes);
    put16(22, static_cast<uint16_t>(w.channels));
    put32(24, static_cast<uint32_t>(w.rate));
    put32(28, static_cast<uint32_t>(w.rate * w.channels * 2));
    put16(32, static_cast<uint16_t>(w.channels * 2));
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, bytes);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(h), 44);
    f.write(reinterpret_cast<const char*>(w.samples.data()), bytes);
    return static_cast<bool>(f);
}

// Makes a recording loop without a click: its last `fade` seconds are mixed
// into its start (equal power) and cut off.
void LoopSeam(Wav& w, float fade) {
    size_t frames = w.samples.size() / w.channels;
    size_t n = std::min<size_t>(static_cast<size_t>(fade * w.rate), frames / 3);
    if (n < 16) return;
    size_t tail = frames - n;
    for (size_t i = 0; i < n; i++) {
        float t = static_cast<float>(i) / n;
        float in = std::sin(t * 1.5707963f), out = std::cos(t * 1.5707963f);
        for (int c = 0; c < w.channels; c++) {
            float v = w.samples[i * w.channels + c] * in + w.samples[(tail + i) * w.channels + c] * out;
            w.samples[i * w.channels + c] = static_cast<int16_t>(std::clamp(v, -32768.f, 32767.f));
        }
    }
    w.samples.resize(tail * w.channels);
}

// Evens out Skate 3's levels (its banks are mastered for per-patch volumes
// we don't have): one-shots to a fixed peak, loops to a fixed loudness.
void Normalise(Wav& w, bool loop) {
    if (w.samples.empty()) return;
    double sum = 0;
    int peak = 1;
    for (int16_t s : w.samples) {
        sum += double(s) * s;
        peak = std::max(peak, std::abs(int(s)));
    }
    float rms = static_cast<float>(std::sqrt(sum / w.samples.size()));
    float gain = loop ? std::min(0.15f * 32767.f / std::max(rms, 1.f), 0.9f * 32767.f / peak) : 0.8f * 32767.f / peak;
    for (int16_t& s : w.samples) s = static_cast<int16_t>(std::clamp(s * gain, -32768.f, 32767.f));
}

// Trims silence (below about -60 dB) from the end of a one-shot.
void TrimTail(Wav& w) {
    size_t frames = w.samples.size() / w.channels;
    while (frames > 1) {
        bool quiet = true;
        for (int c = 0; c < w.channels; c++) quiet &= std::abs(w.samples[(frames - 1) * w.channels + c]) < 32;
        if (!quiet) break;
        frames--;
    }
    w.samples.resize(frames * w.channels);
}

// ------------------------------------------------------------- sound picks

// Where each skateboard sound comes from in Skate 3's audio files.
enum class Source { Grain, Abk, Splc };
struct Pick {
    const char* name;  // output sfx\<name>.wav
    Source source;
    const char* file;  // grain name, .abk or .bnk inside audiofiles.big / grains.big
    int index;         // .abk subsong (1-based) or .bnk sound (0-based)
    bool loop;
    float seconds = 0; // a loop kept only this long (0: all of it)
};

// Chosen by ear-proxy (spectrograms and envelopes): Skate 3's banks carry no
// names, except the grains (rolling recordings per surface).
const Pick kPicks[] = {
    {"roll_concrete", Source::Grain, "concrete_smooth_hard", 0, true},
    {"roll_concrete_slow", Source::Grain, "concrete_smooth_soft", 0, true},
    {"roll_asphalt", Source::Grain, "asphalt_rough_hard", 0, true},
    {"roll_asphalt_slow", Source::Grain, "asphalt_rough_soft", 0, true},
    {"roll_brick", Source::Grain, "concrete_rough_hard", 0, true},
    {"roll_brick_slow", Source::Grain, "concrete_rough_soft", 0, true},
    {"roll_metal", Source::Grain, "metal_smooth_hard", 0, true},
    {"roll_wood", Source::Grain, "wood_ramp_hard", 0, true},
    {"roll_grass", Source::Abk, "PatchBank_Rolling_Surfaces.abk", 7, true},
    {"seam_1", Source::Abk, "Seams_Bank.abk", 1, false},
    {"seam_2", Source::Abk, "Seams_Bank.abk", 4, false},
    {"seam_3", Source::Abk, "Seams_Bank.abk", 7, false},
    {"seam_4", Source::Abk, "Seams_Bank.abk", 10, false},
    {"seam_5", Source::Abk, "Seams_Bank.abk", 13, false},
    {"seam_6", Source::Abk, "Seams_Bank.abk", 16, false},
    {"pop_1", Source::Splc, "Skate_Collisions.bnk", 556, false},
    {"pop_2", Source::Splc, "Skate_Collisions.bnk", 559, false},
    {"pop_3", Source::Splc, "Skate_Collisions.bnk", 561, false},
    {"pop_4", Source::Splc, "Skate_Collisions.bnk", 562, false},
    {"pop_5", Source::Splc, "Skate_Collisions.bnk", 565, false},
    {"pop_6", Source::Splc, "Skate_Collisions.bnk", 567, false},
    {"flip_1", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 6, false},
    {"flip_2", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 7, false},
    {"flip_3", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 8, false},
    {"flip_4", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 10, false},
    {"flip_5", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 11, false},
    {"flip_6", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 12, false},
    {"flipfast_1", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 17, false},
    {"flipfast_2", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 19, false},
    {"flipfast_3", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 21, false},
    {"flipfast_4", Source::Abk, "Sk8_Air_Flip_Tricks.abk", 23, false},
    {"land_1", Source::Splc, "Skate_Collisions.bnk", 746, false},
    {"land_2", Source::Splc, "Skate_Collisions.bnk", 747, false},
    {"land_3", Source::Splc, "Skate_Collisions.bnk", 748, false},
    {"land_4", Source::Splc, "Skate_Collisions.bnk", 756, false},
    {"land_5", Source::Splc, "Skate_Collisions.bnk", 757, false},
    {"grind_metal_1", Source::Abk, "GRINDS.abk", 41, true},
    {"grind_metal_2", Source::Abk, "GRINDS.abk", 57, true},
    {"grind_metal_3", Source::Abk, "GRINDS.abk", 70, true},
    {"grind_concrete_1", Source::Abk, "GRINDS.abk", 6, true},
    {"grind_concrete_2", Source::Abk, "GRINDS.abk", 21, true},
    {"grind_concrete_3", Source::Abk, "GRINDS.abk", 91, true},
    {"bail_1", Source::Splc, "HOM_Set_1.bnk", 0, false},
    {"bail_2", Source::Splc, "HOM_Set_1.bnk", 1, false},
    {"bail_3", Source::Splc, "HOM_Set_1.bnk", 2, false},
    {"bail_4", Source::Splc, "HOM_Set_1.bnk", 3, false},
    {"bail_5", Source::Splc, "HOM_Set_1.bnk", 4, false},
    {"bail_6", Source::Splc, "HOM_Set_1.bnk", 5, false},
    {"body_1", Source::Splc, "Skate_Collisions.bnk", 768, false},
    {"body_2", Source::Splc, "Skate_Collisions.bnk", 769, false},
    {"body_3", Source::Splc, "Skate_Collisions.bnk", 770, false},
    {"body_4", Source::Splc, "Skate_Collisions.bnk", 771, false},
    {"board_1", Source::Splc, "Skate_Collisions.bnk", 883, false},
    {"board_2", Source::Splc, "Skate_Collisions.bnk", 885, false},
    {"board_3", Source::Splc, "Skate_Collisions.bnk", 887, false},
    {"board_4", Source::Splc, "Skate_Collisions.bnk", 889, false},
    {"bodyslide", Source::Abk, "Bodyslide.abk", 1, true},
    {"combo_x2", Source::Splc, "sk8_menu.bnk", 48, false}, // picked by the user's ear; combo_x3 is built from it (MakeComboX3)
    {"wheels_air", Source::Grain, "", -1, true}, // see Main: wheels.big
    // Feel (2026-10-05), from Skate 3's own sound projects' classes: the
    // sense of speed (SenseOfSpeed_wind/rattle/tone), cloth flapping, cars
    // whooshing by, the pre-landing whoosh, cloth on tricks, the ollie
    // rattle, truck squeaks on carves, the board tumbling after a bail.
    {"speed_wind", Source::Abk, "sense_of_speed.abk", 3, true, 20.f},
    {"speed_rattle", Source::Abk, "sense_of_speed.abk", 2, true, 15.f},
    {"speed_tone", Source::Abk, "sense_of_speed.abk", 6, true, 15.f},
    {"cloth_flap", Source::Abk, "clothes_flap.abk", 1, true, 20.f},
    {"whoosh_1", Source::Splc, "Sk82_Whsh_Bys.bnk", 2, false},
    {"whoosh_2", Source::Splc, "Sk82_Whsh_Bys.bnk", 9, false},
    {"whoosh_3", Source::Splc, "Sk82_Whsh_Bys.bnk", 13, false},
    {"whoosh_4", Source::Splc, "Sk82_Whsh_Bys.bnk", 14, false},
    {"whoosh_5", Source::Splc, "Sk82_Whsh_Bys.bnk", 16, false},
    {"whoosh_6", Source::Splc, "Sk82_Whsh_Bys.bnk", 17, false},
    {"preland_1", Source::Abk, "Treatments.abk", 4, false},
    {"preland_2", Source::Abk, "Treatments.abk", 6, false},
    {"cloth_1", Source::Abk, "Foley_Cloth.abk", 9, false},
    {"cloth_2", Source::Abk, "Foley_Cloth.abk", 10, false},
    {"cloth_3", Source::Abk, "Foley_Cloth.abk", 11, false},
    {"cloth_4", Source::Abk, "Foley_Cloth.abk", 16, false},
    {"cloth_5", Source::Abk, "Foley_Cloth.abk", 20, false},
    {"cloth_6", Source::Abk, "Foley_Cloth.abk", 21, false},
    {"rattle_1", Source::Abk, "sense_of_speed.abk", 8, false},
    {"rattle_2", Source::Abk, "sense_of_speed.abk", 9, false},
    {"rattle_3", Source::Abk, "sense_of_speed.abk", 10, false},
    {"rattle_4", Source::Abk, "sense_of_speed.abk", 11, false},
    {"rattle_5", Source::Abk, "sense_of_speed.abk", 12, false},
    {"squeak_1", Source::Abk, "Brd_Squeaks.abk", 13, false},
    {"squeak_2", Source::Abk, "Brd_Squeaks.abk", 14, false},
    {"squeak_3", Source::Abk, "Brd_Squeaks.abk", 15, false},
    {"squeak_4", Source::Abk, "Brd_Squeaks.abk", 16, false},
    {"squeak_5", Source::Abk, "Brd_Squeaks.abk", 1, false},
    {"squeak_6", Source::Abk, "Brd_Squeaks.abk", 5, false},
    {"tumble_1", Source::Abk, "board_scrapes.abk", 26, false},
    {"tumble_2", Source::Abk, "board_scrapes.abk", 27, false},
    {"tumble_3", Source::Abk, "board_scrapes.abk", 28, false},
    {"tumble_4", Source::Abk, "board_scrapes.abk", 21, false},
    {"tumble_5", Source::Abk, "board_scrapes.abk", 23, false},
    {"tumble_6", Source::Abk, "board_scrapes.abk", 24, false},
};

// Skate 3 plays skids and foot drags as granular sounds: short grains of
// one surface chained live. We chain a run of them once into a loop.
struct GrainLoop {
    const char* name; // output sfx\<name>.wav
    const char* file; // .abk inside audiofiles.big
    int first, last;  // subsongs (1-based), chained in order
};
const GrainLoop kGrainLoops[] = {
    {"powerslide", "WHEEL_SKID_BANK.abk", 41, 48},
    {"footbrake", "FOOT_DRAG.abk", 97, 108},
};

// SPLC banks (Skate_Collisions.bnk...): EA-AC sounds stored back to back
// after the patch tables. Returns each sound's bytes, in bank order, as a
// standalone RAM .snr vgmstream reads.
bool SplitSplc(const std::vector<uint8_t>& d, std::vector<std::pair<size_t, size_t>>& sounds) {
    if (d.size() < 0x40 || memcmp(d.data(), "SPLC", 4)) return false;
    uint32_t expected = Be32(&d[0x18]);
    static const uint32_t rates[] = {48000, 44100, 32000, 24000, 22050, 16000, 11025};
    for (size_t i = 0x40; i + 16 <= d.size();) {
        uint32_t h1 = Be32(&d[i]), h2 = Be32(&d[i + 4]);
        uint32_t rate = h1 & 0x3FFFF, samples = h2 & 0x1FFFFFFF;
        bool loop = (h2 >> 29) & 1;
        bool header = (h1 >> 28) == 0 && ((h1 >> 24) & 0xF) == 3 && ((h1 >> 18) & 0x3F) < 2 &&
                      std::find(std::begin(rates), std::end(rates), rate) != std::end(rates) && (h2 >> 30) == 0 &&
                      samples > 0 && samples < 48000u * 120;
        size_t blocks = i + 8 + (loop ? 4 : 0);
        if (header && blocks + 8 <= d.size()) {
            uint32_t bh = Be32(&d[blocks]);
            uint32_t flag = bh >> 24, size = bh & 0xFFFFFF;
            if ((flag == 0 || flag == 0x80) && size > 8 && blocks + size <= d.size() && Be32(&d[blocks + 4]) <= samples) {
                size_t p = blocks;
                while (p + 8 <= d.size()) { // blocks: flag 0x00, the last 0x80
                    uint32_t b = Be32(&d[p]);
                    uint32_t bs = b & 0xFFFFFF;
                    if ((b >> 24 != 0 && b >> 24 != 0x80) || bs < 8 || p + bs > d.size()) break;
                    p += bs;
                    if (b >> 24 == 0x80) break;
                }
                sounds.emplace_back(i, p - i);
                i += 8;
                continue;
            }
        }
        i++;
    }
    return sounds.size() == expected;
}

bool DecodeSplcSound(const std::vector<uint8_t>& bank, const std::pair<size_t, size_t>& sound, const std::wstring& tmp, Wav& w) {
    std::wstring snr = tmp + L".snr", wav = tmp + L".wav";
    if (!WriteFile(snr, bank.data() + sound.first, sound.second) || !Decode(snr, 0, wav) || !LoadWav(wav, w)) return false;
    DeleteFileW(wav.c_str());
    return true;
}

// Plays a sound back `semitones` higher, as a sampler would (so shorter too).
Wav Repitch(const Wav& w, float semitones) {
    Wav out = w;
    double rate = std::pow(2.0, semitones / 12.0);
    size_t frames = w.samples.size() / w.channels, n = static_cast<size_t>(frames / rate);
    out.samples.assign(n * w.channels, 0);
    for (size_t i = 0; i < n; i++) {
        double at = i * rate;
        size_t k = static_cast<size_t>(at);
        double t = at - k;
        for (int c = 0; c < w.channels; c++) {
            double a = w.samples[std::min(k, frames - 1) * w.channels + c], b = w.samples[std::min(k + 1, frames - 1) * w.channels + c];
            out.samples[i * w.channels + c] = static_cast<int16_t>(a + (b - a) * t);
        }
    }
    return out;
}

// Skate 3's x3 combo sound as the user matched it by ear against the game:
// menu sounds 48 (the x2 sound) and 49 layered, 3 semitones up.
bool MakeComboX3(const std::vector<uint8_t>& menu, const std::vector<std::pair<size_t, size_t>>& sounds, const std::wstring& sfx) {
    Wav a, b;
    if (sounds.size() <= 49 || !DecodeSplcSound(menu, sounds[48], g_temp + L"\\combo_a", a) ||
        !DecodeSplcSound(menu, sounds[49], g_temp + L"\\combo_b", b) || a.rate != b.rate || a.channels != b.channels) {
        Say("  combo_x3: cannot decode sk8_menu.bnk sounds 48 and 49");
        return false;
    }
    a = Repitch(a, 3.f);
    b = Repitch(b, 3.f);
    std::vector<float> mix(std::max(a.samples.size(), b.samples.size()), 0.f);
    for (size_t i = 0; i < a.samples.size(); i++) mix[i] += a.samples[i];
    for (size_t i = 0; i < b.samples.size(); i++) mix[i] += b.samples[i];
    float peak = 1.f;
    for (float v : mix) peak = std::max(peak, std::fabs(v));
    Wav x3 = a;
    x3.samples.resize(mix.size());
    for (size_t i = 0; i < mix.size(); i++) x3.samples[i] = static_cast<int16_t>(mix[i] * (0.8f * 32767.f / peak));
    TrimTail(x3);
    return SaveWav(sfx + L"\\combo_x3.wav", x3);
}

bool MakeSounds(const std::wstring& sfx) {
    std::wstring audio = g_skate + L"\\data\\audio\\";
    Archive files, grains, wheels;
    if (!files.Open(audio + L"audiofiles.big") || !grains.Open(audio + L"grains.big") || !wheels.Open(audio + L"wheels.big")) {
        Say("ERROR: cannot read Skate 3's audio archives in %s", Narrow(audio).c_str());
        return false;
    }
    std::map<std::string, std::wstring> extracted;           // bank name -> temp file
    std::map<std::string, std::vector<uint8_t>> splcData;   // SPLC banks, loaded
    std::map<std::string, std::vector<std::pair<size_t, size_t>>> splcSounds;
    int made = 0, failed = 0;
    for (const Pick& pick : kPicks) {
        std::wstring input;
        int sub = 0;
        std::wstring tmp = g_temp + L"\\" + Wide(pick.name);
        if (pick.source == Source::Grain) {
            std::vector<uint8_t> data;
            bool ok = pick.index < 0 ? wheels.Read("Whls_spins_Jump_1.snr", data)
                                     : grains.Read(std::string(pick.file) + ".grain", data);
            if (ok && pick.index >= 0) { // a grain: a seek table, then a RAM .snr
                uint32_t at = data.size() >= 4 ? Be32(data.data()) : 0;
                ok = at > 0 && at < data.size();
                if (ok) data.erase(data.begin(), data.begin() + at);
            }
            input = tmp + L".snr";
            if (!ok || !WriteFile(input, data.data(), data.size())) {
                Say("  %s: missing from Skate 3's files", pick.name);
                failed++;
                continue;
            }
        } else if (pick.source == Source::Abk) {
            auto it = extracted.find(pick.file);
            if (it == extracted.end()) {
                std::vector<uint8_t> data;
                std::wstring path = g_temp + L"\\" + Wide(pick.file);
                if (!files.Read(pick.file, data) || !WriteFile(path, data.data(), data.size())) {
                    Say("  %s: %s missing from audiofiles.big", pick.name, pick.file);
                    failed++;
                    continue;
                }
                it = extracted.emplace(pick.file, path).first;
            }
            input = it->second;
            sub = pick.index;
        } else {
            if (!splcData.count(pick.file)) {
                std::vector<uint8_t> data;
                std::vector<std::pair<size_t, size_t>> sounds;
                if (!files.Read(pick.file, data) || !SplitSplc(data, sounds)) {
                    Say("  %s: cannot split %s", pick.name, pick.file);
                    failed++;
                    continue;
                }
                splcData[pick.file] = std::move(data);
                splcSounds[pick.file] = std::move(sounds);
            }
            const auto& sounds = splcSounds[pick.file];
            if (pick.index < 0 || pick.index >= static_cast<int>(sounds.size())) {
                failed++;
                continue;
            }
            input = tmp + L".snr";
            const auto& s = sounds[pick.index];
            WriteFile(input, splcData[pick.file].data() + s.first, s.second);
        }
        Wav w;
        std::wstring wav = tmp + L".wav";
        if (!Decode(input, sub, wav) || !LoadWav(wav, w)) {
            Say("  %s: vgmstream could not decode it", pick.name);
            failed++;
            continue;
        }
        if (w.channels > 2) { // some banks hold quad sounds: mix them down to mono
            size_t frames = w.samples.size() / w.channels;
            std::vector<int16_t> mono(frames);
            for (size_t i = 0; i < frames; i++) {
                int sum = 0;
                for (int c = 0; c < w.channels; c++) sum += w.samples[i * w.channels + c];
                mono[i] = static_cast<int16_t>(sum / w.channels);
            }
            w.samples = std::move(mono);
            w.channels = 1;
        }
        if (pick.seconds > 0.f) {
            size_t keep = static_cast<size_t>((pick.seconds + 0.25f) * w.rate) * w.channels; // the seam eats 0.25 s
            if (w.samples.size() > keep) w.samples.resize(keep);
        }
        if (pick.loop) LoopSeam(w, 0.25f);
        else TrimTail(w);
        Normalise(w, pick.loop);
        if (!SaveWav(sfx + L"\\" + Wide(pick.name) + L".wav", w)) {
            failed++;
            continue;
        }
        made++;
        DeleteFileW(wav.c_str());
    }
    if (splcData.count("sk8_menu.bnk")) {
        if (MakeComboX3(splcData["sk8_menu.bnk"], splcSounds["sk8_menu.bnk"], sfx)) made++;
        else failed++;
    }
    // Granular loops: the grains chained with short crossfades, then looped.
    for (const GrainLoop& g : kGrainLoops) {
        std::vector<uint8_t> data;
        std::wstring path = g_temp + L"\\" + Wide(g.file), wav = g_temp + L"\\grain.wav";
        if (!files.Read(g.file, data) || !WriteFile(path, data.data(), data.size())) {
            Say("  %s: %s missing from audiofiles.big", g.name, g.file);
            failed++;
            continue;
        }
        Wav loop;
        bool ok = true;
        for (int sub = g.first; ok && sub <= g.last; sub++) {
            Wav grain;
            ok = Decode(path, sub, wav) && LoadWav(wav, grain) && (loop.samples.empty() || (grain.rate == loop.rate && grain.channels == loop.channels));
            if (!ok) break;
            if (loop.samples.empty()) {
                loop = grain;
                continue;
            }
            size_t fade = std::min<size_t>(static_cast<size_t>(0.015f * grain.rate), grain.samples.size() / grain.channels / 3) * grain.channels;
            fade = std::min(fade, loop.samples.size());
            size_t at = loop.samples.size() - fade;
            for (size_t i = 0; i < fade; i++) {
                float t = static_cast<float>(i / grain.channels) / (fade / grain.channels);
                float v = loop.samples[at + i] * (1.f - t) + grain.samples[i] * t;
                loop.samples[at + i] = static_cast<int16_t>(std::clamp(v, -32768.f, 32767.f));
            }
            loop.samples.insert(loop.samples.end(), grain.samples.begin() + fade, grain.samples.end());
        }
        DeleteFileW(wav.c_str());
        if (!ok || loop.samples.empty()) {
            Say("  %s: vgmstream could not decode its grains", g.name);
            failed++;
            continue;
        }
        LoopSeam(loop, 0.05f);
        Normalise(loop, true);
        if (SaveWav(sfx + L"\\" + Wide(g.name) + L".wav", loop)) made++;
        else failed++;
    }
    Say("Skateboard sounds: %d made, %d failed", made, failed);
    return made > 0;
}

// Every sound of the banks the picks come from, for choosing by ear:
// <out>\candidates\<bank>\NNNN.wav.
void MakeCandidates() {
    std::wstring audio = g_skate + L"\\data\\audio\\";
    Archive files;
    if (!files.Open(audio + L"audiofiles.big")) return;
    std::wstring root = g_out + L"\\candidates";
    CreateDirectoryW(root.c_str(), nullptr);
    for (const char* bank : {"Skate_Collisions.bnk", "HOM_Set_1.bnk", "HOM_Set_2.bnk", "sk8_foley.bnk"}) {
        std::vector<uint8_t> data;
        std::vector<std::pair<size_t, size_t>> sounds;
        if (!files.Read(bank, data) || !SplitSplc(data, sounds)) continue;
        std::wstring dir = root + L"\\" + Wide(bank);
        CreateDirectoryW(dir.c_str(), nullptr);
        for (size_t i = 0; i < sounds.size(); i++) {
            wchar_t name[16];
            swprintf(name, 16, L"%04zu", i);
            std::wstring snr = g_temp + L"\\cand.snr";
            WriteFile(snr, data.data() + sounds[i].first, sounds[i].second);
            Decode(snr, 0, dir + L"\\" + name + L".wav");
        }
        Say("Candidates: %zu sounds from %s", sounds.size(), bank);
    }
}

// --------------------------------------------------------------- trick names

// EA's string hash (bStringHash), the key of Skate 3's text table.
uint32_t Hash(const char* s) {
    uint32_t h = 0xFFFFFFFF;
    for (; *s; s++) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}

// Skate 3's English text table (miscboot.big): hash -> text, for the short
// strings trick names are. Written as "hhhhhhhh<TAB>text" lines.
bool MakeTrickNames(const std::wstring& path) {
    Archive boot;
    std::vector<uint8_t> d;
    if (!boot.Open(g_skate + L"\\data\\big\\miscboot.big") || !boot.Read("LANGUAGE_English_Global_skate3ng.BIN", d) ||
        d.size() < 0x24) {
        Say("WARNING: Skate 3's English text wasn't found; tricks show their internal names");
        return false;
    }
    uint32_t count = Le32(&d[8]), table = Le32(&d[12]) + 8, text = Le32(&d[16]) + 8;
    std::string out;
    int kept = 0;
    for (uint32_t i = 0; i < count && table + i * 8 + 8 <= d.size(); i++) {
        uint32_t h = Le32(&d[table + i * 8]), at = text + Le32(&d[table + i * 8 + 4]);
        if (at >= d.size()) continue;
        const char* s = reinterpret_cast<const char*>(&d[at]);
        size_t n = strnlen(s, d.size() - at);
        if (n == 0 || n > 48) continue;
        bool plain = true;
        for (size_t k = 0; k < n; k++) plain &= s[k] >= 0x20 && s[k] < 0x7F;
        if (!plain) continue;
        char key[16];
        snprintf(key, sizeof key, "%08x\t", h);
        out += key;
        out.append(s, n);
        out += '\n';
        kept++;
    }
    WriteFile(path, out.data(), out.size());
    Say("Trick names: %d texts", kept);
    return kept > 0;
}

// --------------------------------------------------------------------- SK8-FM

// Encodes a 16-bit PCM WAV to AAC in an .m4a with Windows' Media Foundation.
bool EncodeAac(const Wav& w, const std::wstring& path) {
    IMFSinkWriter* writer = nullptr;
    IMFMediaType *outType = nullptr, *inType = nullptr;
    DWORD stream = 0;
    bool ok = false;
    DeleteFileW(path.c_str());
    if (SUCCEEDED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer)) && SUCCEEDED(MFCreateMediaType(&outType)) &&
        SUCCEEDED(MFCreateMediaType(&inType))) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, w.rate);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, w.channels);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, w.rate);
        inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, w.channels);
        inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, w.channels * 2);
        inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, w.rate * w.channels * 2);
        if (SUCCEEDED(writer->AddStream(outType, &stream)) && SUCCEEDED(writer->SetInputMediaType(stream, inType, nullptr)) &&
            SUCCEEDED(writer->BeginWriting())) {
            ok = true;
            const size_t chunk = static_cast<size_t>(w.rate) * w.channels; // one second
            for (size_t at = 0; ok && at < w.samples.size(); at += chunk) {
                size_t n = std::min(chunk, w.samples.size() - at);
                IMFMediaBuffer* buffer = nullptr;
                IMFSample* sample = nullptr;
                BYTE* dst = nullptr;
                ok = SUCCEEDED(MFCreateMemoryBuffer(static_cast<DWORD>(n * 2), &buffer)) && SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr));
                if (ok) {
                    memcpy(dst, &w.samples[at], n * 2);
                    buffer->Unlock();
                    buffer->SetCurrentLength(static_cast<DWORD>(n * 2));
                    LONGLONG start = static_cast<LONGLONG>(at / w.channels) * 10000000LL / w.rate;
                    LONGLONG length = static_cast<LONGLONG>(n / w.channels) * 10000000LL / w.rate;
                    ok = SUCCEEDED(MFCreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer)) &&
                         SUCCEEDED(sample->SetSampleTime(start)) && SUCCEEDED(sample->SetSampleDuration(length)) &&
                         SUCCEEDED(writer->WriteSample(stream, sample));
                }
                if (sample) sample->Release();
                if (buffer) buffer->Release();
            }
            ok = ok && SUCCEEDED(writer->Finalize());
        }
    }
    if (inType) inType->Release();
    if (outType) outType->Release();
    if (writer) writer->Release();
    return ok;
}

// The iPod set (ipod.mpf + Ipod_Stream.mus) is Skate 3's licensed soundtrack:
// each song is a run of samples sharing one tag in the MPF's sample table.
bool MakeSongs(const std::wstring& dir) {
    std::wstring mpf = g_skate + L"\\data\\audio\\music\\ipod.mpf";
    std::vector<uint8_t> d;
    if (!ReadFile(mpf, d) || d.size() < 0x40 || memcmp(d.data(), "PFDx", 4) || !Exists(g_skate + L"\\data\\audio\\music\\Ipod_Stream.mus")) {
        Say("WARNING: Skate 3's soundtrack (data\\audio\\music\\ipod.mpf) wasn't found; SK8-FM stays off");
        return false;
    }
    uint32_t table = Be32(&d[0x34]), end = Be32(&d[0x38]);
    if (end > d.size() || table >= end) return false;
    std::vector<std::pair<uint32_t, int>> songs; // first sample, sample count
    uint32_t lastTag = ~0u;
    for (uint32_t i = 0; table + i * 8 + 8 <= end; i++) {
        uint32_t tag = Be32(&d[table + i * 8 + 4]);
        if (songs.empty() || tag != lastTag) songs.push_back({i, 0});
        songs.back().second++;
        lastTag = tag;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        Say("WARNING: Windows Media Foundation is missing (Windows N? install the Media Feature Pack); SK8-FM stays off");
        return false;
    }
    int made = 0;
    for (size_t k = 0; k < songs.size(); k++) {
        wchar_t name[32];
        swprintf(name, 32, L"%02zu.m4a", k + 1);
        std::wstring out = dir + L"\\" + name;
        if (Exists(out)) {
            made++;
            continue;
        }
        std::string txtp;
        for (int s = 0; s < songs[k].second; s++) txtp += Narrow(mpf) + "#" + std::to_string(songs[k].first + s + 1) + "\n";
        std::wstring list = g_temp + L"\\song.txtp", wav = g_temp + L"\\song.wav";
        Wav w;
        WriteFile(list, txtp.data(), txtp.size());
        if (!Decode(list, 0, wav) || !LoadWav(wav, w) || !EncodeAac(w, out)) {
            Say("  song %zu: could not convert", k + 1);
            DeleteFileW(out.c_str());
            continue;
        }
        DeleteFileW(wav.c_str());
        made++;
        Say("SK8-FM: song %zu of %zu (%d:%02d)", k + 1, songs.size(), static_cast<int>(w.samples.size() / w.channels / w.rate) / 60,
            static_cast<int>(w.samples.size() / w.channels / w.rate) % 60);
    }
    MFShutdown();
    Say("SK8-FM: %d songs ready", made);
    return made > 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--skate" && i + 1 < argc) g_skate = argv[++i];
        else if (a == L"--out" && i + 1 < argc) g_out = argv[++i];
        else if (a == L"--vgmstream" && i + 1 < argc) g_vgm = argv[++i];
        else if (a == L"--no-music") g_music = false;
        else if (a == L"--candidates") g_candidates = true;
    }
    if (g_skate.empty() || g_out.empty() || g_vgm.empty()) {
        Say("usage: skate-audio --skate <Skate 3 folder> --out <folder> --vgmstream <vgmstream-cli.exe> [--no-music]");
        return 2;
    }
    while (!g_skate.empty() && (g_skate.back() == L'\\' || g_skate.back() == L'/')) g_skate.pop_back();
    if (!Exists(g_vgm)) {
        Say("ERROR: vgmstream-cli.exe not found at %s", Narrow(g_vgm).c_str());
        return 2;
    }
    if (!Exists(g_skate + L"\\data\\audio\\audiofiles.big")) {
        Say("ERROR: %s has no data\\audio\\audiofiles.big (pick the folder with Skate 3's default.xex)", Narrow(g_skate).c_str());
        return 2;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_temp = std::wstring(tmp) + L"SanAnskateas-audio";
    CreateDirectoryW(g_temp.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring sfx = g_out + L"\\sfx", fm = g_out + L"\\sk8fm";
    CreateDirectoryW(g_out.c_str(), nullptr);
    CreateDirectoryW(sfx.c_str(), nullptr);
    CreateDirectoryW(fm.c_str(), nullptr);
    bool sounds = MakeSounds(sfx);
    MakeTrickNames(g_out + L"\\trick-names.txt");
    if (g_candidates) MakeCandidates();
    if (g_music) MakeSongs(fm);
    CoUninitialize();
    Say(sounds ? "Skate 3 audio ready" : "ERROR: no skateboard sounds could be made");
    return sounds ? 0 : 1;
}
