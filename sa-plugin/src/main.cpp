// SanAnskateas: Skate 3 skating in GTA San Andreas, driven by the Skate 3
// Rust engine through skate_ffi.dll.
//
// Frame order (CGame::Process): pads update -> scripts -> [processScriptsEvent:
// we block the pad, step Skate and place CJ] -> CWorld::Process (physics,
// with CJ frozen) -> camera -> [gameProcessEvent: we re-place CJ so nothing
// the frame did can drift him] -> render.
#include "plugin.h"
#include "common.h"
#include "CAERadioTrackManager.h"
#include "CAudioEngine.h"
#include "CCamera.h"
#include "CCarCtrl.h"
#include "CClock.h"
#include "CColPoint.h"
#include "CCutsceneMgr.h"
#include "CDraw.h"
#include "CEntryExitManager.h"
#include "CFont.h"
#include "CGame.h"
#include "CHud.h"
#include "CMenuManager.h"
#include "CPad.h"
#include "CSprite2d.h"
#include "CBaseModelInfo.h"
#include "CColModel.h"
#include "CModelInfo.h"
#include "CAutomobile.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CPopulation.h"
#include "CPtrNodeDoubleLink.h"
#include "CPtrNodeSingleLink.h"
#include "CRepeatSector.h"
#include "CSector.h"
#include "CStreaming.h"
#include "CTimeCycle.h"
#include "CTimer.h"
#include "CVehicle.h"
#include "CWanted.h"
#include "CWaterLevel.h"
#include "CWeather.h"
#include "CWorld.h"
#include "eSurfaceType.h"

#include "capture.h"
#include "skate_api.h"
#include "sound.h"
#include "settings_menu.h"
#include "stick_combo.h"
#include "test_script.h"

#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

using namespace plugin;

namespace {

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------- logging

std::wstring g_dir; // folder holding this .asi
std::mutex g_logMutex;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    std::lock_guard lock(g_logMutex);
    if (!g_log) {
        g_log = _wfopen((g_dir + L"\\SanAnskateas.log").c_str(), L"w");
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ----------------------------------------------------------------- config

struct Config {
    std::wstring assetsDir;  // the converted skate-data\assets folder
    int toggleKey = 'J';
    bool padToggle = true;   // L3 + R3 also gets on and off
    ULONGLONG comboWindowMs = 100; // how long a lone L3/R3 waits for the other stick
    float feetOffset = 1.0f; // a ped's position sits this far above its feet
    float floorSize = 400.f; // fallback floor when no collision is loaded, in metres
    float worldRadius = 60.f;       // San Andreas collision handed to Skate around the skater
    float worldRebuild = 20.f;      // rebuild once the skater gets this far from the last centre
    float worldRefresh = 4.f;       // and at least this often (s), so moved cars and objects update
    bool parkedCarsCollide = true;
    bool animate = true;     // CJ's skeleton follows the skater
    float rigFacing = NAN;   // turns CJ's model onto the skater's; NaN = measure it
    float elbowFollow = 0.5f;    // 0: CJ's elbows hang naturally, 1: point like the skater's
    float shoulderFollow = 0.5f; // 0: shoulders ride the chest, 1: move like the skater's
    std::string difficulty = "easy"; // Skate physics: easy, normal or hardcore (menu: Easy/Normal/Hard)
    bool lowCamera = false;    // Skate 3's Low ("OG") camera instead of High
    int trucks = 5;            // truck tightness, bar 1 (loosest) to kTruckBars (tightest)
    bool skateCamera = true;   // Skate 3's own camera while skating
    float cameraBlendIn = 0.5f, cameraBlendOut = 0.5f; // seconds
    float exitAirPitch = 15.f; // getting off mid-air tilts the game camera down this much
    bool carryMomentum = true; // CJ's velocity onto the board and back
    float hopSpeed = 2.5f;   // getting off on the ground faster than this (m/s) hops off
    float hopUp = 3.0f;      // upward speed of that hop (m/s)
    float boardBrightness = 0.45f; // the board's share of the game's lights, to match CJ
    std::wstring audioDir;     // Skate 3 sounds, SK8-FM songs and trick names, made by Setup
    bool sounds = true;        // skateboard sounds
    float soundVolume = 1.f;   // on top of the game's SFX volume
    bool hud = true;           // Skate 3's score HUD while skating
    bool radio = true;         // the radio plays while skating; LB + D-pad left/right changes it
    std::wstring radioStation; // the last one picked: sk8fm, off, or a GTA station number
} g_cfg;

float IniFloat(const wchar_t* ini, const wchar_t* key, float fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Skate", key, L"", buf, 64, ini);
    wchar_t* end = nullptr;
    float v = wcstof(buf, &end);
    return end != buf && std::isfinite(v) ? v : fallback;
}

void LoadConfig() {
    std::wstring ini = g_dir + L"\\SanAnskateas.ini";
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"Skate", L"AssetsDir", L"SanAnskateas\\skate-data\\assets", buf, MAX_PATH, ini.c_str());
    g_cfg.assetsDir = buf;
    if (g_cfg.assetsDir.size() < 2 || g_cfg.assetsDir[1] != L':') {
        g_cfg.assetsDir = g_dir + L"\\" + g_cfg.assetsDir; // relative to the game folder
    }
    g_cfg.toggleKey = GetPrivateProfileIntW(L"Skate", L"ToggleKey", 'J', ini.c_str());
    g_cfg.padToggle = GetPrivateProfileIntW(L"Skate", L"PadToggle", 1, ini.c_str()) != 0;
    float window = IniFloat(ini.c_str(), L"ComboWindow", 0.1f);
    g_cfg.comboWindowMs = static_cast<ULONGLONG>(std::fmin(std::fmax(window, 0.f), 2.f) * 1000.f);
    g_cfg.feetOffset = IniFloat(ini.c_str(), L"FeetOffset", 1.0f);
    GetPrivateProfileStringW(L"Skate", L"Difficulty", L"easy", buf, MAX_PATH, ini.c_str());
    g_cfg.difficulty = Utf8(buf);
    if (g_cfg.difficulty == "hard") g_cfg.difficulty = "hardcore";
    if (g_cfg.difficulty != "easy" && g_cfg.difficulty != "normal" && g_cfg.difficulty != "hardcore") g_cfg.difficulty = "easy";
    GetPrivateProfileStringW(L"Skate", L"Camera", L"high", buf, MAX_PATH, ini.c_str());
    g_cfg.lowCamera = _wcsicmp(buf, L"low") == 0;
    g_cfg.trucks = std::min(std::max(static_cast<int>(GetPrivateProfileIntW(L"Skate", L"Trucks", 5, ini.c_str())), 1), 10);
    g_cfg.skateCamera = GetPrivateProfileIntW(L"Skate", L"SkateCamera", 1, ini.c_str()) != 0;
    g_cfg.cameraBlendIn = std::fmax(IniFloat(ini.c_str(), L"CameraBlendIn", 0.5f), 0.f);
    g_cfg.cameraBlendOut = std::fmax(IniFloat(ini.c_str(), L"CameraBlendOut", 0.5f), 0.f);
    g_cfg.exitAirPitch = IniFloat(ini.c_str(), L"ExitAirCameraPitch", 15.f);
    g_cfg.carryMomentum = GetPrivateProfileIntW(L"Skate", L"CarryMomentum", 1, ini.c_str()) != 0;
    g_cfg.hopSpeed = IniFloat(ini.c_str(), L"DismountHopSpeed", 2.5f);
    g_cfg.hopUp = IniFloat(ini.c_str(), L"DismountHop", 3.0f);
    g_cfg.floorSize = IniFloat(ini.c_str(), L"TestFloorSize", 400.f);
    g_cfg.worldRadius = std::fmin(std::fmax(IniFloat(ini.c_str(), L"CollisionRadius", 60.f), 25.f), 150.f);
    g_cfg.worldRebuild = std::fmin(std::fmax(IniFloat(ini.c_str(), L"CollisionRebuildDistance", 20.f), 5.f), g_cfg.worldRadius * 0.5f);
    g_cfg.worldRefresh = std::fmax(IniFloat(ini.c_str(), L"CollisionRefreshSeconds", 4.f), 1.f);
    g_cfg.parkedCarsCollide = GetPrivateProfileIntW(L"Skate", L"ParkedCarsCollide", 1, ini.c_str()) != 0;
    g_cfg.animate = GetPrivateProfileIntW(L"Skate", L"AnimateCJ", 1, ini.c_str()) != 0;
    g_cfg.rigFacing = IniFloat(ini.c_str(), L"RigFacingDegrees", NAN) * kPi / 180.f; // "auto" stays NaN
    g_cfg.elbowFollow = IniFloat(ini.c_str(), L"ArmElbowFollow", 0.5f);
    g_cfg.shoulderFollow = IniFloat(ini.c_str(), L"ShoulderFollow", 0.5f);
    g_cfg.boardBrightness = std::fmax(0.f, IniFloat(ini.c_str(), L"BoardBrightness", 0.45f));
    GetPrivateProfileStringW(L"Skate", L"AudioDir", L"SanAnskateas\\skate-audio", buf, MAX_PATH, ini.c_str());
    g_cfg.audioDir = buf;
    if (g_cfg.audioDir.size() < 2 || g_cfg.audioDir[1] != L':') g_cfg.audioDir = g_dir + L"\\" + g_cfg.audioDir;
    g_cfg.sounds = GetPrivateProfileIntW(L"Skate", L"Sounds", 1, ini.c_str()) != 0;
    g_cfg.soundVolume = std::fmin(std::fmax(IniFloat(ini.c_str(), L"SoundVolume", 1.f), 0.f), 2.f);
    g_cfg.hud = GetPrivateProfileIntW(L"Skate", L"ScoreHud", 1, ini.c_str()) != 0;
    g_cfg.radio = GetPrivateProfileIntW(L"Skate", L"SkateRadio", 1, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"Skate", L"RadioStation", L"sk8fm", buf, MAX_PATH, ini.c_str());
    g_cfg.radioStation = buf;
}

// ---------------------------------------------------------- engine loading

enum class Load { NotStarted, Loading, Ready, Failed };
std::atomic<Load> g_load{Load::NotStarted};
SkateApi g_api;
SkSession* g_session = nullptr; // set by the loader before Ready; game thread only after
std::string g_loadError;        // set by the loader before Failed

// ------------------------------------------------------------ settings menu

// Options > SAN ANSKATEAS in GTA's pause menu (settings_menu.h): Skate 3's
// difficulty, its camera (High, or Low: the older games' "OG" camera) and
// its truck tightness. They apply the moment they change and are kept in
// SanAnskateas.ini.
constexpr int kTruckBars = 10;
const char* const kDifficultyNames[] = {"EASY", "NORMAL", "HARD"};
const char* const kDifficultyKeys[] = {"easy", "normal", "hardcore"};
const char* const kCameraNames[] = {"HIGH", "LOW"};
settings_menu::Option g_menuOptions[] = {
    {"SKT_DIF", "DIFFICULTY", 3, false, kDifficultyNames, 0},
    {"SKT_CAM", "CAMERA", 2, false, kCameraNames, 0},
    {"SKT_TRK", "TRUCKS", kTruckBars, true, nullptr, 4},
};
constexpr int kMenuOptions = sizeof(g_menuOptions) / sizeof(g_menuOptions[0]);

// Bar 1 is Skate 3's loosest trucks (0), the last its tightest (1).
float TruckTightness(int bar) { return static_cast<float>(bar - 1) / (kTruckBars - 1); }

// The options into the engine (after it loads, and as they change).
void ApplySkateOptions() {
    if (!g_session) return;
    g_api.set_difficulty(g_session, g_cfg.difficulty.c_str());
    g_api.set_camera(g_session, g_cfg.lowCamera ? 1 : 0);
    g_api.set_trucks(g_session, TruckTightness(g_cfg.trucks));
}

void SyncMenuOptions() {
    for (int i = 0; i < 3; i++) {
        if (g_cfg.difficulty == kDifficultyKeys[i]) g_menuOptions[0].value = i;
    }
    g_menuOptions[1].value = g_cfg.lowCamera ? 1 : 0;
    g_menuOptions[2].value = g_cfg.trucks - 1;
}

void OnMenuOption(int option, int value) {
    std::wstring ini = g_dir + L"\\SanAnskateas.ini";
    switch (option) {
    case 0:
        g_cfg.difficulty = kDifficultyKeys[value];
        WritePrivateProfileStringW(L"Skate", L"Difficulty", std::wstring(g_cfg.difficulty.begin(), g_cfg.difficulty.end()).c_str(), ini.c_str());
        break;
    case 1:
        g_cfg.lowCamera = value == 1;
        WritePrivateProfileStringW(L"Skate", L"Camera", g_cfg.lowCamera ? L"low" : L"high", ini.c_str());
        break;
    case 2:
        g_cfg.trucks = value + 1;
        WritePrivateProfileStringW(L"Skate", L"Trucks", std::to_wstring(g_cfg.trucks).c_str(), ini.c_str());
        break;
    default:
        return;
    }
    if (g_load.load(std::memory_order_acquire) == Load::Ready) ApplySkateOptions(); // else applied once it loads
    Log("Settings: difficulty %s, camera %s, trucks %d of %d", g_cfg.difficulty.c_str(), g_cfg.lowCamera ? "low" : "high", g_cfg.trucks,
        kTruckBars);
}

void LoadEngine() {
    ULONGLONG start = GetTickCount64();
    std::string assets = Utf8(g_cfg.assetsDir);
    Log("Loading the Skate engine from %s", assets.c_str());
    std::string error = g_api.Load(g_dir + L"\\SanAnskateas\\skate_ffi.dll");
    if (error.empty()) {
        // Far below the map; real collision is installed when skating starts.
        float placeholder[9] = {0, 0, -5000, 1, 0, -5000, 0, 1, -5000};
        Log("Skate physics difficulty: %s", g_cfg.difficulty.c_str());
        g_session = g_api.session_new_mode(assets.c_str(), placeholder, 1, nullptr, nullptr, 0, 1.0f,
                                           g_cfg.difficulty.c_str());
        if (!g_session) error = g_api.last_error();
        ApplySkateOptions();
    }
    if (!error.empty()) {
        Log("Skate engine failed to load: %s", error.c_str());
        g_loadError = error;
        g_load.store(Load::Failed, std::memory_order_release);
        return;
    }
    Log("Skate engine ready in %llu ms (step period %.4f s)", GetTickCount64() - start, g_api.period(g_session));
    g_load.store(Load::Ready, std::memory_order_release);
}

// ----------------------------------------------------------------- skating

struct Skate {
    bool active = false;
    CPlayerPed* ped = nullptr;
    bool savedDisableCollisionForce = false;
    bool savedDontApplySpeed = false;
    CVector placed;           // where we last put CJ
    CVector floorCentre;
    SkPose pose{};
    ULONGLONG nextStatsLog = 0;
    ULONGLONG padCheckAt = 0; // when to warn if Skate still sees no controller
    bool padChecked = false;
    ULONGLONG hitsFrom = 0;   // car hits are ignored until then (see CheckCarHits)
    int hits = 0;             // car hits so far
    ULONGLONG recoveredAt = 0; // last engine-error recovery (see RecoverEngine)
    // A car he's going over carries on under him until then (game time, ms).
    CVehicle* underCar = nullptr;
    CVector underSpeed;       // as m_vecMoveSpeed
    unsigned underUntil = 0;
} g_skate;

bool g_toggleHeld = false;
bool g_readyAnnounced = false;

// Clicking both sticks (L3 + R3) gets on and off; see stick_combo.h. The
// sticks are read from XInput directly, so this works with or without GInput.
struct PadSticks {
    StickCombo combo;
    ULONGLONG retryAt[XUSER_MAX_COUNT] = {};

    void Read(bool& left, bool& right) {
        left = right = false;
        ULONGLONG now = GetTickCount64();
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            if (now < retryAt[i]) continue;
            XINPUT_STATE s{};
            if (XInputGetState(i, &s) != ERROR_SUCCESS) {
                retryAt[i] = now + 2000; // probing an empty slot is slow; don't do it every frame
                continue;
            }
            left |= (s.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            right |= (s.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        }
    }

    // True on the frame the combo completes; also rewrites the game's view
    // of the two stick buttons (on foot only).
    bool Update(CPlayerPed* ped, bool skating, ULONGLONG window) {
        bool left, right;
        Read(left, right);
        bool onFoot = ped && !ped->bInVehicle && !FindPlayerVehicle(-1, false);
        ULONGLONG now = GetTickCount64();
        bool fired = combo.Update(left, right, onFoot, skating, now, window);
        if (onFoot) {
            CControllerState& s = CPad::GetPad(0)->NewState;
            combo.Apply(s.ShockButtonL, s.ShockButtonR, now);
        }
        return fired;
    }
} g_sticks;

void Message(const char* text) {
    CHud::SetHelpMessage(text, true, false, false);
}

bool ToggleKeyPressed() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool held = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
    bool pressed = held && !g_toggleHeld;
    g_toggleHeld = held;
    return pressed;
}

// Why CJ can't skate right now (`why` is null if he can), and whether
// stopping for that reason keeps the board's momentum: a death or knockdown
// does; a script warp or cutscene must not fling him.
struct Blocked {
    const char* why = nullptr;
    bool momentum = false;
};

// Not CPed::IsPedInControl: that refuses mid-air (jumping onto the board is
// allowed) and ignores knockdowns anyway. Hits while skating become Skate
// bails once vehicles are in Skate's world (step 3).
Blocked Blocker(CPlayerPed* ped) {
    if (!ped) return {"no player"};
    if (!ped->IsAlive()) return {"the player died", true};
    if (ped->m_ePedState == PEDSTATE_ARRESTED) return {"the player was busted"};
    if (ped->bInVehicle || FindPlayerVehicle(-1, false)) return {"the player is in a vehicle"};
    if (CCutsceneMgr::ms_running) return {"a cutscene is running"};
    if (FrontEndMenuManager.m_bMenuActive) return {"the menu is open"};
    if (CPad::GetPad(0)->DisablePlayerControls) return {"a script has taken control of the player"};
    return {};
}

bool Check(int result, const char* what) {
    if (result >= 0) return true;
    Log("%s failed: %s", what, g_api.last_error());
    return false;
}

// Step 2 collision: a flat square at the given height. Real San Andreas
// collision replaces this in step 3.
bool InstallFloor(const CVector& centre) {
    float h = g_cfg.floorSize * 0.5f, z = centre.z;
    float tris[18] = {
        centre.x - h, centre.y - h, z, centre.x + h, centre.y - h, z, centre.x + h, centre.y + h, z,
        centre.x - h, centre.y - h, z, centre.x + h, centre.y + h, z, centre.x - h, centre.y + h, z,
    };
    if (!Check(g_api.install_collision(g_session, tris, 2, nullptr, nullptr, 0), "sk_install_collision")) return false;
    g_skate.floorCentre = centre;
    return true;
}

// ------------------------------------------------------------------ world

// San Andreas collision near the skater, handed to Skate as world-space
// triangles: buildings (terrain included), objects (benches, fences, rails)
// and stopped or slow vehicles, found through the world's 50 m sectors. Skate
// finds the grind rails in it and builds it on a background thread.
struct World {
    std::vector<float> tris; // reused between gathers
    CVector centre;          // of the world installed or building
    int pending = 0;         // generation still building, 0 = none
    ULONGLONG refreshAt = 0;
    std::vector<std::pair<CVehicle*, CVector>> cars; // in that world, and where they were
    ULONGLONG carsCheckAt = 0;
    ULONGLONG carsRebuildAt = 0; // cars may trigger a rebuild again from then
    uint64_t hash = 0;           // of the triangles last handed to Skate
} g_world;

// Car changes only matter this close to the skater (m), and rebuild his world
// at most this often (ms): cars creeping up to a light, or moving off far
// away, rebuilt it every quarter second.
constexpr float kCarWatchRange = 25.f;
constexpr ULONGLONG kCarRebuildMs = 1000;

// FNV-1a over a gather, to skip handing Skate the same world again.
uint64_t HashTris(const std::vector<float>& tris) {
    uint64_t h = 14695981039346656037ull;
    const auto* p = reinterpret_cast<const unsigned char*>(tris.data());
    for (size_t i = 0, n = tris.size() * sizeof(float); i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Cars slower than this (5 mph) are part of Skate's world, so riding into one
// is Skate's own collision; faster ones knock him off (see CheckCarHits).
constexpr float kCarMovingSpeed = 2.2f; // m/s
std::vector<std::pair<CVehicle*, CVector>> g_gatheredCars; // per gather

constexpr float kWorldBelow = 30.f, kWorldAbove = 40.f; // vertical reach of a gather

struct PackedVertex {
    int16_t x, y, z; // SA collision vertices, in 1/128 m
};

struct Xform {
    CVector right, fwd, up, pos;
    CVector Apply(const CVector& v) const { return pos + right * v.x + fwd * v.y + up * v.z; }
};

// Buildings without a matrix are placed by position and heading only.
Xform XformOf(CEntity* e) {
    if (e->m_matrix) {
        const CMatrix& m = *e->m_matrix;
        return {m.GetRight(), m.GetForward(), m.GetUp(), m.GetPosition()};
    }
    float c = std::cos(e->m_placement.m_fHeading), s = std::sin(e->m_placement.m_fHeading);
    return {CVector(c, s, 0.f), CVector(-s, c, 0.f), CVector(0.f, 0.f, 1.f), e->m_placement.m_vPosn};
}

struct Bounds {
    CVector lo, hi;
    bool Touches(const CVector& a, const CVector& b, const CVector& c) const {
        return std::fmax(a.x, std::fmax(b.x, c.x)) >= lo.x && std::fmin(a.x, std::fmin(b.x, c.x)) <= hi.x &&
               std::fmax(a.y, std::fmax(b.y, c.y)) >= lo.y && std::fmin(a.y, std::fmin(b.y, c.y)) <= hi.y &&
               std::fmax(a.z, std::fmax(b.z, c.z)) >= lo.z && std::fmin(a.z, std::fmin(b.z, c.z)) <= hi.z;
    }
};

void AddTri(std::vector<float>& out, const Bounds& bounds, const CVector& a, const CVector& b, const CVector& c) {
    if (!bounds.Touches(a, b, c)) return;
    out.insert(out.end(), {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
}

// A box as 12 outward-facing triangles.
void AddBox(std::vector<float>& out, const Bounds& bounds, const Xform& x, const CVector& mn, const CVector& mx) {
    CVector p[8];
    for (int i = 0; i < 8; i++) {
        p[i] = x.Apply(CVector(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z));
    }
    static const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        AddTri(out, bounds, p[f[0]], p[f[1]], p[f[2]]);
        AddTri(out, bounds, p[f[0]], p[f[2]], p[f[3]]);
    }
}

int g_unloaded = 0; // entities in range whose collision isn't streamed in, per gather

// `dummy`: an object the game hasn't made real yet (it does near CJ); it has
// the object's collision but isn't collidable itself. True if it was added.
bool AddEntity(CEntity* e, const Bounds& bounds, std::vector<float>& out, int& entities, bool dummy = false) {
    if (!e || e->m_nScanCode == CWorld::ms_nCurrentScanCode) return false;
    if (dummy ? !e->bIsVisible || e->m_nAreaCode != CGame::currArea : !e->bUsesCollision) return false;
    e->m_nScanCode = CWorld::ms_nCurrentScanCode; // big entities sit in many sectors
    CBaseModelInfo* info = CModelInfo::GetModelInfo(e->m_nModelIndex);
    CColModel* col = info ? info->m_pColModel : nullptr;
    if (!col) return false;
    CCollisionData* data = col->m_pColData; // null until streamed in
    if (!data) {
        g_unloaded++;
        return false;
    }
    Xform x = XformOf(e);
    CVector c = x.Apply(col->m_boundSphere.m_vecCenter);
    float r = col->m_boundSphere.m_fRadius;
    if (c.x + r < bounds.lo.x || c.x - r > bounds.hi.x || c.y + r < bounds.lo.y || c.y - r > bounds.hi.y ||
        c.z + r < bounds.lo.z || c.z - r > bounds.hi.z) {
        return false;
    }
    const auto* v = reinterpret_cast<const PackedVertex*>(data->m_pVertices);
    auto vertex = [&](unsigned i) { return x.Apply(CVector(v[i].x / 128.f, v[i].y / 128.f, v[i].z / 128.f)); };
    for (int i = 0; i < data->m_nNumTriangles && v; i++) {
        // SA's face normal is (C - A) x (B - A) (CColTrianglePlane), the
        // opposite winding to Skate's, so B and C swap.
        const CColTriangle& t = data->m_pTriangles[i];
        AddTri(out, bounds, vertex(t.m_nVertA), vertex(t.m_nVertC), vertex(t.m_nVertB));
    }
    for (int i = 0; i < data->m_nNumBoxes; i++) {
        AddBox(out, bounds, x, data->m_pBoxes[i].m_vecMin, data->m_pBoxes[i].m_vecMax);
    }
    for (int i = 0; i < data->m_nNumSpheres; i++) { // as boxes: close enough to skate into
        const CColSphere& s = data->m_pSpheres[i];
        CVector extent(s.m_fRadius, s.m_fRadius, s.m_fRadius);
        AddBox(out, bounds, x, s.m_vecCenter - extent, s.m_vecCenter + extent);
    }
    entities++;
    return true;
}

// Collects the collision within `radius` of `centre` into `out`, from
// kWorldBelow under the lower of the centre and the skater's height `skaterZ`
// to kWorldAbove over the higher, so the ground he is on is always in it.
int GatherWorld(const CVector& centre, float skaterZ, float radius, std::vector<float>& out) {
    out.clear();
    Bounds bounds{CVector(centre.x - radius, centre.y - radius, std::fmin(centre.z, skaterZ) - kWorldBelow),
                  CVector(centre.x + radius, centre.y + radius, std::fmax(centre.z, skaterZ) + kWorldAbove)};
    int entities = 0;
    g_unloaded = 0;
    g_gatheredCars.clear();
    CWorld::AdvanceCurrentScanCode();
    int x0 = std::max(0, static_cast<int>(CWorld::GetSectorX(bounds.lo.x)));
    int x1 = std::min(NUMSECTORS_X - 1, static_cast<int>(CWorld::GetSectorX(bounds.hi.x)));
    int y0 = std::max(0, static_cast<int>(CWorld::GetSectorY(bounds.lo.y)));
    int y1 = std::min(NUMSECTORS_Y - 1, static_cast<int>(CWorld::GetSectorY(bounds.hi.y)));
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            CSector* sector = CWorld::GetSector(x, y);
            for (auto* n = reinterpret_cast<CPtrNodeSingleLink*>(sector->m_buildingList.GetNode()); n; n = n->pNext) {
                AddEntity(static_cast<CEntity*>(n->pItem), bounds, out, entities);
            }
            // Street furniture farther from CJ (or all of it, just after a
            // respawn) is still a dummy; a real object removes its dummy.
            for (auto* n = reinterpret_cast<CPtrNodeDoubleLink*>(sector->m_dummyList.GetNode()); n; n = n->pNext) {
                AddEntity(static_cast<CEntity*>(n->pItem), bounds, out, entities, true);
            }
            CRepeatSector* repeat = CWorld::GetRepeatSector(x % NUMREPEATSECTORS_X, y % NUMREPEATSECTORS_Y);
            for (auto* n = reinterpret_cast<CPtrNodeDoubleLink*>(repeat->m_lists[2].GetNode()); n; n = n->pNext) {
                AddEntity(static_cast<CEntity*>(n->pItem), bounds, out, entities); // objects
            }
            if (!g_cfg.parkedCarsCollide) continue;
            for (auto* n = reinterpret_cast<CPtrNodeDoubleLink*>(repeat->m_lists[0].GetNode()); n; n = n->pNext) {
                auto* car = static_cast<CVehicle*>(n->pItem); // stopped or slow: see kCarMovingSpeed
                if (car && car->m_vecMoveSpeed.Magnitude() * 50.f < kCarMovingSpeed && AddEntity(car, bounds, out, entities)) {
                    g_gatheredCars.emplace_back(car, car->GetPosition());
                }
            }
        }
    }
    return entities;
}

// Gathers around `centre` and has Skate build it in the background.
void QueueWorld(const CVector& centre, float skaterZ) {
    ULONGLONG started = GetTickCount64();
    int entities = GatherWorld(centre, skaterZ, g_cfg.worldRadius, g_world.tris);
    g_world.refreshAt = started + static_cast<ULONGLONG>(g_cfg.worldRefresh * 1000.f);
    if (g_world.tris.size() < 9) return; // nothing streamed in here yet: keep the current world
    uint64_t hash = HashTris(g_world.tris);
    if (hash == g_world.hash) { // the same world again: nothing to build
        g_world.centre = centre;
        g_world.cars = g_gatheredCars;
        return;
    }
    int generation = g_api.queue_world(g_session, g_world.tris.data(), static_cast<uint32_t>(g_world.tris.size() / 9));
    if (!Check(generation, "sk_queue_world")) return;
    g_world.pending = generation;
    g_world.centre = centre;
    g_world.cars = g_gatheredCars;
    g_world.hash = hash;
    Log("World %d queued around %.0f %.0f %.0f: %d entities (%d not streamed in), %zu triangles, gathered in %llu ms",
        generation, centre.x, centre.y, centre.z, entities, g_unloaded, g_world.tris.size() / 9, GetTickCount64() - started);
}

// The world under a skater who starts (or is put back by Skate's respawn)
// there, built right away (about 70 ms), or a flat floor if no collision is
// loaded there. Collision the game hasn't streamed in yet is loaded first.
bool InstallWorldAt(const CVector& centre, float skaterZ) {
    ULONGLONG started = GetTickCount64();
    int entities = GatherWorld(centre, skaterZ, g_cfg.worldRadius, g_world.tris);
    if (g_unloaded > 0) {
        CVector at = centre;
        CStreaming::LoadSceneCollision(&at);
        entities = GatherWorld(centre, skaterZ, g_cfg.worldRadius, g_world.tris);
    }
    g_world.pending = 0;
    g_world.centre = centre;
    g_world.refreshAt = started + static_cast<ULONGLONG>(g_cfg.worldRefresh * 1000.f);
    if (g_world.tris.size() >= 9 * 8) {
        int generation = g_api.install_world(g_session, g_world.tris.data(), static_cast<uint32_t>(g_world.tris.size() / 9));
        SkWorldInfo info{};
        if (Check(generation, "sk_install_world") && Check(g_api.world_info(g_session, &info), "sk_world_info")) {
            Log("World %d installed around %.0f %.0f %.0f: %d entities (%d not streamed in), %u triangles, %u rails, %llu ms",
                generation, centre.x, centre.y, centre.z, entities, g_unloaded, info.triangles, info.rails,
                GetTickCount64() - started);
            g_world.cars = g_gatheredCars;
            g_world.hash = HashTris(g_world.tris);
            return true;
        }
    }
    Log("No San Andreas collision loaded here (%d entities); using a flat floor", entities);
    g_world.cars.clear();
    g_world.hash = 0;
    return InstallFloor(centre);
}

// Whether the cars in the skater's world are still as gathered: none moved
// off or sped up, and no car near him slowed into it (stopped at a light,
// say). Otherwise the world is rebuilt, so stopped cars are solid where they
// are and a car that drove off leaves nothing behind.
bool CarsChanged(const CVector& skater) {
    if (!g_cfg.parkedCarsCollide || !CPools::ms_pVehiclePool) return false;
    const float range = std::fmin(kCarWatchRange, g_cfg.worldRadius * 0.7f); // ("near" is a Windows macro)
    std::vector<bool> seen(g_world.cars.size(), false);
    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
        CVehicle* car = CPools::ms_pVehiclePool->GetAt(i);
        if (!car) continue;
        CVector at = car->GetPosition();
        bool slow = car->m_vecMoveSpeed.Magnitude() * 50.f < kCarMovingSpeed;
        auto known = std::find_if(g_world.cars.begin(), g_world.cars.end(), [car](const auto& c) { return c.first == car; });
        if (known != g_world.cars.end()) {
            seen[known - g_world.cars.begin()] = true;
            if ((!slow || (at - known->second).Magnitude() > 0.4f) && (at - skater).Magnitude() < range) return true;
        } else if (slow && car->bUsesCollision && (at - skater).Magnitude() < range) {
            return true;
        }
    }
    for (size_t k = 0; k < seen.size(); k++) { // a car that's gone (deleted)
        if (!seen[k] && (g_world.cars[k].second - skater).Magnitude() < range) return true;
    }
    return false;
}

// Keeps the skater's world around him, a little ahead of where he's going.
void StreamWorld() {
    if (g_world.pending) {
        SkWorldInfo info{};
        if (!Check(g_api.world_info(g_session, &info), "sk_world_info") || static_cast<int>(info.generation) < g_world.pending) {
            return;
        }
        Log("World %u ready: %u triangles, %u rails, built in %.0f ms (%u failed builds so far)", info.generation,
            info.triangles, info.rails, info.build_ms, info.failures);
        g_world.pending = 0;
    }
    const float* r = g_skate.pose.root;
    const float* v = g_skate.pose.velocity;
    CVector skater(r[12], r[13], r[14]);
    // Centre on the ground, never on the skater's own height (a skater who
    // fell must not drag his world down after him), searched from just above
    // him: from higher up, a bridge or freeway overhead was taken for the
    // ground and the street under it left out of the world.
    auto groundBelow = [](float x, float y, float z) {
        bool found = false;
        float ground = CWorld::FindGroundZFor3DCoord(x, y, z + 2.f, &found, nullptr);
        return found ? ground : g_world.centre.z; // none: he is under the map; keep the last height
    };
    // Put somewhere outside his world (Skate's respawn at a session marker):
    // have the game load that area's collision and buildings now (it streams
    // them in around CJ only over the next few seconds), then build his world
    // there right away rather than in the background.
    float sx = skater.x - g_world.centre.x, sy = skater.y - g_world.centre.y;
    if (std::sqrt(sx * sx + sy * sy) > g_cfg.worldRadius * 0.6f) {
        Log("The skater left his world (respawned?) at %.0f %.0f %.0f", skater.x, skater.y, skater.z);
        CStreaming::LoadSceneCollision(&skater);
        InstallWorldAt(CVector(skater.x, skater.y, groundBelow(skater.x, skater.y, skater.z)), skater.z);
        return;
    }
    float lead = std::fmin(0.8f, 20.f / std::fmax(1.f, std::sqrt(v[0] * v[0] + v[1] * v[1]))); // at most 20 m ahead
    CVector ahead(skater.x + v[0] * lead, skater.y + v[1] * lead, 0.f);
    ahead.z = groundBelow(ahead.x, ahead.y, skater.z);
    float dx = ahead.x - g_world.centre.x, dy = ahead.y - g_world.centre.y;
    ULONGLONG now = GetTickCount64();
    bool carsChanged = false;
    if (now >= g_world.carsCheckAt && now >= g_world.carsRebuildAt) {
        g_world.carsCheckAt = now + 250;
        carsChanged = CarsChanged(skater);
    }
    if (std::sqrt(dx * dx + dy * dy) < g_cfg.worldRebuild && now < g_world.refreshAt && !carsChanged) return;
    g_world.carsRebuildAt = now + kCarRebuildMs;
    QueueWorld(ahead, skater.z);
}

// Puts CJ where the skater is. The root's translation is at the skater's
// feet; its -Y axis is the way the skater faces (skate_ffi maps Skate's +Z
// forward that way).
void PlaceCJ(CPlayerPed* ped) {
    const float* r = g_skate.pose.root;
    CVector pos(r[12], r[13], r[14] + g_cfg.feetOffset);
    float heading = std::atan2(-r[5], -r[4]) - kPi * 0.5f;
    ped->SetHeading(heading);
    ped->SetPosn(pos);
    ped->m_fHeadingCurrent = heading;
    ped->m_fHeadingGoal = heading;
    ped->m_vecMoveSpeed = CVector(0.f, 0.f, 0.f);
    ped->m_vecTurnSpeed = CVector(0.f, 0.f, 0.f);
    ped->bIsStanding = true;
    ped->bWasStanding = true;
    ped->UpdateRwMatrix();
    ped->UpdateRwFrame();
    ped->RemoveAndAdd();
    g_skate.placed = pos;
}

// ------------------------------------------------------------------ camera

// Skate 3's camera (computed by the engine) replaces the game's while
// skating. It is written into the active CCam right after CCam::Process,
// inside CCamera::Process, so everything the game derives from the camera
// later in the frame (matrices, culling, LODs, coronas, blur, FOV through
// Widescreen Fix) is built from it. Getting on blends from the game's camera;
// getting off turns the game's camera to look the same way (tilted down if
// CJ leaves mid-air) and blends back to it.
struct CamPose {
    CVector pos, fwd, up;
    float fov; // in the game's FOV convention
};

// The automated test's camera (see AutoTest below): while on, it replaces
// whatever the game or Skate would show.
struct TestCamera {
    bool on = false;
    CamPose pose{};
} g_testCamera;

struct SkateCamera {
    enum class Phase { Off, In, On, Out };
    Phase phase = Phase::Off;
    ULONGLONG blendStart = 0;
    CamPose last{};       // last Skate pose shown; frozen while blending out
    bool haveLast = false;
} g_cam;

CVector Cross(const CVector& a, const CVector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(const CVector& a, const CVector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CVector Normalised(const CVector& v, const CVector& fallback) {
    float length = v.Magnitude();
    return length > 1e-4f ? v * (1.f / length) : fallback;
}

// Skate's FOV is vertical. The game's FOV is horizontal: for a 4:3 screen
// with Widescreen Fix (which widens it to the real screen itself), or for
// the game's own aspect ratio without it.
float GameFov(float skateVerticalDegrees) {
    static const bool widescreenFix = GetModuleHandleA("GTASA.WidescreenFix.asi") != nullptr;
    float aspect = widescreenFix ? 4.f / 3.f : CDraw::ms_fAspectRatio;
    float half = skateVerticalDegrees * kPi / 360.f;
    return std::atan(std::tan(half) * aspect) * 360.f / kPi;
}

bool SkatePose(CamPose& out) {
    const SkPose& p = g_skate.pose;
    if (!p.has_camera || !(p.camera_fov > 1.f && p.camera_fov < 170.f)) return false;
    out.pos = CVector(p.camera_position[0], p.camera_position[1], p.camera_position[2]);
    out.fwd = Normalised(CVector(p.camera_forward[0], p.camera_forward[1], p.camera_forward[2]), CVector(0.f, 1.f, 0.f));
    out.up = Normalised(CVector(p.camera_up[0], p.camera_up[1], p.camera_up[2]), CVector(0.f, 0.f, 1.f));
    out.fov = GameFov(p.camera_fov);
    return true;
}

CamPose PoseOf(const CCam& cam) {
    return {cam.m_vecSource, cam.m_vecFront, cam.m_vecUp, cam.m_fFOV};
}

CamPose Blend(const CamPose& a, const CamPose& b, float t) {
    t = std::fmin(std::fmax(t, 0.f), 1.f);
    t = t * t * (3.f - 2.f * t); // ease in and out
    return {a.pos + (b.pos - a.pos) * t, Normalised(a.fwd + (b.fwd - a.fwd) * t, b.fwd),
            Normalised(a.up + (b.up - a.up) * t, b.up), a.fov + (b.fov - a.fov) * t};
}

void WritePose(CCam& cam, const CamPose& c) {
    CVector right = Normalised(Cross(c.fwd, c.up), CVector(1.f, 0.f, 0.f));
    cam.m_vecSource = c.pos;
    cam.m_vecFront = c.fwd;
    cam.m_vecUp = Normalised(Cross(right, c.fwd), CVector(0.f, 0.f, 1.f));
    cam.m_fFOV = c.fov;
}

float BlendProgress(float seconds) {
    if (seconds <= 0.f) return 1.f;
    return static_cast<float>(GetTickCount64() - g_cam.blendStart) / (seconds * 1000.f);
}

// Called right after the game's active camera computed this frame's pose.
void OnCameraProcessed(CCam* cam) {
    if (cam != &TheCamera.m_aCams[TheCamera.m_nActiveCam]) return;
    if (g_testCamera.on) {
        WritePose(*cam, g_testCamera.pose);
        return;
    }
    switch (g_cam.phase) {
    case SkateCamera::Phase::Off:
        return;
    case SkateCamera::Phase::In:
    case SkateCamera::Phase::On: {
        CamPose skate;
        if (!g_skate.active || !SkatePose(skate)) return;
        CamPose out = skate;
        if (g_cam.phase == SkateCamera::Phase::In) {
            float t = BlendProgress(g_cfg.cameraBlendIn);
            if (t >= 1.f) g_cam.phase = SkateCamera::Phase::On;
            else out = Blend(PoseOf(*cam), skate, t);
        }
        WritePose(*cam, out);
        g_cam.last = out;
        g_cam.haveLast = true;
        return;
    }
    case SkateCamera::Phase::Out: {
        float t = BlendProgress(g_cfg.cameraBlendOut);
        if (t >= 1.f || !g_cam.haveLast) {
            g_cam.phase = SkateCamera::Phase::Off;
            return;
        }
        WritePose(*cam, Blend(g_cam.last, PoseOf(*cam), t));
        return;
    }
    }
}

// CCamera::Process calls the active CCam::Process at 0x52B90A (1.0 US). We
// take over that call, keeping whatever it called before (the game, or
// another mod that hooked it first).
using CamProcessFn = void(__thiscall*)(CCam*);
CamProcessFn g_camProcess = nullptr;

void __fastcall CamProcessHook(CCam* cam, void* /*edx*/) {
    g_camProcess(cam);
    OnCameraProcessed(cam);
}

bool HookCameraProcess() {
    constexpr uintptr_t at = 0x52B90A;
    auto* code = reinterpret_cast<uint8_t*>(at);
    if (code[0] != 0xE8) return false; // not the call we expect
    g_camProcess = reinterpret_cast<CamProcessFn>(at + 5 + *reinterpret_cast<int32_t*>(code + 1));
    DWORD protect;
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &protect)) return false;
    *reinterpret_cast<int32_t*>(code + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CamProcessHook) - (at + 5));
    VirtualProtect(code, 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    return true;
}

bool g_cameraHooked = false;

void StartCamera() {
    if (!g_cfg.skateCamera || !g_cameraHooked) return;
    g_api.set_aspect_ratio(g_session, static_cast<float>(RsGlobal.maximumWidth) /
                                          std::fmax(1.f, static_cast<float>(RsGlobal.maximumHeight)));
    g_cam.phase = SkateCamera::Phase::In; // blends from the game's live camera
    g_cam.blendStart = GetTickCount64();
    g_cam.haveLast = false;
}

// `airborne`: CJ leaves mid-air, so the game camera looks down at the landing.
void StopCamera(bool blend, bool airborne) {
    if (g_cam.phase == SkateCamera::Phase::Off) return;
    if (!blend || !g_cam.haveLast) {
        g_cam.phase = SkateCamera::Phase::Off;
        return;
    }
    // Turn the game's follow camera to where the Skate camera looks, so the
    // blend back is short. Its look direction is
    // (-cos H cos V, -sin H cos V, sin V).
    const CVector& f = g_cam.last.fwd;
    CCam& cam = TheCamera.m_aCams[TheCamera.m_nActiveCam];
    cam.m_fHorizontalAngle = std::atan2(-f.y, -f.x);
    cam.m_fVerticalAngle = airborne ? -g_cfg.exitAirPitch * kPi / 180.f : std::asin(std::fmin(std::fmax(f.z, -1.f), 1.f));
    g_cam.phase = SkateCamera::Phase::Out;
    g_cam.blendStart = GetTickCount64();
}

// --------------------------------------------------------------- animation

// CJ's skeleton follows the skater: each San Andreas bone is fitted to its
// Skate counterpart by skate_ffi (sk_rig_*, after the mashup's soldier), and
// the result replaces the ped's bone matrices just before he renders. SA ped
// hierarchies keep world-space matrices (no local-space flag), which is what
// sk_rig_pose returns.
struct BoneMap {
    int id;            // San Andreas bone ID (ePedBones)
    const char* skate; // the Skate bone it follows
    int childId;       // bone at the end of its segment, -1 for none
    const char* skateChild;
};

// Neck, head and fingers ride on their parents, as in the mashup.
constexpr BoneMap kBoneMap[] = {
    {1, "HIPS", 3, "SPINE"},
    {2, "HIPS", 3, "SPINE"},
    {3, "SPINE", 4, "SPINE2"},
    {4, "SPINE2", 5, "NECK"},
    {21, "RIGHTSHOULDER", 22, "RIGHTARM"},
    {22, "RIGHTARM", 23, "RIGHTFOREARM"},
    {23, "RIGHTFOREARM", 24, "RIGHTHAND"},
    {24, "RIGHTHAND", -1, nullptr},
    {31, "LEFTSHOULDER", 32, "LEFTARM"},
    {32, "LEFTARM", 33, "LEFTFOREARM"},
    {33, "LEFTFOREARM", 34, "LEFTHAND"},
    {34, "LEFTHAND", -1, nullptr},
    {41, "LEFTUPLEG", 42, "LEFTLEG"},
    {42, "LEFTLEG", 43, "LEFTFOOT"},
    {43, "LEFTFOOT", 44, "LEFTTOEBASE"},
    {44, "LEFTTOEBASE", -1, nullptr},
    {51, "RIGHTUPLEG", 52, "RIGHTLEG"},
    {52, "RIGHTLEG", 53, "RIGHTFOOT"},
    {53, "RIGHTFOOT", 54, "RIGHTTOEBASE"},
    {54, "RIGHTTOEBASE", -1, nullptr},
};

struct Rig {
    RpClump* clump = nullptr; // the clump it was set up for (clothes rebuild it)
    int bones = 0;
    bool ready = false;
    bool failedLogged = false;
    std::vector<float> matrices;
} g_rig;

// Inverse of an affine RW matrix, as 16 column-major floats.
void InverseColumns(const RwMatrix& m, float* out) {
    const RwV3d &a = m.right, &b = m.up, &c = m.at, &p = m.pos;
    RwV3d bc = {b.y * c.z - b.z * c.y, b.z * c.x - b.x * c.z, b.x * c.y - b.y * c.x};
    RwV3d ca = {c.y * a.z - c.z * a.y, c.z * a.x - c.x * a.z, c.x * a.y - c.y * a.x};
    RwV3d ab = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    float det = a.x * bc.x + a.y * bc.y + a.z * bc.z;
    float k = std::fabs(det) > 1e-12f ? 1.f / det : 0.f;
    // Rows of the inverse rotation are bc, ca, ab (scaled); store as columns.
    float r[3][3] = {{bc.x * k, bc.y * k, bc.z * k}, {ca.x * k, ca.y * k, ca.z * k}, {ab.x * k, ab.y * k, ab.z * k}};
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) out[col * 4 + row] = r[row][col];
        out[col * 4 + 3] = 0.f;
    }
    for (int row = 0; row < 3; row++) out[12 + row] = -(r[row][0] * p.x + r[row][1] * p.y + r[row][2] * p.z);
    out[15] = 1.f;
}

bool SetupRig(CPlayerPed* ped) {
    RpClump* clump = ped->m_pRwClump;
    RpHAnimHierarchy* h = clump ? GetAnimHierarchyFromSkinClump(clump) : nullptr;
    RpAtomic* atomic = clump ? GetFirstAtomic(clump) : nullptr;
    RpSkin* skin = atomic ? RpSkinGeometryGetSkin(RpAtomicGetGeometry(atomic)) : nullptr;
    if (!h || !skin || h->numNodes <= 0) {
        Log("Rig: CJ has no skinned skeleton; he won't animate");
        return false;
    }
    const RwMatrix* toBone = RpSkinGetSkinToBoneMatrices(skin);
    int n = h->numNodes;
    // RW walks the nodes in order, pushing the parent for a branch and
    // popping back to it after a leaf.
    std::vector<int> parent(n);
    std::vector<int> stack;
    int current = -1;
    for (int i = 0; i < n; i++) {
        parent[i] = current;
        int flags = h->pNodeInfo[i].flags;
        if (flags & rpHANIMPUSHPARENTMATRIX) stack.push_back(current);
        if (flags & rpHANIMPOPPARENTMATRIX) {
            current = stack.empty() ? -1 : stack.back();
            if (!stack.empty()) stack.pop_back();
        } else {
            current = i;
        }
    }
    std::vector<SkRigBone> bones(n);
    int mapped = 0;
    // The chest bones on the shoulder joints (301 right, 302 left) hang off
    // the lower spine in SA's skeleton. They ride on the clavicles instead,
    // and skate_ffi turns them part way with the upper arms (shoulder
    // helpers), or the skin round the shoulders stretches.
    int rightClavicle = RpHAnimIDGetIndex(h, 21), leftClavicle = RpHAnimIDGetIndex(h, 31);
    for (int i = 0; i < n; i++) {
        SkRigBone& b = bones[i];
        b = SkRigBone{nullptr, nullptr, parent[i], -1, {}};
        int id = h->pNodeInfo[i].nodeID;
        int clavicle = id == 301 ? rightClavicle : id == 302 ? leftClavicle : -1;
        if (clavicle >= 0 && clavicle < i) b.parent = clavicle;
        InverseColumns(toBone[i], b.bind); // skin-to-bone inverted: the bone's bind pose
        for (const BoneMap& m : kBoneMap) {
            if (m.id != h->pNodeInfo[i].nodeID) continue;
            b.skate = m.skate;
            b.skate_child = m.skateChild;
            b.child = m.childId >= 0 ? RpHAnimIDGetIndex(h, m.childId) : -1;
            mapped++;
        }
    }
    if (!Check(g_api.rig_setup(g_session, bones.data(), static_cast<uint32_t>(n), g_cfg.rigFacing), "sk_rig_setup")) return false;
    g_rig.clump = clump;
    g_rig.bones = n;
    g_rig.ready = true;
    g_rig.failedLogged = false;
    g_rig.matrices.resize(static_cast<size_t>(n) * 16);
    Check(g_api.rig_tuning(g_session, g_cfg.elbowFollow, g_cfg.shoulderFollow), "sk_rig_tuning");
    float q[4] = {0.f, 0.f, 0.f, 1.f};
    g_api.rig_facing(g_session, q);
    Log("Rig: %d bones, %d follow the skater; facing quaternion %.3f %.3f %.3f %.3f", n, mapped, q[0], q[1], q[2], q[3]);
    for (int i = 0; i < n; i++) {
        const SkRigBone& b = bones[i];
        Log("  bone %2d id %3d parent %2d %-14s bind at %6.3f %6.3f %6.3f", i, h->pNodeInfo[i].nodeID, b.parent,
            b.skate ? b.skate : "-", b.bind[12], b.bind[13], b.bind[14]);
    }
    return true;
}

// Just before CJ renders: his bones take the skater's pose.
void ApplyRig(CPed* ped) {
    if (!g_cfg.animate || !g_skate.active || ped != g_skate.ped || !g_rig.ready) return;
    if (ped->m_pRwClump != g_rig.clump) { // clothes changed: set up again next time he gets on
        g_rig.ready = false;
        return;
    }
    RpHAnimHierarchy* h = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!h || h->numNodes != g_rig.bones) return;
    if (g_api.rig_pose(g_session, g_rig.matrices.data(), static_cast<uint32_t>(g_rig.bones)) != g_rig.bones) {
        if (!g_rig.failedLogged) Log("sk_rig_pose failed: %s", g_api.last_error());
        g_rig.failedLogged = true;
        return;
    }
    RwMatrix* m = RpHAnimHierarchyGetMatrixArray(h);
    for (int i = 0; i < g_rig.bones; i++) {
        const float* c = &g_rig.matrices[static_cast<size_t>(i) * 16];
        m[i].right = {c[0], c[1], c[2]};
        m[i].up = {c[4], c[5], c[6]};
        m[i].at = {c[8], c[9], c[10]};
        m[i].pos = {c[12], c[13], c[14]};
        m[i].flags &= ~0x00020000u; // rwMATRIXINTERNALIDENTITY: these are never identity
    }
}

// --------------------------------------------------------------- the board

// The Skate skateboard (deck, trucks, wheels) from skate_ffi, posed with the
// skater's board bones and drawn just before CJ renders, lit by the time of
// day like the world's objects. It is drawn with Direct3D directly between a
// saved and a restored copy of the whole device state: other mods change
// the device behind RenderWare's back, so RenderWare's state cache can't be
// trusted to draw it opaque and depth-tested (it came out see-through).
struct BoardVertex {
    float x, y, z;
    D3DCOLOR colour;
    float u, v;
};
constexpr DWORD kBoardFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct BoardPart {
    IDirect3DTexture9* texture = nullptr; // managed: survives device resets
    uint32_t first = 0, count = 0;        // its vertices in sk_board_pose's output
    std::vector<uint16_t> indices;
    std::vector<float> uvs;
};

struct Board {
    bool tried = false; // set up on the first draw
    bool failedLogged = false;
    std::vector<BoardPart> parts;
    std::vector<float> posed; // 6 floats per vertex
    std::vector<BoardVertex> vertices;
} g_board;

// A mipmapped texture from RGBA rows. Opaque: the converted textures' alpha
// holds other material data.
IDirect3DTexture9* BoardTexture(IDirect3DDevice9* device, const SkBoardSurface& s) {
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(s.width, s.height, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                     &texture, nullptr))) {
        return nullptr;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (uint32_t y = 0; y < s.height; y++) {
        const uint8_t* src = s.rgba + static_cast<size_t>(y) * s.width * 4;
        auto* dst = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        for (uint32_t x = 0; x < s.width; x++, src += 4, dst += 4) {
            dst[0] = src[2]; // BGRA in memory
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
    }
    texture->UnlockRect(0);
    texture->GenerateMipSubLevels();
    return texture;
}

void SetupBoard() {
    g_board.tried = true;
    auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
    if (!device) return;
    SkBoardSurface s[8];
    int n = g_api.board_mesh(g_session, s, 8);
    if (n <= 0 || n > 8) {
        Log("Board: nothing to draw (%s)", n < 0 ? g_api.last_error() : "no surfaces");
        return;
    }
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        BoardPart part;
        part.texture = BoardTexture(device, s[i]);
        if (!part.texture) Log("Board: texture %d (%ux%u) could not be made; that part is drawn plain", i, s[i].width, s[i].height);
        part.first = s[i].first_vertex;
        part.count = s[i].vertex_count;
        part.indices.assign(s[i].indices, s[i].indices + s[i].index_count);
        part.uvs.assign(s[i].uvs, s[i].uvs + static_cast<size_t>(s[i].vertex_count) * 2);
        total = std::max(total, part.first + part.count);
        g_board.parts.push_back(std::move(part));
    }
    g_board.posed.resize(static_cast<size_t>(total) * 6);
    g_board.vertices.resize(total);
    Log("Board: %d parts, %u vertices", n, total);
}

void DrawBoard() {
    if (!g_board.tried) SetupBoard();
    if (g_board.parts.empty()) return;
    auto total = static_cast<uint32_t>(g_board.vertices.size());
    if (g_api.board_pose(g_session, g_board.posed.data(), total) != static_cast<int>(total)) {
        if (!g_board.failedLogged) Log("sk_board_pose failed: %s", g_api.last_error());
        g_board.failedLogged = true;
        return;
    }
    // Lit by the lights the game set up for CJ this frame (CEntity::
    // SetupLighting runs just before CPed::Render): the time of day's
    // ambient and sun, scaled by the shade he stands in, so the board matches
    // him at every hour. 1.0 US addresses, from gta-reversed's app_light.h.
    const auto& ambientLight = *reinterpret_cast<const RwRGBAReal*>(0xC886A4); // AmbientLightColour
    const auto& directLight = *reinterpret_cast<const RwRGBAReal*>(0xC88694);  // DirectionalLightColour
    RpLight* directional = *reinterpret_cast<RpLight**>(0xC886EC);             // pDirect
    // CJ's own materials and shaders take less of those lights than the raw
    // values; BoardBrightness matches the board to him (test runs may
    // override it with the SK_BOARD_BRIGHTNESS environment variable).
    float brightness = g_cfg.boardBrightness;
    char env[32];
    if (GetEnvironmentVariableA("SK_BOARD_BRIGHTNESS", env, sizeof env)) brightness = std::strtof(env, nullptr);
    const float ambient[3] = {ambientLight.red * brightness, ambientLight.green * brightness, ambientLight.blue * brightness};
    const float direct[3] = {directLight.red * brightness, directLight.green * brightness, directLight.blue * brightness};
    CVector toLight(0.f, 0.f, 0.f); // none while the game has the directional light off (interiors)
    if (directional && (RpLightGetFlags(directional) & rpLIGHTLIGHTATOMICS)) {
        const RwV3d& at = RwFrameGetLTM(RpLightGetFrame(directional))->at; // the way the light shines
        toLight = CVector(-at.x, -at.y, -at.z);
    }
    for (const BoardPart& part : g_board.parts) {
        for (uint32_t k = 0; k < part.count; k++) {
            const float* p = &g_board.posed[static_cast<size_t>(part.first + k) * 6];
            float facing = std::fmax(0.f, p[3] * toLight.x + p[4] * toLight.y + p[5] * toLight.z);
            auto channel = [&](int i) { return static_cast<DWORD>(std::fmin(1.f, ambient[i] + direct[i] * facing) * 255.f); };
            g_board.vertices[part.first + k] = {p[0], p[1], p[2], D3DCOLOR_XRGB(channel(0), channel(1), channel(2)),
                                                part.uvs[k * 2], part.uvs[k * 2 + 1]};
        }
    }
    auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
    IDirect3DStateBlock9* saved = nullptr;
    if (!device || FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
    // Positions are already in the world; the camera's view and projection
    // are the ones RenderWare set for the frame.
    const D3DMATRIX identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kBoardFvf);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    for (const BoardPart& part : g_board.parts) {
        device->SetTexture(0, part.texture);
        device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, part.count, static_cast<UINT>(part.indices.size() / 3),
                                       part.indices.data(), D3DFMT_INDEX16, &g_board.vertices[part.first], sizeof(BoardVertex));
    }
    saved->Apply();
    saved->Release();
}

// ------------------------------------------------------- skateboard sounds

// Skate 3's own skateboard sounds, made by Setup from the player's game
// (skate-audio\sfx): the wheels rolling on whatever CJ rides (concrete,
// street, brick, grass, wood, metal), pops, flips, landings, grinds and bails,
// driven each frame by the skater's state (sk_feedback). They play through
// XAudio2 (sound.h) at the game's SFX volume.
Sound g_sound;
bool g_windowPaused = false; // the game window lost focus (the game pauses its own audio)
bool g_logSounds = false;    // log each moment's sound (test runs)

enum Ground { kConcrete, kAsphalt, kBrick, kGrass, kWood, kMetal, kGrounds };
const char* const kGroundNames[kGrounds] = {"concrete", "street", "brick", "grass", "wood", "metal"};
// How loud each ground's rolling loop is, against the others.
constexpr float kRollGain[kGrounds] = {0.55f, 0.6f, 0.6f, 0.45f, 0.6f, 0.5f};

struct SkateSounds {
    bool ready = false;
    std::vector<int> pop, flip, flipFast, land, seam, bail, body, board; // clips
    int roll[kGrounds][2];         // loops per ground: rolling fast, slow (-1: none)
    float rollVolume[kGrounds][2] = {};
    std::vector<int> grinds[2];    // loops: concrete, metal
    int grind = -1;                // the grind loop in use
    float grindVolume = 0.f;
    int slide = -1, air = -1;      // loops: the body sliding in a bail, the wheels spinning in the air
    int combo[2] = {-1, -1};       // Skate 3's combo sounds as the multiplier steps up: x2, x3
    // Feel: loops that follow speed (wind, rattle, tone, cloth flapping),
    // powerslides and the foot brake; one-shots for cars whooshing by, the
    // swell before a big landing, cloth on tricks, the rattle after a pop,
    // truck squeaks on hard carves, the loose board tumbling after a bail.
    enum FeelLoop { kWind, kRattle, kTone, kFlap, kSlideSkid, kBrake, kFeelLoops };
    int feel[kFeelLoops] = {-1, -1, -1, -1, -1, -1};
    float feelVolume[kFeelLoops] = {};
    std::vector<int> whoosh, preland, cloth, rattle, squeak, tumble;
    struct Passing {
        const void* car;
        float closing; // last frame's closing speed (m/s, >0 coming nearer)
        ULONGLONG whooshedAt;
    };
    std::vector<Passing> passing;  // cars near the skater
    float airTime = 0.f;
    bool prelanded = false;        // the pre-landing swell played this air
    ULONGLONG squeakAt = 0, tumbleAt = 0, airProbeAt = 0;
    float groundBelow = 0.f;       // the ground under the board in the air (z, metres)
    bool groundKnown = false;
    CVector boardVelocity;
    float slideVolume = 0.f, airVolume = 0.f, airStart = 0.f;
    SkFeedback last{};
    bool haveLast = false;
    Ground ground = kConcrete;
    bool metal = false;            // the board is on metal (rails): grinds sound like it
    ULONGLONG probeAt = 0;
    float seamTravel = 0.f, seamEvery = 1.f;
    bool flipped = false;          // a flip sound already played this air
    CVector riderVelocity;
    ULONGLONG bodyHitAt = 0, boardAt = 0;
    std::mt19937 rng{1234567u};
    std::vector<std::string> texturesSeen; // logged once each, for tuning the word lists
} g_sfx;

std::vector<int> LoadClips(const std::wstring& dir, const std::wstring& prefix) {
    std::vector<int> clips;
    for (int i = 1; i <= 16; i++) {
        int clip = g_sound.Load(dir + L"\\" + prefix + L"_" + std::to_wstring(i) + L".wav");
        if (clip < 0) break;
        clips.push_back(clip);
    }
    return clips;
}

int LoadLoop(const std::wstring& dir, const std::wstring& name) {
    return g_sound.AddLoop(g_sound.Load(dir + L"\\" + name + L".wav"));
}

void LoadSounds() {
    std::wstring dir = g_cfg.audioDir + L"\\sfx";
    SkateSounds& s = g_sfx;
    s.pop = LoadClips(dir, L"pop");
    s.flip = LoadClips(dir, L"flip");
    s.flipFast = LoadClips(dir, L"flipfast");
    s.land = LoadClips(dir, L"land");
    s.seam = LoadClips(dir, L"seam");
    s.bail = LoadClips(dir, L"bail");
    s.body = LoadClips(dir, L"body");
    s.board = LoadClips(dir, L"board");
    static const wchar_t* const rolls[kGrounds] = {L"roll_concrete", L"roll_asphalt", L"roll_brick", L"roll_grass", L"roll_wood", L"roll_metal"};
    int loops = 0;
    for (int g = 0; g < kGrounds; g++) {
        s.roll[g][0] = LoadLoop(dir, rolls[g]);
        s.roll[g][1] = LoadLoop(dir, std::wstring(rolls[g]) + L"_slow");
        loops += (s.roll[g][0] >= 0) + (s.roll[g][1] >= 0);
    }
    for (int i = 1; i <= 8; i++) {
        int concrete = LoadLoop(dir, L"grind_concrete_" + std::to_wstring(i)), steel = LoadLoop(dir, L"grind_metal_" + std::to_wstring(i));
        if (concrete >= 0) s.grinds[0].push_back(concrete);
        if (steel >= 0) s.grinds[1].push_back(steel);
    }
    s.slide = LoadLoop(dir, L"bodyslide");
    s.air = LoadLoop(dir, L"wheels_air");
    s.combo[0] = g_sound.Load(dir + L"\\combo_x2.wav");
    s.combo[1] = g_sound.Load(dir + L"\\combo_x3.wav");
    if (s.combo[0] < 0) Log("Sounds: no combo sounds (run Setup again to convert them)");
    static const wchar_t* const feelLoops[SkateSounds::kFeelLoops] = {L"speed_wind", L"speed_rattle", L"speed_tone", L"cloth_flap",
                                                                      L"powerslide", L"footbrake"};
    int feel = 0;
    for (int i = 0; i < SkateSounds::kFeelLoops; i++) feel += (s.feel[i] = LoadLoop(dir, feelLoops[i])) >= 0;
    s.whoosh = LoadClips(dir, L"whoosh");
    s.preland = LoadClips(dir, L"preland");
    s.cloth = LoadClips(dir, L"cloth");
    s.rattle = LoadClips(dir, L"rattle");
    s.squeak = LoadClips(dir, L"squeak");
    s.tumble = LoadClips(dir, L"tumble");
    Log("Sounds: %d feel loops, %zu whooshes, %zu cloth, %zu squeaks, %zu tumbles%s", feel, s.whoosh.size(), s.cloth.size(),
        s.squeak.size(), s.tumble.size(), feel ? "" : " (run Setup again to convert them)");
    s.ready = loops > 0 || !s.pop.empty();
    Log("Sounds: %d rolling loops, %zu pops, %zu flips, %zu landings, %zu+%zu grinds, %zu bails (%s)", loops, s.pop.size(),
        s.flip.size() + s.flipFast.size(), s.land.size(), s.grinds[0].size(), s.grinds[1].size(), s.bail.size() + s.body.size(),
        s.ready ? "ready" : "none found: run Setup again to convert Skate 3's audio");
}

int PickClip(const std::vector<int>& clips) {
    return clips.empty() ? -1 : clips[g_sfx.rng() % clips.size()];
}

// Skate 3's combo sound as a landing steps the multiplier up to x2 or x3
// (x1.5 is silent in Skate 3).
void PlayComboSound(float multiplier) {
    const SkateSounds& s = g_sfx;
    if (multiplier < 1.99f) return;
    if (multiplier >= 2.99f && s.combo[1] >= 0) g_sound.Play(s.combo[1], 1.1f); // user: x3 +10%
    else g_sound.Play(s.combo[0], 0.85f * 1.05f);                               // user: x2 +5%
    if (g_logSounds) Log("Sound: combo x%.1f", multiplier);
}

float Ramp(float v, float from, float to) {
    float t = std::fmin(std::fmax((v - from) / (to - from), 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
}

// Moves `v` toward `target` at `rate` per second (of the gap).
void Approach(float& v, float target, float dt, float rate) {
    v += (target - v) * std::fmin(1.f, dt * rate);
    if (std::fabs(v - target) < 0.002f) v = target;
}

// The ground under the board. San Andreas' collision surfaces have no brick,
// and paving, plazas and many roads are plain "pavement" or "default", so the
// texture drawn there decides first (the name of the render triangle under
// the board), and the collision surface only when no word in it fits.
Ground GroundFromSurface(int surface) {
    switch (surface) {
    case SURFACE_TARMAC:
    case SURFACE_TARMAC_FUCKED:
    case SURFACE_TARMAC_REALLYFUCKED:
        return kAsphalt;
    case SURFACE_GRASS_SHORT_LUSH: case SURFACE_GRASS_MEDIUM_LUSH: case SURFACE_GRASS_LONG_LUSH: case SURFACE_GRASS_SHORT_DRY:
    case SURFACE_GRASS_MEDIUM_DRY: case SURFACE_GRASS_LONG_DRY: case SURFACE_GOLFGRASS_ROUGH: case SURFACE_GOLFGRASS_SMOOTH:
    case SURFACE_STEEP_SLIDYGRASS: case SURFACE_FLOWERBED: case SURFACE_MEADOW: case SURFACE_WASTEGROUND: case SURFACE_WOODLANDGROUND:
    case SURFACE_VEGETATION: case SURFACE_MUD_WET: case SURFACE_MUD_DRY: case SURFACE_DIRT: case SURFACE_DIRTTRACK: case SURFACE_GRAVEL:
    case SURFACE_SAND_DEEP: case SURFACE_SAND_MEDIUM: case SURFACE_SAND_COMPACT: case SURFACE_SAND_ARID: case SURFACE_SAND_MORE:
    case SURFACE_SAND_BEACH: case SURFACE_HEDGE: case SURFACE_PARKGRASS:
        return kGrass;
    case SURFACE_WOOD_CRATES: case SURFACE_WOOD_SOLID: case SURFACE_WOOD_THIN: case SURFACE_WOOD_BENCH: case SURFACE_FLOORBOARD:
    case SURFACE_STAIRSWOOD: case SURFACE_WOOD_PICKET_FENCE: case SURFACE_WOOD_SLATTED_FENCE: case SURFACE_WOOD_RANCH_FENCE:
        return kWood;
    case SURFACE_GARAGE_DOOR: case SURFACE_THICK_METAL_PLATE: case SURFACE_SCAFFOLD_POLE: case SURFACE_LAMP_POST:
    case SURFACE_METAL_GATE: case SURFACE_METAL_CHAIN_FENCE: case SURFACE_GIRDER: case SURFACE_SURFACE_FIRE_HYDRANT: case SURFACE_CONTAINER:
    case SURFACE_NEWS_VENDOR: case SURFACE_CAR: case SURFACE_CAR_PANEL: case SURFACE_CAR_MOVINGCOMPONENT: case SURFACE_STAIRSMETAL:
    case SURFACE_FLOORMETAL: case SURFACE_THIN_METAL_SHEET: case SURFACE_METAL_BARREL: case SURFACE_METAL_DUMPSTER: case SURFACE_RAILTRACK:
        return kMetal;
    default:
        if (surface >= SURFACE_P_SAND && surface <= SURFACE_P_SANDBEACH) return kGrass;
        if (surface >= SURFACE_P_GRASS_SHORT && surface <= SURFACE_P_WOODDENSE) return kGrass;
        if (surface >= SURFACE_P_MOUNTAIN && surface <= SURFACE_P_SPARSEFLOWERS) return kGrass;
        if (surface >= SURFACE_P_GRASSLIGHT && surface <= SURFACE_P_GRASSDIRTMIX) return kGrass;
        return kConcrete;
    }
}

// Words in San Andreas' texture names, first match wins.
bool GroundFromTexture(std::string name, Ground& out) {
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    static const struct {
        const char* word;
        Ground ground;
    } kWords[] = {
        {"brick", kBrick}, {"cobble", kBrick}, {"paving", kBrick}, {"paver", kBrick}, {"tile", kBrick}, {"flagstone", kBrick},
        {"grass", kGrass}, {"grss", kGrass}, {"lawn", kGrass}, {"turf", kGrass}, {"weed", kGrass}, {"dirt", kGrass},
        {"mud", kGrass}, {"sand", kGrass}, {"gravel", kGrass}, {"soil", kGrass}, {"field", kGrass},
        {"sidewalk", kConcrete}, {"pavement", kConcrete}, {"kerb", kConcrete}, {"curb", kConcrete}, {"conc", kConcrete},
        {"cement", kConcrete}, {"plaza", kConcrete}, {"slab", kConcrete},
        {"road", kAsphalt}, {"tar_", kAsphalt}, {"tarmac", kAsphalt}, {"asphalt", kAsphalt}, {"street", kAsphalt},
        {"freew", kAsphalt}, {"hiway", kAsphalt}, {"highway", kAsphalt}, {"carpark", kAsphalt}, {"junction", kAsphalt},
        {"crossing", kAsphalt}, {"runway", kAsphalt},
        {"wood", kWood}, {"plank", kWood}, {"floorboard", kWood}, {"pier", kWood}, {"boardwalk", kWood},
        {"metal", kMetal}, {"steel", kMetal}, {"grate", kMetal}, {"grill", kMetal}, {"iron", kMetal},
    };
    if (name.rfind("tar", 0) == 0) { // Tar_1line256HV and friends
        out = kAsphalt;
        return true;
    }
    for (const auto& w : kWords) {
        if (name.find(w.word) != std::string::npos) {
            out = w.ground;
            return true;
        }
    }
    return false;
}

// The texture of the triangle of `atomic` under the world point p (at most
// 1.5 m below it), or null.
const char* TextureUnderAtomic(RpAtomic* atomic, const Xform& x, const CVector& p, float& bestDz) {
    RpGeometry* g = atomic ? RpAtomicGetGeometry(atomic) : nullptr;
    if (!g || !g->morphTarget || !g->morphTarget[0].verts) return nullptr;
    const RwV3d* v = g->morphTarget[0].verts;
    CVector d = p - x.pos;
    float lx = Dot(d, x.right), ly = Dot(d, x.fwd), lz = Dot(d, x.up);
    const char* best = nullptr;
    auto test = [&](unsigned ia, unsigned ib, unsigned ic, RpMaterial* material) {
        if (ia >= static_cast<unsigned>(g->numVertices) || ib >= static_cast<unsigned>(g->numVertices) ||
            ic >= static_cast<unsigned>(g->numVertices)) {
            return;
        }
        const RwV3d &a = v[ia], &b = v[ib], &c = v[ic];
        float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(det) < 1e-6f) return;
        float wa = ((b.y - c.y) * (lx - c.x) + (c.x - b.x) * (ly - c.y)) / det;
        float wb = ((c.y - a.y) * (lx - c.x) + (a.x - c.x) * (ly - c.y)) / det;
        if (wa < -0.01f || wb < -0.01f || wa + wb > 1.01f) return;
        float z = wa * a.z + wb * b.z + (1.f - wa - wb) * c.z;
        float dz = lz - z;
        if (dz < -0.3f || dz > 1.5f || dz >= bestDz) return;
        if (!material || !material->texture) return;
        bestDz = dz;
        best = material->texture->name;
    };
    if (g->triangles) {
        for (int i = 0; i < g->numTriangles; i++) {
            const RpTriangle& t = g->triangles[i];
            test(t.vertIndex[0], t.vertIndex[1], t.vertIndex[2],
                 t.matIndex < g->matList.numMaterials ? g->matList.materials[t.matIndex] : nullptr);
        }
    } else if (g->mesh) { // only the instanced meshes kept: walk them
        const RpMesh* mesh = reinterpret_cast<const RpMesh*>(g->mesh + 1);
        bool strip = (g->mesh->flags & rpMESHHEADERTRISTRIP) != 0;
        for (int m = 0; m < g->mesh->numMeshes; m++, mesh++) {
            const RxVertexIndex* idx = mesh->indices;
            if (!idx) continue;
            if (strip) {
                for (RwUInt32 i = 2; i < mesh->numIndices; i++) test(idx[i - 2], idx[i - 1], idx[i], mesh->material);
            } else {
                for (RwUInt32 i = 2; i < mesh->numIndices; i += 3) test(idx[i - 2], idx[i - 1], idx[i], mesh->material);
            }
        }
    }
    return best;
}

const char* TextureUnder(CEntity* e, const CVector& p) {
    if (!e || !e->m_pRwObject) return nullptr;
    Xform x = XformOf(e);
    float bestDz = 1e9f;
    if (e->m_pRwObject->type == rpATOMIC) return TextureUnderAtomic(reinterpret_cast<RpAtomic*>(e->m_pRwObject), x, p, bestDz);
    if (e->m_pRwObject->type != rpCLUMP) return nullptr;
    struct Search {
        const Xform* x;
        const CVector* p;
        float* bestDz;
        const char* best;
    } search{&x, &p, &bestDz, nullptr};
    RpClumpForAllAtomics(reinterpret_cast<RpClump*>(e->m_pRwObject), [](RpAtomic* a, void* data) -> RpAtomic* {
        auto* s = static_cast<Search*>(data);
        if (const char* t = TextureUnderAtomic(a, *s->x, *s->p, *s->bestDz)) s->best = t;
        return a;
    }, &search);
    return search.best;
}

void ProbeGround(const CVector& feet) {
    CColPoint cp{};
    CEntity* hit = nullptr;
    if (!CWorld::ProcessVerticalLine(CVector(feet.x, feet.y, feet.z + 0.5f), feet.z - 1.5f, cp, hit, true, true, false, true, true,
                                     false, nullptr)) {
        return; // keep the last ground
    }
    int surface = cp.m_nSurfaceTypeB;
    Ground ground = GroundFromSurface(surface);
    const char* texture = TextureUnder(hit, CVector(feet.x, feet.y, feet.z + 0.3f));
    Ground fromTexture;
    if (texture && GroundFromTexture(texture, fromTexture)) ground = fromTexture;
    g_sfx.ground = ground;
    g_sfx.metal = ground == kMetal;
    std::string name = texture ? texture : "(none)";
    if (g_sfx.texturesSeen.size() < 300 &&
        std::find(g_sfx.texturesSeen.begin(), g_sfx.texturesSeen.end(), name) == g_sfx.texturesSeen.end()) {
        g_sfx.texturesSeen.push_back(name);
        Log("Ground: texture %s, surface %d -> %s", name.c_str(), surface, kGroundNames[ground]);
    }
}

// All loops silent at once (skating stopped).
void SilenceSounds() {
    SkateSounds& s = g_sfx;
    for (int g = 0; g < kGrounds; g++) {
        for (int k = 0; k < 2; k++) {
            s.rollVolume[g][k] = 0.f;
            g_sound.SetLoop(s.roll[g][k], 0.f, 1.f);
        }
    }
    s.grindVolume = s.slideVolume = s.airVolume = 0.f;
    g_sound.SetLoop(s.grind, 0.f, 1.f);
    g_sound.SetLoop(s.slide, 0.f, 1.f);
    g_sound.SetLoop(s.air, 0.f, 1.f);
    for (int i = 0; i < SkateSounds::kFeelLoops; i++) {
        s.feelVolume[i] = 0.f;
        g_sound.SetLoop(s.feel[i], 0.f, 1.f);
    }
    s.passing.clear();
    s.haveLast = false;
    s.boardAt = 0;
}

// Cars passing close and fast whoosh by: each car's closing speed turning
// from coming nearer to going away is its closest approach.
void WhooshBys(const CVector& feet, const CVector& board) {
    SkateSounds& s = g_sfx;
    if (s.whoosh.empty() || !CPools::ms_pVehiclePool) return;
    ULONGLONG now = GetTickCount64();
    std::vector<SkateSounds::Passing> nearby;
    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
        CVehicle* car = CPools::ms_pVehiclePool->GetAt(i);
        if (!car) continue;
        CVector p = car->GetPosition() - feet;
        float distance = p.Magnitude();
        if (distance > 15.f || distance < 0.01f) continue;
        CVector v = car->m_vecMoveSpeed * 50.f - board; // relative to the skater, m/s
        float closing = -(p.x * v.x + p.y * v.y + p.z * v.z) / distance;
        auto was = std::find_if(s.passing.begin(), s.passing.end(), [car](const auto& c) { return c.car == car; });
        SkateSounds::Passing c{car, closing, was != s.passing.end() ? was->whooshedAt : 0};
        float rush = v.Magnitude();
        // Only properly fast, close passes; low and soft (user: too loud, too high).
        if (was != s.passing.end() && was->closing > 0.f && closing <= 0.f && distance < 3.5f && rush > 12.f && now > c.whooshedAt + 2000) {
            c.whooshedAt = now;
            float volume = (0.15f + 0.3f * Ramp(rush, 12.f, 30.f)) * (1.f - 0.5f * Ramp(distance, 1.5f, 3.5f));
            g_sound.Play(PickClip(s.whoosh), volume, 0.66f + 0.08f * Ramp(rush, 12.f, 30.f) + std::uniform_real_distribution<float>(-0.03f, 0.03f)(s.rng));
            if (g_logSounds) Log("Sound: whoosh-by (car %.1f m away at %.0f m/s)", distance, rush);
        }
        nearby.push_back(c);
    }
    s.passing = std::move(nearby);
}

// Skate 3's feel sounds (see SkateSounds): wind, rattle, a tone and cloth
// flapping that grow with speed, powerslide and foot-brake skids, the swell
// just before a big landing, squeaks on hard carves, cars whooshing by and
// the loose board tumbling after a bail.
void UpdateFeel(float dt, const SkFeedback& f, float speed, const CVector& feet) {
    SkateSounds& s = g_sfx;
    bool riding = f.state >= 100 && f.state <= 105, air = f.state >= 200 && f.state <= 202;
    bool grinding = f.state >= 400 && f.state <= 405, bailing = f.state == 300, onBoard = riding || air || grinding;
    ULONGLONG now = GetTickCount64();
    CVector board(f.board_velocity[0], f.board_velocity[1], f.board_velocity[2]);
    // Wind only from above top push speed (8.5 m/s), building slowly to full
    // on a very fast downhill (user, twice: too much, too high, too soon).
    float full = board.Magnitude(), rush = onBoard ? Ramp(full, 9.f, 25.f) : 0.f, level = Ramp(speed, 0.5f, 6.f);
    rush *= rush; // gentle at first
    float target[SkateSounds::kFeelLoops] = {};
    target[SkateSounds::kWind] = 0.3f * rush;
    target[SkateSounds::kFlap] = 0.18f * rush;
    float fast = Ramp(speed, 8.f, 20.f); // the speed rattle: only past top push speed too
    target[SkateSounds::kRattle] = riding && f.wheels > 0 ? 0.2f * fast * fast : 0.f;
    target[SkateSounds::kTone] = onBoard ? 0.08f * Ramp(full, 16.f, 25.f) : 0.f;
    target[SkateSounds::kSlideSkid] = f.state == 101 && speed > 0.5f ? 0.3f + 0.5f * level : 0.f;
    target[SkateSounds::kBrake] = riding && f.state != 101 && f.brake > 0.05f && speed > 0.5f ? std::fmin(1.f, f.brake) * (0.25f + 0.5f * level) : 0.f;
    const float pitch[SkateSounds::kFeelLoops] = {0.6f + 0.12f * rush, 0.75f + 0.15f * fast, 0.8f + 0.1f * rush, 0.7f + 0.15f * rush,
                                                  0.9f + 0.2f * level, 0.9f + 0.2f * level};
    for (int i = 0; i < SkateSounds::kFeelLoops; i++) {
        bool quick = i == SkateSounds::kSlideSkid || i == SkateSounds::kBrake;
        Approach(s.feelVolume[i], target[i], dt, quick ? 18.f : 5.f);
        g_sound.SetLoop(s.feel[i], s.feelVolume[i], pitch[i]);
    }

    // The swell just before a big landing, timed to peak as the wheels hit.
    if (air) {
        s.airTime += dt;
        if (now >= s.airProbeAt) {
            s.airProbeAt = now + 50;
            CColPoint cp{};
            CEntity* hit = nullptr;
            s.groundKnown = CWorld::ProcessVerticalLine(CVector(feet.x, feet.y, feet.z + 0.2f), feet.z - 40.f, cp, hit, true, true, false,
                                                        true, true, false, nullptr);
            if (s.groundKnown) s.groundBelow = cp.m_vecPoint.z;
        }
        float down = -board.z, height = feet.z - s.groundBelow;
        if (!s.prelanded && s.groundKnown && down > 3.5f && s.airTime > 0.45f && height > 0.f) {
            float t = (-down + std::sqrt(down * down + 2.f * 9.81f * height)) / 9.81f; // until the wheels touch
            if (t < 0.25f) {
                s.prelanded = true;
                g_sound.Play(PickClip(s.preland), 0.35f + 0.45f * Ramp(down, 3.5f, 10.f));
                if (g_logSounds) Log("Sound: pre-landing swell (falling %.1f m/s, %.2f s to land)", down, t);
            }
        }
    } else {
        s.airTime = 0.f;
        s.prelanded = false;
        s.groundKnown = false;
    }

    // Truck squeaks on a hard carve.
    float yaw = std::fabs(f.board_spin[2]);
    if (riding && f.wheels >= 4 && speed > 2.5f && yaw > 1.4f && now > s.squeakAt) {
        s.squeakAt = now + 1200;
        if (s.rng() % 10 < 6) {
            g_sound.Play(PickClip(s.squeak), 0.3f + 0.2f * Ramp(yaw, 1.4f, 3.f), std::uniform_real_distribution<float>(0.9f, 1.1f)(s.rng));
            if (g_logSounds) Log("Sound: truck squeak (turning %.1f rad/s)", yaw);
        }
    }

    // The board tumbling away on its own after a bail.
    if (bailing && s.haveLast) {
        float jolt = (board - s.boardVelocity).Magnitude();
        if (jolt > 2.5f && now > s.tumbleAt + 120) {
            s.tumbleAt = now;
            g_sound.Play(PickClip(s.tumble), 0.25f + 0.5f * Ramp(jolt, 2.5f, 8.f), std::uniform_real_distribution<float>(0.9f, 1.1f)(s.rng));
        }
    }
    s.boardVelocity = board;

    if (onBoard) WhooshBys(feet, board);
    else s.passing.clear();
}

void UpdateSounds(float dt, const SkFeedback& f) {
    SkateSounds& s = g_sfx;
    if (!s.ready) return;
    const float* r = g_skate.pose.root;
    CVector feet(r[12], r[13], r[14]);
    float speed = std::sqrt(f.board_velocity[0] * f.board_velocity[0] + f.board_velocity[1] * f.board_velocity[1]);
    float level = Ramp(speed, 0.3f, 8.5f);
    bool riding = f.state >= 100 && f.state <= 105, air = f.state >= 200 && f.state <= 202;
    bool grinding = f.state >= 400 && f.state <= 405, bailing = f.state == 300;
    ULONGLONG now = GetTickCount64();
    if ((riding || grinding) && now >= s.probeAt) {
        ProbeGround(feet);
        s.probeAt = now + 120;
    }

    // The wheels: the ground's loop, a slow and a fast take blended by speed,
    // faster and louder the faster the board rolls.
    bool rolling = riding && f.wheels > 0 && speed > 0.3f;
    float fast = Ramp(speed, 1.5f, 5.f), pitch = 0.82f + 0.36f * level;
    for (int g = 0; g < kGrounds; g++) {
        bool slow = s.roll[g][1] >= 0;
        for (int k = 0; k < 2; k++) {
            float target = 0.f;
            if (rolling && g == s.ground) target = kRollGain[g] * (0.3f + 0.7f * level) * (slow ? (k == 0 ? fast : 1.f - fast) : k == 0);
            Approach(s.rollVolume[g][k], target, dt, target > s.rollVolume[g][k] ? 14.f : 9.f);
            g_sound.SetLoop(s.roll[g][k], s.rollVolume[g][k], pitch);
        }
    }
    // Brick joints and sidewalk cracks clicking under the wheels.
    if (rolling && (s.ground == kBrick || s.ground == kConcrete) && !s.seam.empty()) {
        s.seamTravel += speed * dt;
        if (s.seamTravel >= s.seamEvery) {
            s.seamTravel = 0.f;
            float spacing = s.ground == kBrick ? 0.3f : 1.6f;
            s.seamEvery = spacing * std::uniform_real_distribution<float>(0.85f, 1.2f)(s.rng);
            g_sound.Play(PickClip(s.seam), (s.ground == kBrick ? 0.25f : 0.18f) + 0.3f * level, 0.9f + 0.2f * level);
        }
    }
    // The wheels spinning down in the air.
    if (air) {
        if (s.airVolume == 0.f && s.airStart == 0.f) s.airStart = 0.08f + 0.22f * level;
        s.airStart = std::fmax(0.f, s.airStart - dt * 0.12f);
    } else {
        s.airStart = 0.f;
    }
    Approach(s.airVolume, air ? s.airStart : 0.f, dt, 10.f);
    g_sound.SetLoop(s.air, s.airVolume, 0.9f + 0.3f * level);

    // Grinds: metal on rails, concrete on ledges.
    Approach(s.grindVolume, grinding ? 0.45f + 0.4f * Ramp(speed, 0.5f, 6.f) : 0.f, dt, grinding ? 20.f : 12.f);
    g_sound.SetLoop(s.grind, s.grindVolume, 0.9f + 0.2f * level);

    // A bail: the body sliding along the ground, and thumping into it.
    CVector rider(f.rider_velocity[0], f.rider_velocity[1], f.rider_velocity[2]);
    float slide = bailing && std::fabs(rider.z) < 1.5f ? Ramp(std::sqrt(rider.x * rider.x + rider.y * rider.y), 1.f, 7.f) : 0.f;
    Approach(s.slideVolume, 0.6f * slide, dt, 10.f);
    g_sound.SetLoop(s.slide, s.slideVolume, 1.f);
    if (bailing && s.haveLast) {
        float jolt = (rider - s.riderVelocity).Magnitude();
        if (jolt > 3.f && now > s.bodyHitAt + 150) {
            s.bodyHitAt = now;
            g_sound.Play(PickClip(s.body), std::fmin(1.f, 0.3f + jolt / 12.f), std::uniform_real_distribution<float>(0.9f, 1.1f)(s.rng));
        }
    }
    s.riderVelocity = rider;
    if (s.boardAt && now >= s.boardAt) { // the board clattering away after a bail
        s.boardAt = 0;
        g_sound.Play(PickClip(s.board), 0.8f);
    }

    // Moments: pops, flips, landings, grind starts, bails.
    if (s.haveLast) {
        auto vary = [&] { return std::uniform_real_distribution<float>(0.95f, 1.05f)(s.rng); };
        if (f.pops != s.last.pops) {
            g_sound.Play(PickClip(s.pop), 0.65f + 0.35f * Ramp(f.pop_speed, 1.f, 5.f), vary());
            g_sound.Play(PickClip(s.rattle), 0.3f, vary()); // the board rattling as it leaves
            g_sound.Play(PickClip(s.cloth), 0.3f, vary());
            if (g_logSounds) Log("Sound: pop (%.1f m/s up)", f.pop_speed);
        }
        if (f.dismounts != s.last.dismounts) { // Y: stepping off, the board kicked up
            g_sound.Play(PickClip(s.pop), 0.6f, vary());
            g_sound.Play(PickClip(s.cloth), 0.25f, vary());
            if (g_logSounds) Log("Sound: dismount pop");
        }
        if (f.landings != s.last.landings && f.landing_speed > 0.8f) {
            g_sound.Play(PickClip(s.land), 0.4f + 0.6f * Ramp(f.landing_speed, 1.f, 8.f), vary());
            if (g_logSounds) Log("Sound: landing (%.1f m/s) on %s", f.landing_speed, kGroundNames[s.ground]);
        }
        if (f.grinds != s.last.grinds) {
            ProbeGround(feet);
            const std::vector<int>& loops = s.grinds[s.metal && !s.grinds[1].empty() ? 1 : 0];
            if (s.grind >= 0 && s.grindVolume > 0.f) g_sound.SetLoop(s.grind, 0.f, 1.f);
            s.grind = loops.empty() ? -1 : loops[s.rng() % loops.size()];
            s.grindVolume = 0.f;
            g_sound.Play(PickClip(s.land), 0.5f, vary());
            if (g_logSounds) Log("Sound: grind (%s)", s.metal ? "metal" : "concrete");
        }
        if (f.bails != s.last.bails) {
            g_sound.Play(PickClip(s.bail), 0.5f + 0.5f * Ramp(f.bail_speed, 1.f, 9.f), vary());
            s.boardAt = now + 250;
            if (g_logSounds) Log("Sound: bail (%.1f m/s)", f.bail_speed);
        }
    }
    float spin = std::sqrt(f.board_spin[0] * f.board_spin[0] + f.board_spin[1] * f.board_spin[1] + f.board_spin[2] * f.board_spin[2]);
    if (air && !s.flipped && spin > 9.f) { // the board flipping or spinning: one whoosh per air
        s.flipped = true;
        g_sound.Play(PickClip(spin > 24.f && !s.flipFast.empty() ? s.flipFast : s.flip), 0.75f);
        g_sound.Play(PickClip(s.cloth), 0.45f); // his clothes whipping round with the trick
        if (g_logSounds) Log("Sound: flip (board spinning at %.0f rad/s)", spin);
    }
    if (!air) s.flipped = false;
    UpdateFeel(dt, f, speed, feet);
    s.last = f;
    s.haveLast = true;
}

// --------------------------------------------------------------- skate radio

// While skating the radio plays as it would in a car: GTA's stations through
// the game's own radio (it runs on foot too), and SK8-FM, Skate 3's licensed
// soundtrack (skate-audio\sk8fm, made by Setup), through our own player.
// LB + D-pad left/right changes station; Skate 3 itself only uses LB with
// D-pad up and down (its session markers), and the game's pad is blocked.
constexpr int kStationOff = 0, kStationSk8 = 100;

struct SkateRadio {
    std::vector<int> stations; // GTA's 1-12, then SK8-FM, then off
    std::vector<std::wstring> songs;
    Station sk8;
    int index = 0;             // the station picked
    int playing = -1;          // the one on now, -1 for none
    ULONGLONG tuneAt = 0;      // a new pick is tuned in after a short pause, like a car's
    ULONGLONG nameUntil = 0;   // the station's name shows until then
    WORD buttons = 0;          // the pad last frame
    int song = -1;             // SK8-FM's song, as last logged
    bool built = false;
} g_radio;

std::string StationName(int id) {
    if (id == kStationSk8) return "SK8-FM";
    if (id == kStationOff) return "Radio Off";
    const char* name = AERadioTrackManager.GetRadioStationName(static_cast<signed char>(id));
    return name && *name ? name : "Radio " + std::to_string(id);
}

void BuildStations() {
    SkateRadio& r = g_radio;
    r.built = true;
    r.songs.clear();
    for (int i = 1; i <= 99; i++) {
        wchar_t name[16];
        swprintf(name, 16, L"\\%02d.m4a", i);
        std::wstring path = g_cfg.audioDir + L"\\sk8fm" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        r.songs.push_back(path);
    }
    r.stations.clear();
    for (int id = 1; id <= 11; id++) r.stations.push_back(id);
    uint16_t userTracks = *reinterpret_cast<const uint16_t*>(0xB6B976); // AEUserRadioTrackManager.m_nUserTracksCount
    if (userTracks > 0) r.stations.push_back(12);
    if (!r.songs.empty()) r.stations.push_back(kStationSk8);
    r.stations.push_back(kStationOff);
    int want = _wcsicmp(g_cfg.radioStation.c_str(), L"sk8fm") == 0 ? kStationSk8
               : _wcsicmp(g_cfg.radioStation.c_str(), L"off") == 0 ? kStationOff
                                                                     : _wtoi(g_cfg.radioStation.c_str());
    if (want == kStationSk8 && r.songs.empty()) want = 1;
    auto it = std::find(r.stations.begin(), r.stations.end(), want);
    r.index = it != r.stations.end() ? static_cast<int>(it - r.stations.begin()) : 0;
    Log("Skate radio: %zu stations, SK8-FM has %zu songs; starting on %s", r.stations.size(), r.songs.size(),
        StationName(r.stations[r.index]).c_str());
}

// GTA's radio only plays for a player in a car: every frame it checks the
// radio settings of the player's car (CAERadioTrackManager::CheckForPause)
// and turns itself off when there are none, a quarter second after it
// starts on foot. While a GTA station plays on the board, those settings
// point at this stand-in car radio.
tVehicleAudioSettings g_boardRadio{};

void UseBoardRadio(bool on, int station) {
    tVehicleAudioSettings*& current = CAEVehicleAudioEntity::s_pVehicleAudioSettingsForRadio;
    if (on) {
        g_boardRadio.m_nRadioType = RADIO_CIVILIAN;
        g_boardRadio.m_nRadioID = static_cast<eRadioID>(station);
        g_boardRadio.m_fBassEq = 1.f;
        if (!current || current == &g_boardRadio) current = &g_boardRadio;
    } else if (current == &g_boardRadio) {
        current = nullptr;
    }
}

// Puts station `id` on (-1: nothing).
void Tune(int id) {
    SkateRadio& r = g_radio;
    if (id == r.playing) return;
    if (r.playing == kStationSk8) r.sk8.Stop(CTimer::m_snTimeInMilliseconds);
    if (id == kStationSk8 || id <= kStationOff) {
        if (r.playing > kStationOff && r.playing != kStationSk8) AudioEngine.StopRadio(nullptr, false);
        UseBoardRadio(false, 0);
    }
    r.playing = id;
    if (id == kStationSk8) {
        r.sk8.Start(g_sound, r.songs, CTimer::m_snTimeInMilliseconds);
    } else if (id > kStationOff) {
        UseBoardRadio(true, id);
        AudioEngine.StartRadio(id, 0);
    }
    Log("Skate radio: %s", id < 0 ? "off (stopped skating)" : StationName(id).c_str());
}

void StartRadio() {
    if (!g_cfg.radio) return;
    if (!g_radio.built) BuildStations();
    g_radio.tuneAt = 0;
    g_radio.nameUntil = GetTickCount64() + 2500;
    Tune(g_radio.stations[g_radio.index]);
}

void StopRadio() {
    if (g_radio.playing >= 0) Tune(-1);
}

// LB + D-pad left/right, read from the pad Skate reads.
void UpdateRadio() {
    SkateRadio& r = g_radio;
    if (!g_cfg.radio || r.stations.empty()) return;
    XINPUT_STATE pad{};
    int controller = g_api.controller(g_session);
    WORD buttons = controller >= 0 && XInputGetState(static_cast<DWORD>(controller), &pad) == ERROR_SUCCESS ? pad.Gamepad.wButtons : 0;
    WORD pressed = buttons & ~r.buttons;
    r.buttons = buttons;
    ULONGLONG now = GetTickCount64();
    if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) {
        int step = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
        if (step) {
            int n = static_cast<int>(r.stations.size());
            r.index = (r.index + step + n) % n;
            r.tuneAt = now + 600;
            r.nameUntil = now + 2500;
            int id = r.stations[r.index];
            std::wstring saved = id == kStationSk8 ? L"sk8fm" : id == kStationOff ? L"off" : std::to_wstring(id);
            WritePrivateProfileStringW(L"Skate", L"RadioStation", saved.c_str(), (g_dir + L"\\SanAnskateas.ini").c_str());
            g_cfg.radioStation = saved;
        }
    }
    if (r.tuneAt && now >= r.tuneAt) {
        r.tuneAt = 0;
        Tune(r.stations[r.index]);
    }
    int song = r.playing == kStationSk8 ? r.sk8.Song() : -1;
    if (song != r.song && song >= 0) {
        int at = static_cast<int>(r.sk8.Position() / 1000);
        Log("SK8-FM: song %d of %zu, from %d:%02d", song + 1, r.songs.size(), at / 60, at % 60);
    }
    r.song = song;
    static ULONGLONG checkAt = 0;
    if (g_logSounds && r.playing >= 0 && now >= checkAt) { // test runs: is the station still on?
        checkAt = now + 1000;
        int at = static_cast<int>(r.sk8.Position() / 1000);
        Log("Radio check: %s; GTA radio %s (station %d); SK8-FM song %d at %d:%02d", StationName(r.playing).c_str(),
            AudioEngine.IsRadioOn() ? "on" : "off", AudioEngine.GetCurrentRadioStationID(), song + 1, at / 60, at % 60);
    }
}

// ----------------------------------------------------------------- score HUD

// Skate 3's score while skating, in Grove Street green: the tricks of the
// current line as Skate 3 names them, the line's points and multiplier with
// its draining timer, points popping up as sequences land, and the session's
// total. Holding LB shows Skate 3's D-pad menu: session markers and the radio.
std::unordered_map<uint32_t, std::string> g_trickNames; // Skate 3's English texts by EA string hash

uint32_t EaHash(const char* s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; s++) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}

void LoadTrickNames() {
    std::ifstream f(g_cfg.audioDir + L"\\trick-names.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == 8) g_trickNames[static_cast<uint32_t>(strtoul(line.substr(0, 8).c_str(), nullptr, 16))] = line.substr(9);
    }
    Log("Trick names: %zu texts from Skate 3", g_trickNames.size());
}

// The text for a trick's Skate 3 label key (ID_TRICK_FLIP_KICKFLIP ->
// Kickflip), with Fakie or Switch in front as Skate 3 does.
std::string TrickName(const char* key, uint32_t stance) {
    std::string name;
    auto it = g_trickNames.find(EaHash(key));
    if (it != g_trickNames.end()) {
        name = it->second;
    } else { // no text table: make the key readable
        std::string k = key;
        for (const char* prefix : {"ID_TRICK_", "ID_", "TRICK_"}) {
            if (k.rfind(prefix, 0) == 0) k = k.substr(strlen(prefix));
        }
        bool start = true;
        for (char c : k) {
            if (c == '_') {
                name += ' ';
                start = true;
            } else {
                name += start ? c : static_cast<char>(tolower(static_cast<unsigned char>(c)));
                start = false;
            }
        }
    }
    if (name.empty()) name = "Trick";
    auto starts = [&](const char* w) { return _strnicmp(name.c_str(), w, strlen(w)) == 0; };
    if ((stance & 2) && !starts("Fakie")) name = "Fakie " + name;
    else if ((stance & 1) && !starts("Switch")) name = "Switch " + name;
    return name;
}

std::string Points(float v) {
    long long n = std::llround(std::fmax(v, 0.f));
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// A combo's points count once CJ rolls away from it on all four wheels with
// nothing still going (no manual, no grind), for this long. Skate 3 itself
// publishes each sequence as it ends, mid-combo too. Skate grades and scores
// a landing ~0.03 s after touchdown; 0.1 s more only rides out the wheels'
// bounce (user: 0.35 s made the grade pop up late).
constexpr float kComboSettle = 0.1f; // s

struct ScoreHud {
    SkScore now{}, last{};
    bool haveLast = false;
    std::vector<std::string> feed; // the combo's tricks, oldest first
    bool bailed = false;           // the feed shows a bail
    ULONGLONG feedAt = 0;          // its last change
    float total = 0.f;             // the SCORE shown: every combo landed this session
    float counted = 0.f;           // Skate's sequence points already counted (or lost) when the combo began
    bool combo = false;            // tricks or points since then
    float settle = 0.f;            // how long the board has rolled with the combo over
    float line = 0.f;              // combos counted in this line: shown with the multiplier till its timer runs out
    float glow = 0.f;              // the last line's points, lit up as it ended
    ULONGLONG glowAt = 0;          // when it ended (0: nothing lit)
    struct Popup {
        std::string text;
        const char* grade; // how the combo landed, beside its points
        CRGBA colour;
        ULONGLONG at;
        CRGBA gradeColour;
    };
    std::vector<Popup> popups;
    SkMarker marker{};
} g_hud;

const CRGBA kGrove(64, 196, 84, 255);       // Grove Street green
const CRGBA kGroveDark(18, 82, 34, 255);
const CRGBA kBailRed(214, 64, 52, 255);
const CRGBA kNeon(110, 255, 135, 255);      // the glow of a finished line
constexpr float kLineHold = 1.5f;           // s a line without a timer shows its points before it ends
constexpr float kGlowTime = 2.6f;           // s a finished line's points glow, flash and fade
constexpr float kGlowLevel = 0.6f;          // the glow's brightness (user: 40% under the first try)

const char* LandingGrade(uint32_t grade) {
    switch (grade) {
    case 1: return "Clean";
    case 2: return "Okay";
    case 3: return "Sketchy";
    default: return nullptr;
    }
}

CRGBA GradeColour(uint32_t grade) {
    return grade == 1 ? kGrove : grade == 3 ? kBailRed : CRGBA(255, 255, 255, 255);
}

void UpdateHud(float dt, const SkFeedback& f) {
    ScoreHud& h = g_hud;
    SkScore s{};
    if (g_api.score(g_session, &s) != 0) return;
    g_api.marker(g_session, &h.marker);
    ULONGLONG now = GetTickCount64();
    if (!h.haveLast) {
        h.last = s;
        h.counted = s.total;
        h.haveLast = true;
    }
    // A combo begun after the last line ran out starts a new line: a fresh
    // feed and amount.
    auto begin = [&] {
        if (!h.combo && (h.bailed || s.line_time <= 0.f)) {
            h.feed.clear();
            h.bailed = false;
            h.line = 0.f;
        }
        h.combo = true;
    };
    // The feed follows every trick as Skate 3 announces it.
    bool converted = s.converts != h.last.converts;
    if (converted && !h.feed.empty() && !h.bailed) h.feed.back() = TrickName(s.trick, s.stance);
    if (s.tricks != h.last.tricks) {
        std::string name = TrickName(s.trick, s.stance);
        begin();
        if (!(converted && !h.feed.empty() && h.feed.back() == name)) h.feed.push_back(name);
        h.feedAt = now;
    }
    if (s.total > h.counted + 0.5f && !h.combo) begin();
    // A bail, or leaving the board, loses the combo: no points, no grade.
    bool bailing = f.state == 300 || s.bails != h.last.bails;
    if (s.multiplier > h.last.multiplier + 0.01f && !bailing && g_cfg.sounds) PlayComboSound(s.multiplier);
    if (bailing || (f.state >= 500 && f.state < 600)) {
        if (h.combo && bailing) {
            h.bailed = true;
            h.feedAt = now;
            h.popups.push_back({"BAIL", nullptr, kBailRed, now, kBailRed});
        }
        h.counted = s.total;
        h.combo = false;
        h.settle = 0.f;
        h.line = 0.f;
    }
    // Rolling on away from the combo: its points count, graded by how the
    // last landing went.
    bool riding = f.state >= 100 && f.state <= 105;
    h.settle = h.combo && !s.sequence_active && riding && f.wheels >= 4 ? h.settle + dt : 0.f;
    if (h.settle >= kComboSettle) {
        float points = s.total - h.counted;
        if (points >= 1.f) {
            h.total += points;
            h.line += points;
            h.popups.push_back({"+" + Points(points), LandingGrade(f.landing_grade), kGrove, now, GradeColour(f.landing_grade)});
            if (g_logSounds) Log("Score: +%s %s (line %s)", Points(points).c_str(), LandingGrade(f.landing_grade), Points(h.line).c_str());
        }
        h.counted = s.total;
        h.combo = false;
        h.settle = 0.f;
        h.feedAt = now;
    }
    bool running = s.sequence_active || h.combo || s.line_time > 0.f;
    // The line is over (its timer ran out, or a line without one has had its
    // moment): its points light up and fade, and its tricks fade with them.
    bool timerOut = h.last.line_time > 0.f, held = now >= h.feedAt + static_cast<ULONGLONG>(kLineHold * 1000.f);
    if (!running && h.line >= 1.f && !h.bailed && (timerOut || held)) {
        h.glow = h.line;
        h.glowAt = now;
        h.line = 0.f;
        h.feedAt = now - 1500; // the feed's fade-out starts now
        if (g_logSounds) Log("Line over: %s points", Points(h.glow).c_str());
    }
    bool newPoints = h.combo && s.total - h.counted + (s.sequence_active ? s.sequence : 0.f) >= 1.f; // a new line's amount is up
    if (h.glowAt && (newPoints || now > h.glowAt + static_cast<ULONGLONG>(kGlowTime * 1000.f))) h.glowAt = 0;
    if (!running && !h.feed.empty() && now > h.feedAt + 2500) {
        h.feed.clear();
        h.bailed = false;
        h.line = 0.f;
    }
    while (!h.popups.empty() && now > h.popups.front().at + 1600) h.popups.erase(h.popups.begin());
    h.last = h.now = s;
}

// Off the board: an unfinished combo is lost; the session's SCORE stays.
void ResetHud() {
    g_hud.feed.clear();
    g_hud.popups.clear();
    g_hud.bailed = false;
    g_hud.haveLast = false;
    g_hud.combo = false;
    g_hud.settle = 0.f;
    g_hud.line = 0.f;
    g_hud.glowAt = 0;
    g_hud.marker = SkMarker{};
}

void Text(float x, float y, float size, const std::string& text, CRGBA colour, eFontAlignment align, short style = FONT_SUBTITLES,
          float u = 1.f) {
    CFont::SetFontStyle(style);
    CFont::SetProportional(true);
    CFont::SetBackground(false, false);
    CFont::SetOrientation(align);
    CFont::SetWrapx(SCREEN_WIDTH * 2.f);
    CFont::SetCentreSize(SCREEN_WIDTH * 2.f);
    CFont::SetRightJustifyWrap(0.f);
    CFont::SetScale(size * 0.5f * u, size * u);
    CFont::SetEdge(1);
    CFont::SetDropColor(CRGBA(0, 0, 0, colour.a));
    CFont::SetColor(colour);
    CFont::PrintString(x, y, text.c_str());
}

void Box(float x0, float y0, float x1, float y1, CRGBA colour) {
    CSprite2d::DrawRect(CRect(x0, y0, x1, y1), colour);
}

// Untextured triangles on screen, shaded between their corners' colours.
class Shapes {
public:
    void Tri(float ax, float ay, CRGBA ac, float bx, float by, CRGBA bc, float cx, float cy, CRGBA cc) {
        if (n_ + 3 > kMax) return;
        Put(ax, ay, ac);
        Put(bx, by, bc);
        Put(cx, cy, cc);
    }
    void Draw() {
        void *cull = nullptr, *shade = nullptr; // the triangles wind both ways; GTA draws its HUD flat-shaded
        bool gotCull = RwRenderStateGet(rwRENDERSTATECULLMODE, &cull) != 0;
        bool gotShade = RwRenderStateGet(rwRENDERSTATESHADEMODE, &shade) != 0;
        RwRenderStateSet(rwRENDERSTATECULLMODE, reinterpret_cast<void*>(rwCULLMODECULLNONE));
        RwRenderStateSet(rwRENDERSTATESHADEMODE, reinterpret_cast<void*>(rwSHADEMODEGOURAUD));
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, reinterpret_cast<void*>(TRUE));
        RwRenderStateSet(rwRENDERSTATESRCBLEND, reinterpret_cast<void*>(rwBLENDSRCALPHA));
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, reinterpret_cast<void*>(rwBLENDINVSRCALPHA));
        RwIm2DRenderPrimitive(rwPRIMTYPETRILIST, v_, n_);
        if (gotCull) RwRenderStateSet(rwRENDERSTATECULLMODE, cull);
        if (gotShade) RwRenderStateSet(rwRENDERSTATESHADEMODE, shade);
    }

private:
    static constexpr int kMax = 96;
    RwIm2DVertex v_[kMax];
    int n_ = 0;
    void Put(float x, float y, CRGBA c) {
        RwIm2DVertex& p = v_[n_++];
        RwIm2DVertexSetScreenX(&p, x);
        RwIm2DVertexSetScreenY(&p, y);
        RwIm2DVertexSetScreenZ(&p, CSprite2d::NearScreenZ);
        RwIm2DVertexSetRecipCameraZ(&p, CSprite2d::RecipNearClip);
        RwIm2DVertexSetU(&p, 0.f, CSprite2d::RecipNearClip);
        RwIm2DVertexSetV(&p, 0.f, CSprite2d::RecipNearClip);
        RwIm2DVertexSetIntRGBA(&p, c.r, c.g, c.b, c.a);
    }
};

// A soft light: solid over the box, fading to nothing `pad` beyond it, with
// rounded corners.
void SoftLight(float x0, float y0, float x1, float y1, float pad, CRGBA c) {
    CRGBA o(c.r, c.g, c.b, 0);
    Shapes s;
    s.Tri(x0, y0, c, x1, y0, c, x1, y1, c);
    s.Tri(x0, y0, c, x1, y1, c, x0, y1, c);
    s.Tri(x0, y0 - pad, o, x1, y0 - pad, o, x1, y0, c); // above
    s.Tri(x0, y0 - pad, o, x1, y0, c, x0, y0, c);
    s.Tri(x0, y1, c, x1, y1, c, x1, y1 + pad, o); // below
    s.Tri(x0, y1, c, x1, y1 + pad, o, x0, y1 + pad, o);
    s.Tri(x0 - pad, y0, o, x0, y0, c, x0, y1, c); // left
    s.Tri(x0 - pad, y0, o, x0, y1, c, x0 - pad, y1, o);
    s.Tri(x1, y0, c, x1 + pad, y0, o, x1 + pad, y1, o); // right
    s.Tri(x1, y0, c, x1 + pad, y1, o, x1, y1, c);
    const float corners[4][3] = {{x0, y0, 3.14159f}, {x1, y0, 4.71239f}, {x1, y1, 0.f}, {x0, y1, 1.5708f}};
    for (const auto& k : corners) { // a quarter circle fading out from each corner
        for (int i = 0; i < 4; i++) {
            float a0 = k[2] + i * 0.392699f, a1 = a0 + 0.392699f;
            s.Tri(k[0], k[1], c, k[0] + pad * std::cos(a0), k[1] + pad * std::sin(a0), o, k[0] + pad * std::cos(a1),
                  k[1] + pad * std::sin(a1), o);
        }
    }
    s.Draw();
}

// Points lit like a neon sign, centred on x: a green light behind them, a
// halo of green copies around them and a pale core with a green rim.
// `light` is how bright the glow is (1 steady, more in a flash), `alpha` how
// visible it all is.
void NeonText(float x, float y, float size, const std::string& text, float light, float alpha, float u) {
    auto a = [&](float v) { return static_cast<unsigned char>(std::fmin(255.f, std::fmax(0.f, v * alpha))); };
    CFont::SetFontStyle(FONT_PRICEDOWN);
    CFont::SetProportional(true);
    CFont::SetBackground(false, false);
    CFont::SetOrientation(ALIGN_CENTER);
    CFont::SetWrapx(SCREEN_WIDTH * 2.f);
    CFont::SetCentreSize(SCREEN_WIDTH * 2.f);
    CFont::SetRightJustifyWrap(0.f);
    CFont::SetScale(size * 0.5f * u, size * u);
    float halfW = CFont::GetStringWidth(text.c_str(), true) * 0.5f, height = 18.f * size * u;
    // The light behind, brightest on the text: a wide faint one, a tight bright one.
    float y0 = y + height * 0.15f, y1 = y + height * 0.85f;
    SoftLight(x - halfW, y0, x + halfW, y1, 26.f * u * light, CRGBA(kNeon.r, kNeon.g, kNeon.b, a(34.f * kGlowLevel * light)));
    SoftLight(x - halfW * 0.8f, y0, x + halfW * 0.8f, y1, 10.f * u, CRGBA(kNeon.r, kNeon.g, kNeon.b, a(48.f * kGlowLevel * light)));
    // The halo: copies around the text, fainter further out.
    CFont::SetEdge(0);
    const float ring[2] = {1.6f * u, 3.4f * u}, ringAlpha[2] = {95.f, 38.f};
    for (int r = 0; r < 2; r++) {
        CFont::SetColor(CRGBA(kNeon.r, kNeon.g, kNeon.b, a(ringAlpha[r] * kGlowLevel * std::fmin(light, 1.6f))));
        for (int i = 0; i < 8; i++) {
            float angle = i * 0.785398f;
            CFont::PrintString(x + ring[r] * std::cos(angle), y + ring[r] * std::sin(angle), text.c_str());
        }
    }
    // The core: black letters with a neon-green rim (user's call).
    CFont::SetEdge(1);
    CFont::SetDropColor(CRGBA(kNeon.r, kNeon.g, kNeon.b, a(255.f)));
    CFont::SetColor(CRGBA(0, 0, 0, a(255.f)));
    CFont::PrintString(x, y, text.c_str());
}

// Skate 3's D-pad menu while LB is held, in the bottom right corner:
// session markers up and down, the radio left and right.
void DrawMarkerMenu(float screenU) {
    const SkMarker& m = g_hud.marker;
    float u = screenU * 0.8f; // 80% of its first size
    float cx = SCREEN_WIDTH - 80.f * screenU, cy = SCREEN_HEIGHT - 75.f * screenU, cell = 26.f * u, gap = 3.f * u;
    struct Arm {
        float dx, dy;
        std::string label;
        bool enabled;
        eFontAlignment align;
    } arms[4] = {
        {0, -1, "Go to marker (hold)", m.can_return != 0, ALIGN_CENTER},
        {0, 1, "Set marker", m.can_place != 0, ALIGN_CENTER},
        {-1, 0, "Station", true, ALIGN_RIGHT},
        {1, 0, "Station", true, ALIGN_LEFT},
    };
    Box(cx - cell * 0.5f, cy - cell * 0.5f, cx + cell * 0.5f, cy + cell * 0.5f, CRGBA(10, 30, 14, 170));
    for (const Arm& a : arms) {
        float x = cx + a.dx * (cell + gap), y = cy + a.dy * (cell + gap);
        CRGBA fill = a.enabled ? CRGBA(kGrove.r, kGrove.g, kGrove.b, 215) : CRGBA(70, 70, 70, 170);
        Box(x - cell * 0.5f, y - cell * 0.5f, x + cell * 0.5f, y + cell * 0.5f, CRGBA(0, 0, 0, 200));
        Box(x - cell * 0.5f + gap * 0.5f, y - cell * 0.5f + gap * 0.5f, x + cell * 0.5f - gap * 0.5f, y + cell * 0.5f - gap * 0.5f, fill);
        // The arrow: a triangle pointing out from the middle.
        float t = cell * 0.22f, sx = -a.dy, sy = a.dx;
        float tipX = x + a.dx * t, tipY = y + a.dy * t, baseX = x - a.dx * t * 0.7f, baseY = y - a.dy * t * 0.7f;
        CSprite2d::Draw2DPolygon(tipX, tipY, tipX, tipY, baseX + sx * t, baseY + sy * t, baseX - sx * t, baseY - sy * t,
                                 CRGBA(255, 255, 255, 255));
        CRGBA labelColour = a.enabled ? CRGBA(255, 255, 255, 255) : CRGBA(150, 150, 150, 255);
        float lx = x + a.dx * (cell * 0.5f + 6.f * u), ly = y + a.dy * (cell * 0.5f + 4.f * u) - 7.f * u;
        if (a.dy > 0) ly = y + cell * 0.5f + 4.f * u;
        if (a.dy < 0) ly = y - cell * 0.5f - 18.f * u;
        Text(lx, ly, 0.7f, a.label, labelColour, a.align, FONT_SUBTITLES, u);
    }
    if (m.progress > 0.f) { // holding to go back to the marker
        float x0 = cx - cell * 0.5f, y = cy - (cell + gap) - cell * 0.5f - 3.f * u;
        Box(x0, y, x0 + cell, y + 2.f * u, CRGBA(0, 0, 0, 200));
        Box(x0, y, x0 + cell * std::fmin(m.progress, 1.f), y + 2.f * u, CRGBA(255, 255, 255, 255));
    }
    if (g_cfg.radio && !g_radio.stations.empty()) {
        Text(cx, cy + (cell + gap) * 1.5f + 20.f * u, 0.8f, StationName(g_radio.stations[g_radio.index]), kGrove, ALIGN_CENTER,
             FONT_MENU, u);
    }
}

void DrawHud() {
    if (!g_skate.active || FrontEndMenuManager.m_bMenuActive || TheCamera.m_bWideScreenOn || CCutsceneMgr::ms_running) return;
    float W = SCREEN_WIDTH, H = SCREEN_HEIGHT, u = H / 448.f;
    ULONGLONG now = GetTickCount64();
    const ScoreHud& h = g_hud;
    if (g_cfg.radio && now < g_radio.nameUntil && !g_radio.stations.empty()) { // the station, where GTA shows it in a car
        Text(W * 0.5f, 20.f * u, 1.0f, StationName(g_radio.stations[g_radio.index]), g_radio.tuneAt ? CRGBA(150, 150, 150, 255) : kGrove,
             ALIGN_CENTER, FONT_MENU, u);
    }
    if (g_hud.marker.visible) DrawMarkerMenu(u);
    if (!g_cfg.hud) return;
    // The session's points, under GTA's money (which ends ~77 units down at
    // 1280x720, ~85 at the user's 2560x1440), or under the wanted stars
    // (~81-97 / ~105) while there are any.
    CWanted* wanted = FindPlayerWanted();
    float scoreY = (wanted && wanted->m_nWantedLevel > 0 ? 118.f : 96.f) * u;
    Text(W - 30.f * u, scoreY, 0.75f, "SCORE", CRGBA(255, 255, 255, 230), ALIGN_RIGHT, FONT_SUBTITLES, u);
    Text(W - 30.f * u, scoreY + 12.f * u, 1.1f, Points(h.total), kGrove, ALIGN_RIGHT, FONT_PRICEDOWN, u);
    // The combo: its tricks, its points so far and multiplier, and the line timer.
    float fade = 1.f;
    bool running = h.now.sequence_active || h.combo || h.now.line_time > 0.f;
    if (!running && !h.feed.empty()) fade = 1.f - Ramp(static_cast<float>(now - h.feedAt), 1500.f, 2500.f);
    auto alpha = [&](CRGBA c) { return CRGBA(c.r, c.g, c.b, static_cast<unsigned char>(c.a * fade)); };
    float baseY = H - 92.f * u;
    // Under the tricks: the line's amount, then (a little apart) its timer bar.
    const float amountY = baseY + 20.f * u, barY = baseY + 46.f * u;
    bool amountShown = false;
    if (!h.feed.empty() && fade > 0.f) {
        std::string line;
        size_t first = h.feed.size() > 4 ? h.feed.size() - 4 : 0;
        if (first) line = "... + ";
        for (size_t i = first; i < h.feed.size(); i++) line += (i > first ? " + " : "") + h.feed[i];
        Text(W * 0.5f, baseY, 1.0f, line, alpha(h.bailed ? kBailRed : CRGBA(255, 255, 255, 255)), ALIGN_CENTER, FONT_SUBTITLES, u);
        // The line's amount: combos counted in it, plus the one under way.
        float points = h.line + (h.combo ? h.now.total - h.counted + (h.now.sequence_active ? h.now.sequence : 0.f) : 0.f);
        if (points >= 1.f && !h.bailed) {
            std::string score = Points(points);
            if (h.now.multiplier > 1.01f) {
                char mult[16];
                snprintf(mult, sizeof mult, "  x%.1f", h.now.multiplier);
                score += mult;
            }
            Text(W * 0.5f, amountY, 1.25f, score, alpha(kGrove), ALIGN_CENTER, FONT_PRICEDOWN, u);
            amountShown = true;
        }
    }
    // A finished line's points: a quick flash, then a neon glow fading away
    // (gone at once if a new line's amount takes their place).
    if (h.glowAt && !amountShown) {
        float t = static_cast<float>(now - h.glowAt) / 1000.f;
        float flash = t < 0.06f ? t / 0.06f : std::exp(-(t - 0.06f) * 7.f);
        float shown = 1.f - Ramp(t, 0.8f, kGlowTime);
        float size = 1.25f * (1.f + 0.08f * flash), centreY = amountY + 9.f * 1.25f * u;
        if (flash > 0.05f) { // the flash: a pale burst spreading out behind the points
            float halfW = 40.f * u + 30.f * u * (1.f - flash), halfH = 8.f * u + 6.f * u * (1.f - flash);
            SoftLight(W * 0.5f - halfW, centreY - halfH, W * 0.5f + halfW, centreY + halfH, 22.f * u,
                      CRGBA(215, 255, 220, static_cast<unsigned char>(110.f * kGlowLevel * flash)));
        }
        NeonText(W * 0.5f, centreY - 9.f * size * u - 6.f * u * t / kGlowTime, size, Points(h.glow), 1.f + 0.9f * flash, shown, u);
    }
    if (h.now.line_time > 0.f) { // the line timer: keep doing tricks before it runs out
        float half = 70.f * u, y = barY, edge = std::fmax(1.f, 1.f * u);
        Box(W * 0.5f - half - edge, y - edge, W * 0.5f + half + edge, y + 3.f * u + edge, CRGBA(0, 0, 0, 255)); // outline
        Box(W * 0.5f - half, y, W * 0.5f + half, y + 3.f * u, CRGBA(kGroveDark.r, kGroveDark.g, kGroveDark.b, 255));
        Box(W * 0.5f - half, y, W * 0.5f - half + 2.f * half * h.now.line_time, y + 3.f * u, kGrove);
    }
    for (const auto& p : h.popups) { // points rising as a combo lands, how it landed beside them
        float t = static_cast<float>(now - p.at) / 1600.f;
        CRGBA c = p.colour;
        c.a = static_cast<unsigned char>(255.f * (1.f - Ramp(t, 0.6f, 1.f)));
        float y = baseY - 22.f * u - 18.f * u * t;
        Text(W * 0.5f, y, 1.1f, p.text, c, ALIGN_CENTER, FONT_PRICEDOWN, u);
        if (p.grade) {
            float half = CFont::GetStringWidth(p.text.c_str(), true) * 0.5f; // in the font just set
            Text(W * 0.5f + half + 8.f * u, y + 3.f * u, 0.85f, p.grade, CRGBA(p.gradeColour.r, p.gradeColour.g, p.gradeColour.b, c.a),
                 ALIGN_LEFT, FONT_SUBTITLES, u);
        }
    }
    CFont::DrawFonts();
}

// The mix's volumes follow the game's own settings; it holds still while
// the game is paused or in the background.
void UpdateAudioMix() {
    if (!g_sound.Ready()) return;
    float sfx = FrontEndMenuManager.m_nPrefsSfxVolume / 64.f * g_cfg.soundVolume;
    float music = FrontEndMenuManager.m_nPrefsMusicVolume / 64.f * 0.7f; // GTA's radio plays 4 dB under the music volume
    g_sound.SetVolumes(sfx, music);
    g_sound.SetPaused(g_windowPaused || FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CTimer::m_CodePause);
    g_sound.Update();
}

void StartAudio() {
    std::string error = g_sound.Start();
    if (!error.empty()) {
        Log("Sound: %s; skating stays silent", error.c_str());
        return;
    }
    if (g_cfg.sounds) LoadSounds();
    std::string mf = Station::Load();
    if (!mf.empty()) Log("SK8-FM: %s", mf.c_str());
    LoadTrickNames();
}

// What CJ keeps of the board's motion when skating stops.
enum class Exit {
    Freeze,   // nothing: script warps, cutscenes, errors
    Momentum, // the board's velocity: deaths, knockdowns
    Hop,      // the velocity, plus a hop if rolling on the ground: getting off
};

void Stop(const char* reason, bool touchPed, Exit exit = Exit::Freeze) {
    if (!g_skate.active) return;
    g_skate.active = false;
    std::string state = g_api.state(g_session);
    g_api.suspend_input(g_session);
    CPlayerPed* ped = g_skate.ped;
    g_skate.ped = nullptr;
    CVector v(0.f, 0.f, 0.f);
    // PhysicsGround, SlideGround (powerslides), GroundAnimation...; KnownAir is airborne.
    bool grounded = state.find("Ground") != std::string::npos;
    if (touchPed && ped) {
        ped->bDisableCollisionForce = g_skate.savedDisableCollisionForce;
        ped->bDontApplySpeed = g_skate.savedDontApplySpeed;
        if (g_cfg.carryMomentum && exit != Exit::Freeze) {
            const float* board = g_skate.pose.velocity;
            v = CVector(board[0], board[1], board[2]);
            float horizontal = std::sqrt(v.x * v.x + v.y * v.y);
            // SA overwrites a standing ped's speed every frame from his
            // animation, so momentum only survives while he's airborne:
            // mid-air he stays airborne; rolling fast he hops off.
            bool hop = exit == Exit::Hop && grounded && horizontal > g_cfg.hopSpeed;
            if (hop) v.z = std::fmax(v.z, g_cfg.hopUp);
            if (hop || !grounded) {
                ped->bIsStanding = false;
                ped->bWasStanding = false;
            }
        }
        ped->m_vecMoveSpeed = v * (1.f / 50.f); // SA speeds are per 50 fps frame
    }
    StopCamera(touchPed && exit != Exit::Freeze, !grounded);
    SilenceSounds();
    StopRadio();
    ResetHud();
    Log("Skating stopped: %s (state %s, CJ leaves at %.1f %.1f %.1f m/s)", reason, state.c_str(), v.x, v.y, v.z);
}

void Start(CPlayerPed* ped) {
    CVector pos = ped->GetPosition();
    CVector vel = ped->m_vecMoveSpeed * 50.f; // m/s
    bool airborne = !ped->bIsStanding;
    bool found = false;
    float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 1.f, &found, nullptr);
    float floorZ = found ? ground : pos.z - g_cfg.feetOffset;
    float feetZ = airborne ? pos.z - g_cfg.feetOffset : floorZ; // jumping on mid-air lands on the board
    if (!InstallWorldAt(CVector(pos.x, pos.y, floorZ), feetZ)) {
        Message("Skate failed to start (see SanAnskateas.log).");
        return;
    }
    float horizontal = std::sqrt(vel.x * vel.x + vel.y * vel.y);
    bool carry = g_cfg.carryMomentum && (horizontal > 0.5f || airborne);
    // Moving: point the board the way CJ is going, so his speed rolls it forward.
    float yaw = carry && horizontal > 1.f ? std::atan2(vel.y, vel.x)
                                          : ped->GetHeading() + kPi * 0.5f; // SA heading 0 faces +Y
    float spawn[3] = {pos.x, pos.y, feetZ + 0.05f};
    if (!Check(g_api.activate(g_session, spawn, yaw), "sk_activate") ||
        !Check(g_api.get_pose(g_session, &g_skate.pose), "sk_get_pose")) {
        Message("Skate failed to start (see SanAnskateas.log).");
        return;
    }
    if (carry) {
        float along = vel.x * std::cos(yaw) + vel.y * std::sin(yaw); // boards don't roll sideways
        float v[3] = {along * std::cos(yaw), along * std::sin(yaw), airborne ? vel.z : 0.f};
        Check(g_api.set_velocity(g_session, v), "sk_set_velocity");
    }
    g_skate.ped = ped;
    g_skate.savedDisableCollisionForce = ped->bDisableCollisionForce;
    g_skate.savedDontApplySpeed = ped->bDontApplySpeed;
    // Collision stays on, so bullets, fists and cars still hit CJ; they just
    // can't push him off the skater's path.
    ped->bDisableCollisionForce = true;
    ped->bDontApplySpeed = true; // what FREEZE_CHAR_POSITION does
    // End whatever CJ was doing (a jump, a fall, a crouch) the way scripts
    // do with CLEAR_CHAR_TASKS_IMMEDIATELY, so nothing stale plays later.
    if (ped->m_pIntelligence) ped->m_pIntelligence->FlushImmediately(true);
    ped->bIsInTheAir = false;
    ped->bIsLanding = false;
    g_skate.active = true;
    if (!g_rig.ready || ped->m_pRwClump != g_rig.clump) SetupRig(ped);
    StartCamera();
    SilenceSounds();
    ResetHud();
    StartRadio();
    g_skate.padCheckAt = GetTickCount64() + 1000;
    g_skate.padChecked = false;
    PlaceCJ(ped);
    Log("Skating started at %.2f %.2f %.2f (ground %s, %s), CJ speed %.1f m/s, heading %.2f", pos.x, pos.y, feetZ,
        found ? "found" : "guessed", airborne ? "airborne" : "standing", horizontal, ped->GetHeading());
    Message("Skating. Press J or L3 + R3 to get off.");
}

// Car hits. Stopped and slow cars (under kCarMovingSpeed) are part of Skate's
// world, so riding into one is Skate's own collision. Moving ones can't be
// (the world is rebuilt in the background), so the plugin watches them: when
// a moving car's own collision (its spheres, boxes and body mesh) touches the
// skater (spheres up his spine), now or within this frame's travel, Skate
// bails him its own way (its vehicle ejection) and respawns him after. Slower
// than kRollOverSpeed he's thrown along the car's way and off to the side he
// was on; faster, the car sweeps his legs and he goes up over it, tumbling
// back, while it carries on under him. Moving cars near him are told to ignore CJ
// in the game's own collision (set each frame; the game clears it once they're
// apart): to it the frozen CJ is a post, and it stopped cars dead on him.
constexpr float kRollOverSpeed = 9.f;      // m/s (about 20 mph)
constexpr float kBodyRadius = 0.2f;        // m, the spheres up his spine
constexpr float kKnockShare = 1.0f;        // of the car's speed relative to him, along its way...
constexpr float kKnockSide = 0.6f;         // ...and sideways, out of its path
constexpr float kKnockUp = 2.5f;           // m/s upward, so he's thrown, not dragged
constexpr float kRollShare = 0.5f;         // of the car's speed he keeps going up over it...
constexpr float kRollPass = 5.f;           // ...but it passes under him at least this fast (m/s)
constexpr float kRollClearance = 0.4f;     // m between its top and his lowest point (arms, tumbling)
constexpr float kRollMaxRise = 8.f;        // m/s up at most (about 3 m)
constexpr float kRollSpin = 6.f;           // rad/s, tumbling back over it (his joints soak up half)
constexpr float kBoardSide = 3.f;          // m/s, the board out of the car's way
constexpr float kGravity = 9.8f;           // m/s^2, Skate's
constexpr float kCarKeeps = 0.8f;          // of its speed, as if it spent the rest on him
constexpr ULONGLONG kHitCooldownMs = 2500; // one knock per bail

// The point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5).
CVector ClosestOnTriangle(const CVector& p, const CVector& a, const CVector& b, const CVector& c) {
    CVector ab = b - a, ac = c - a, ap = p - a;
    float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) return a;
    CVector bp = p - b;
    float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return a + ab * (d1 / (d1 - d3));
    CVector cp = p - c;
    float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && d4 - d3 >= 0.f && d5 - d6 >= 0.f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Whether a sphere at p (the model's own space) touches the collision.
bool TouchesCollision(const CCollisionData& d, const CVector& p, float r) {
    for (int i = 0; i < d.m_nNumSpheres; i++) {
        if ((p - d.m_pSpheres[i].m_vecCenter).Magnitude() < r + d.m_pSpheres[i].m_fRadius) return true;
    }
    for (int i = 0; i < d.m_nNumBoxes; i++) {
        const CVector &lo = d.m_pBoxes[i].m_vecMin, &hi = d.m_pBoxes[i].m_vecMax;
        CVector q(std::fmin(std::fmax(p.x, lo.x), hi.x), std::fmin(std::fmax(p.y, lo.y), hi.y), std::fmin(std::fmax(p.z, lo.z), hi.z));
        if ((p - q).Magnitude() < r) return true;
    }
    const auto* v = reinterpret_cast<const PackedVertex*>(d.m_pVertices);
    auto vertex = [&](unsigned i) { return CVector(v[i].x / 128.f, v[i].y / 128.f, v[i].z / 128.f); };
    for (int i = 0; v && i < d.m_nNumTriangles; i++) {
        const CColTriangle& t = d.m_pTriangles[i];
        if ((p - ClosestOnTriangle(p, vertex(t.m_nVertA), vertex(t.m_nVertB), vertex(t.m_nVertC))).Magnitude() < r) return true;
    }
    return false;
}

// The height (world z) of the car's collision surface over p: its hood, roof
// or whatever of its spheres, boxes and body mesh is highest there. False if
// none of it is under p.
bool CarTopAt(CVehicle* car, const CVector& p, float& z) {
    CColModel* col = car->GetColModel();
    const CMatrix& m = *car->m_matrix;
    CVector d = p - m.GetPosition();
    float x = Dot(d, m.GetRight()), y = Dot(d, m.GetForward()), top = -1e9f;
    if (const CCollisionData* data = col ? col->m_pColData : nullptr) {
        for (int i = 0; i < data->m_nNumSpheres; i++) {
            const CColSphere& s = data->m_pSpheres[i];
            float dx = x - s.m_vecCenter.x, dy = y - s.m_vecCenter.y, h = s.m_fRadius * s.m_fRadius - dx * dx - dy * dy;
            if (h > 0.f) top = std::fmax(top, s.m_vecCenter.z + std::sqrt(h));
        }
        for (int i = 0; i < data->m_nNumBoxes; i++) {
            const CVector &lo = data->m_pBoxes[i].m_vecMin, &hi = data->m_pBoxes[i].m_vecMax;
            if (x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y) top = std::fmax(top, hi.z);
        }
        const auto* v = reinterpret_cast<const PackedVertex*>(data->m_pVertices);
        auto vertex = [&](unsigned i) { return CVector(v[i].x / 128.f, v[i].y / 128.f, v[i].z / 128.f); };
        for (int i = 0; v && i < data->m_nNumTriangles; i++) {
            const CColTriangle& t = data->m_pTriangles[i];
            CVector a = vertex(t.m_nVertA), b = vertex(t.m_nVertB), c = vertex(t.m_nVertC);
            float det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::fabs(det) < 1e-6f) continue;
            float wa = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / det;
            float wb = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / det;
            if (wa >= 0.f && wb >= 0.f && wa + wb <= 1.f) top = std::fmax(top, wa * a.z + wb * b.z + (1.f - wa - wb) * c.z);
        }
    }
    if (top < -1e8f) return false;
    z = m.GetPosition().z + top * m.GetUp().z; // a car on its wheels, near enough
    return true;
}

// Whether the car's collision touches spheres up the skater's spine, now or
// after `ahead` seconds at their relative velocity.
bool CarTouches(CVehicle* car, const CVector& feet, const CVector& relative, float ahead) {
    CColModel* col = car->GetColModel();
    if (!col || !col->m_pColData || !car->m_matrix) return false;
    const CMatrix& m = *car->m_matrix;
    for (float t : {0.f, ahead}) {
        for (float h = 0.25f; h < 1.8f; h += 0.3f) {
            CVector d = feet + CVector(0.f, 0.f, h) - relative * t - m.GetPosition();
            CVector local(Dot(d, m.GetRight()), Dot(d, m.GetForward()), Dot(d, m.GetUp()));
            if ((local - col->m_boundSphere.m_vecCenter).Magnitude() > col->m_boundSphere.m_fRadius + kBodyRadius) continue;
            if (TouchesCollision(*col->m_pColData, local, kBodyRadius)) return true;
        }
    }
    return false;
}

// After an engine error (it once went NaN mid-air), put the skater back on
// the board where he was, with his speed, rather than ending the session. A
// second error within kRecoverMs ends it.
constexpr ULONGLONG kRecoverMs = 5000;
bool RecoverEngine(CPlayerPed* ped) {
    ULONGLONG now = GetTickCount64();
    if (now < g_skate.recoveredAt + kRecoverMs) return false;
    g_skate.recoveredAt = now;
    const float* r = g_skate.pose.root;
    const float* v = g_skate.pose.velocity;
    float at[3] = {r[12], r[13], r[14] + 0.1f};
    if (!std::isfinite(at[0] + at[1] + at[2])) {
        CVector p = ped->GetPosition();
        at[0] = p.x, at[1] = p.y, at[2] = p.z - g_cfg.feetOffset + 0.1f;
    }
    float velocity[3] = {0.f, 0.f, 0.f};
    if (std::isfinite(v[0] + v[1] + v[2])) velocity[0] = v[0], velocity[1] = v[1], velocity[2] = v[2];
    if (!Check(g_api.activate(g_session, at, ped->GetHeading() + kPi * 0.5f), "sk_activate") ||
        !Check(g_api.set_velocity(g_session, velocity), "sk_set_velocity") ||
        !Check(g_api.get_pose(g_session, &g_skate.pose), "sk_get_pose")) {
        return false;
    }
    Log("Recovered from the engine error: skater put back on the board at %.1f %.1f %.1f", at[0], at[1], at[2]);
    return true;
}

// Ctrl + the toggle key: restart the Skate engine without restarting the game,
// for when it misbehaves. Skating stops first, everything tied to the old
// engine (CJ's rig, the board, the world) is set up again for the new one,
// and the old engine is freed and a new one loaded on a background thread, as
// at startup (about 10 s). Freeing waits for the engine's thread, so it must
// not be the game's thread in case the engine is stuck.
void RestartEngine() {
    if (g_load.load(std::memory_order_acquire) == Load::Loading) {
        Message("Skate 3 is still loading...");
        return;
    }
    if (g_skate.active) Stop("restarting the Skate engine", true);
    g_rig.ready = false;
    for (BoardPart& part : g_board.parts) {
        if (part.texture) part.texture->Release();
    }
    g_board = Board{};
    g_world.pending = 0;
    g_world.hash = 0;
    g_world.cars.clear();
    g_readyAnnounced = false;
    SkSession* old = g_session;
    g_session = nullptr;
    g_load.store(Load::Loading, std::memory_order_release);
    Log("Restarting the Skate engine");
    Message("Restarting Skate 3...");
    std::thread([old] {
        if (old) g_api.session_free(old);
        LoadEngine();
    }).detach();
}

void CheckCarHits() {
    ULONGLONG now = GetTickCount64();
    CPlayerPed* ped = g_skate.ped;
    if (!ped || !CPools::ms_pVehiclePool) return;
    // A car he's going over carries on under him until he's past its tail
    // (it may brake, and he'd come down through it: it isn't in Skate's world).
    if (CVehicle* car = g_skate.underCar) {
        if (CTimer::m_snTimeInMilliseconds < g_skate.underUntil && CPools::ms_pVehiclePool->IsObjectValid(car)) {
            car->m_vecMoveSpeed.x = g_skate.underSpeed.x;
            car->m_vecMoveSpeed.y = g_skate.underSpeed.y;
        } else {
            g_skate.underCar = nullptr;
        }
    }
    const float* r = g_skate.pose.root;
    const float* sv = g_skate.pose.velocity;
    CVector feet(r[12], r[13], r[14]), skaterVelocity(sv[0], sv[1], sv[2]), up(0.f, 0.f, 1.f);
    const char* state = g_api.state(g_session);
    // Not on top of a bail already under way.
    bool canHit = now >= g_skate.hitsFrom && !strstr(state, "Wipeout") && !strstr(state, "Respawn");
    float frame = CTimer::ms_fTimeStep / 50.f; // s
    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
        CVehicle* car = CPools::ms_pVehiclePool->GetAt(i);
        if (!car || !car->m_matrix) continue;
        CVector centre = car->GetPosition();
        if ((centre - feet).Magnitude() > 15.f) continue;
        CVector carVelocity = car->m_vecMoveSpeed * 50.f; // m/s
        float carSpeed = carVelocity.Magnitude();
        if (carSpeed < kCarMovingSpeed) continue; // part of Skate's world
        if (!car->m_pEntityIgnoredCollision || car->m_pEntityIgnoredCollision == ped) car->m_pEntityIgnoredCollision = ped;
        CVector relative = carVelocity - skaterVelocity;
        if (!canHit || relative.Magnitude() < kCarMovingSpeed || !CarTouches(car, feet, relative, frame)) continue;
        CVector along = Normalised(CVector(relative.x, relative.y, 0.f), CVector(1.f, 0.f, 0.f));
        float speed = CVector(relative.x, relative.y, 0.f).Magnitude();
        CVector knock, spin, boardKnock;
        float lift = 0.f, passTime = 0.f;
        bool over = false;
        if (carSpeed >= kRollOverSpeed) {
            // The bumper sweeps his legs: he starts on the hood (just behind
            // its front, under him) and goes up over the car, tumbling back,
            // while it passes under him. He rises just enough to stay clear
            // of its top (hood, windscreen, roof) all the way to its tail; a
            // car too tall or long for that (a bus) throws him instead.
            CVector carDir = Normalised(CVector(carVelocity.x, carVelocity.y, 0.f), along);
            const CMatrix& m = *car->m_matrix;
            const CBox& box = car->GetColModel()->m_boundBox;
            float dx = Dot(carDir, m.GetRight()), dy = Dot(carDir, m.GetForward());
            float tail = std::fmin(box.m_vecMin.x * dx, box.m_vecMax.x * dx) + std::fmin(box.m_vecMin.y * dy, box.m_vecMax.y * dy);
            float behind = Dot(feet - centre, carDir) - tail; // from him back to its tail
            float pass = std::fmax(kRollPass, carSpeed * (kCarKeeps - kRollShare)); // the car under him
            float hood, rise = 2.f;
            if (CarTopAt(car, feet - carDir * 0.6f, hood)) {
                float z0 = std::fmax(feet.z, hood + 0.1f);
                for (float t = 0.05f; pass * t < behind + 0.1f; t += 0.05f) {
                    float top;
                    if (CarTopAt(car, feet - carDir * (pass * t), top)) {
                        rise = std::fmax(rise, (top + kRollClearance - z0 + 0.5f * kGravity * t * t) / t);
                    }
                }
                over = rise <= kRollMaxRise;
                lift = z0 - feet.z;
                passTime = (behind + 0.3f) / pass;
            }
            if (over) {
                knock = carDir * std::fmax(0.f, carSpeed * kCarKeeps - pass) - skaterVelocity;
                knock.z = rise;
                spin = Cross(carDir, up) * kRollSpin;
                // The bumper sends the board out to his side, clear of the car
                // (his swept legs kick it on ahead of him).
                CVector offset(feet.x - centre.x, feet.y - centre.y, 0.f);
                boardKnock = knock + Normalised(offset - carDir * Dot(offset, carDir), Cross(up, carDir)) * kBoardSide;
            }
        }
        if (!over) {
            CVector offset(feet.x - centre.x, feet.y - centre.y, 0.f);
            CVector side = Normalised(offset - along * Dot(offset, along), Cross(up, along));
            knock = (along * kKnockShare + side * kKnockSide) * speed;
            knock.z = kKnockUp;
            lift = 0.f;
        }
        float k[3] = {knock.x, knock.y, knock.z}, w[3] = {spin.x, spin.y, spin.z}, b[3] = {boardKnock.x, boardKnock.y, boardKnock.z};
        if (Check(g_api.knock(g_session, k, over ? w : nullptr, lift, over ? b : nullptr), "sk_knock")) {
            Log("Car hit (model %d at %.1f m/s, %.1f m/s against him): %s, knocked at %.1f %.1f %.1f m/s, lifted %.2f m",
                car->m_nModelIndex, carSpeed, relative.Magnitude(), over ? "over the car" : "thrown aside", k[0], k[1], k[2], lift);
        }
        car->m_vecMoveSpeed *= kCarKeeps;
        if (over) {
            g_skate.underCar = car;
            g_skate.underSpeed = car->m_vecMoveSpeed;
            g_skate.underUntil = CTimer::m_snTimeInMilliseconds + static_cast<unsigned>(1000.f * passTime) + 300;
        }
        g_skate.hitsFrom = now + kHitCooldownMs;
        g_skate.hits++;
        canHit = false; // the rest still get told to ignore CJ
    }
}

// Skate only reads XInput pads; say so once if it hasn't found one.
void CheckPad() {
    if (g_skate.padChecked || GetTickCount64() < g_skate.padCheckAt) return;
    g_skate.padChecked = true;
    int pad = g_api.controller(g_session);
    Log("Skate is reading controller %d", pad);
    if (pad < 0) Message("No Xbox controller found. Skate 3 needs one (DS4Windows works).");
}

// The skater drives everything; the game's own controls would make CJ act too.
void BlockPlayerPad() {
    CPad::GetPad(0)->NewState = CControllerState{};
}

void LogStats(CPlayerPed* ped) {
    ULONGLONG now = GetTickCount64();
    if (now < g_skate.nextStatsLog) return;
    g_skate.nextStatsLog = now + 2000;
    const SkPose& p = g_skate.pose;
    float speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
    CVector at = ped->GetPosition();
    Log("tick=%llu state=%s speed=%.2f m/s skater=%.2f %.2f %.2f cj=%.2f %.2f %.2f heading=%.2f standing=%d pad=%d",
        p.tick, g_api.state(g_session), speed, p.root[12], p.root[13], p.root[14], at.x, at.y, at.z,
        ped->GetHeading(), ped->bIsStanding ? 1 : 0, g_api.controller(g_session));
}

// Skate's wipeout checks (request numbers) in words, for the bail log.
const char* WipeoutCheck(unsigned n) {
    switch (n) {
    case 0: return "the body or arms hit something";
    case 1: return "the body was pushed out of its pose";
    case 2: return "the board hit something at speed";
    case 3: return "the board twisted out from under the feet";
    case 6: return "landed too hard";
    case 8: return "landed on a grind too hard";
    case 10: return "slid off the end of a slide";
    case 17: return "fell off a grind";
    default: return nullptr;
    }
}

// One log line per bail: where, from what, and which of Skate's checks
// threw the skater (the first one is the cause; the rest follow from it).
void LogBail(const SkFeedback& f) {
    static uint32_t lastBails = 0, onBoard = 0; // the last state riding, in the air or grinding
    if (f.state >= 100 && f.state < 500 && f.state != 300) onBoard = f.state;
    if (f.bails < lastBails) lastBails = f.bails; // a new engine session
    if (f.bails == lastBails) return;
    lastBails = f.bails;
    // A run-out (stepping off at speed) comes between the cause and the
    // fall, so this is what he was doing on the board before it.
    const char* doing = onBoard >= 400 ? "grinding" : onBoard >= 200 ? "in the air" : "riding";
    std::string why;
    if (f.bail_first_check == 0xFFFFFFFFu) why = "no check (a car hit, or Skate's own bail)";
    for (unsigned n = 0; n < 64; n++) {
        if (!(f.bail_checks[n / 32] >> (n % 32) & 1)) continue;
        const char* name = WipeoutCheck(n);
        std::string item = name ? std::string(name) + " (" + std::to_string(n) + ")" : "check " + std::to_string(n);
        if (n == f.bail_first_check) why = item + (why.empty() ? "" : ", then " + why);
        else why += (why.empty() ? "" : ", ") + item;
    }
    const float* r = g_skate.pose.root;
    Log("Bail at %.2f %.2f %.2f after %s, %.1f m/s: %s", r[12], r[13], r[14], doing, f.bail_speed, why.c_str());
}

// ------------------------------------------------------------ automated test

// gta-sa.exe -sktest [script]: the plugin loads a save from the main menu,
// follows the script (see test_script.h), photographs CJ into
// SanAnskateas-test\ and quits the game, so the skeleton fitting can be
// checked without anyone at the controls. Normal launches never get here.
struct AutoTest {
    bool enabled = false;
    std::vector<TestCommand> commands;
    size_t next = 0;                      // the command to start next
    const TestCommand* running = nullptr; // a Place, Wait, Pad or Shoot under way
    int left = 0;                         // its frames or engine steps still to go
    int loadSlot = 0;                     // from the script's load command
    int gameFrames = 0;                   // frames of gameplay so far
    int shots = 0;
    std::wstring outDir;
    // The shot under way: which angle, how long the camera has held it, and
    // the photo drawingEvent takes at the end of the frame's 3D.
    size_t angle = 0;
    int angleFrames = 0;
    bool grabDue = false;
    bool screenDue = false; // a screen command: the whole frame, HUD included
    bool grabbed = false;
    std::string grabError;
    ContactSheet sheet;
    std::atomic<bool> done{false};
    std::atomic<ULONGLONG> quitAt{0};
    float timeScale = 1.f; // the game's, put back after a photo
    // The last test car holds its speed (driverless cars brake) until it hits
    // the skater or cruiseLeft seconds run out.
    CVehicle* cruise = nullptr;
    CVector cruiseSpeed; // as m_vecMoveSpeed
    int cruiseHits = 0;  // g_skate.hits when it set off
    float cruiseLeft = 0.f;
    float stepTime = 0.f; // game time the skater hasn't stepped yet (s)
    int wanted = 0;       // a wanted level the script asked for (police are off otherwise)
    int menuTour = 0;     // frames into a menutour (0: none under way)
    int menuSaved[3] = {-1, -1, -1}; // the player's settings, put back when the test ends
} g_test;

constexpr ULONGLONG kTestTimeoutMs = 6 * 60 * 1000; // the whole run, from launch
constexpr int kSettleFrames = 90;                    // after the save loads
constexpr int kShotHoldFrames = 3;                   // frames an angle holds before its photo
constexpr int kCellWidth = 400, kCellHeight = 533;  // one view on a contact sheet

void FinishTest(const char* why) {
    if (g_test.done.exchange(true)) return;
    Log("Test finished: %s (%d photos)", why, g_test.shots);
    for (int i = 0; i < kMenuOptions; i++) { // a menutour changed the player's settings: put them back
        if (g_test.menuSaved[i] >= 0 && g_menuOptions[i].value != g_test.menuSaved[i]) {
            g_menuOptions[i].value = g_test.menuSaved[i];
            OnMenuOption(i, g_test.menuSaved[i]);
        }
    }
    g_testCamera.on = false;
    g_test.quitAt = GetTickCount64();
    RsGlobal.quit = true;
}

// Where CJ's feet are, and which way the board (or CJ) points along the ground.
void TestSubject(CPlayerPed* ped, CVector& feet, CVector& forward) {
    if (g_skate.active) {
        const float* r = g_skate.pose.root;
        feet = CVector(r[12], r[13], r[14]);
        forward = CVector(-r[4], -r[5], 0.f); // the root's -Y (see PlaceCJ)
    } else {
        feet = ped->GetPosition() - CVector(0.f, 0.f, g_cfg.feetOffset);
        forward = ped->GetForward();
        forward.z = 0.f;
    }
    forward = Normalised(forward, CVector(0.f, 1.f, 0.f));
}

// A camera `degrees` round CJ (0 = ahead of him, 90 = his left), looking at him.
CamPose OrbitPose(CPlayerPed* ped, const TestCommand& c, float degrees) {
    CVector feet, forward;
    TestSubject(ped, feet, forward);
    CVector up(0.f, 0.f, 1.f);
    CVector left = Cross(up, forward);
    float a = degrees * kPi / 180.f;
    CVector target = feet + up * c.target;
    // aim=<bone ID>: at that bone, as last drawn.
    RpHAnimHierarchy* h = c.aim && g_rig.ready && ped->m_pRwClump ? GetAnimHierarchyFromSkinClump(ped->m_pRwClump) : nullptr;
    int bone = h ? RpHAnimIDGetIndex(h, c.aim) : -1;
    if (bone >= 0 && bone < g_rig.bones) {
        const float* m = &g_rig.matrices[static_cast<size_t>(bone) * 16];
        target = CVector(m[12], m[13], m[14]);
    }
    CVector pos = target + (forward * std::cos(a) + left * std::sin(a)) * c.distance + up * c.height;
    return {pos, Normalised(target - pos, forward), up, GameFov(c.fov)};
}

// One frame of a shot; true once every angle is photographed and the sheet saved.
bool ShootFrame(CPlayerPed* ped) {
    const TestCommand& c = *g_test.running;
    if (g_test.grabbed || !g_test.grabError.empty()) {
        if (!g_test.grabError.empty()) {
            Log("Test: photo %s at %.0f degrees failed: %s", c.name.c_str(), c.angles[g_test.angle], g_test.grabError.c_str());
        }
        g_test.grabbed = false;
        g_test.grabError.clear();
        g_test.angle++;
        g_test.angleFrames = 0;
    }
    if (g_test.angle >= c.angles.size()) {
        g_testCamera.on = false;
        CTimer::ms_fTimeScale = g_test.timeScale; // the game's world runs on again
        wchar_t number[16];
        swprintf(number, 16, L"\\%02d-", ++g_test.shots);
        std::wstring path = g_test.outDir + number + std::wstring(c.name.begin(), c.name.end()) + L".png";
        std::string error;
        if (g_test.sheet.Save(path, error)) Log("Test: saved %s", Utf8(path).c_str());
        else Log("Test: could not save %s: %s", Utf8(path).c_str(), error.c_str());
        float d[18] = {};
        if (g_api.rig_debug(g_session, d, 18) >= 18) {
            for (int arm = 0; arm < 2; arm++) {
                const float* a = d + arm * 9;
                Log("Test:   arm %d: solved %.0f, roll %.0f -> %.0f, clavicle lift %.0f, arm %.0f / bind %.0f from down, "
                    "grip %.2f, board under feet %.2f, grab hand short by %.1f cm",
                    arm, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
            }
        }
        return true;
    }
    g_testCamera.pose = OrbitPose(ped, c, c.angles[g_test.angle]);
    g_testCamera.on = true;
    g_test.grabDue = ++g_test.angleFrames >= kShotHoldFrames;
    return false;
}

// drawingEvent: the frame's 3D is drawn and the HUD isn't yet. Takes the
// photo a shot asked for this frame.
void TestGrab() {
    if (!g_test.grabDue || !g_test.running) return;
    g_test.grabDue = false;
    const TestCommand& c = *g_test.running;
    char caption[160];
    snprintf(caption, sizeof caption, "%02d %s  %.0f deg  %s  e%.2f s%.2f", g_test.shots + 1, c.name.c_str(),
             c.angles[g_test.angle], g_skate.active ? g_api.state(g_session) : "on foot", g_cfg.elbowFollow,
             g_cfg.shoulderFollow);
    std::string text = caption;
    auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
    if (!device) {
        g_test.grabError = "no D3D device";
        return;
    }
    if (g_test.sheet.Grab(device, static_cast<int>(g_test.angle), std::wstring(text.begin(), text.end()), g_test.grabError)) {
        g_test.grabbed = true;
    }
}

// drawHudEvent, after our HUD: the screen command's photo of the whole frame.
void TestScreen() {
    if (!g_test.screenDue || !g_test.running) return;
    g_test.screenDue = false;
    auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
    if (!device) {
        g_test.grabError = "no D3D device";
        return;
    }
    std::string caption = std::to_string(g_test.shots + 1) + " " + g_test.running->name + "  " +
                          (g_skate.active ? g_api.state(g_session) : "on foot");
    if (g_test.sheet.Grab(device, 0, std::wstring(caption.begin(), caption.end()), g_test.grabError)) g_test.grabbed = true;
}

// A photo of the frame being drawn (the pause menu included), taken at once.
void MenuPhoto(const char* name) {
    auto* device = static_cast<IDirect3DDevice9*>(GetD3DDevice());
    int width = 1280, height = static_cast<int>(width * SCREEN_HEIGHT / std::fmax(1.f, SCREEN_WIDTH));
    std::string caption = std::to_string(g_test.shots + 1) + " " + name, error;
    g_test.sheet.Begin(1, 1, width, height);
    wchar_t number[16];
    swprintf(number, 16, L"\\%02d-", ++g_test.shots);
    std::wstring path = g_test.outDir + number + std::wstring(name, name + strlen(name)) + L".png";
    if (device && g_test.sheet.Grab(device, 0, std::wstring(caption.begin(), caption.end()), error) && g_test.sheet.Save(path, error)) {
        Log("Test: saved %s", Utf8(path).c_str());
    } else {
        Log("Test: menu photo %s failed: %s", name, device ? error.c_str() : "no D3D device");
    }
}

// The menutour command, a step each menu frame: Options, then our page,
// then each option changed through the menu's own input handling (as the
// pad's left/right would), photographed along the way, then the menu closes.
void MenuTourFrame() {
    if (!g_test.menuTour) return;
    CMenuManager& m = FrontEndMenuManager;
    int t = g_test.menuTour++;
    char exit = 0;
    // Presses only ever land on our page (or our entry on Options): on any
    // other page the tour stops and the menu closes.
    auto onOurs = [&] {
        settings_menu::Page* pages = settings_menu::Table();
        return pages && m.m_nCurrentMenuPage == settings_menu::g.page && settings_menu::IsOurs(pages[settings_menu::g.page]);
    };
    auto press = [&](int entry, int leftRight, bool enter) {
        if (!onOurs()) {
            Log("Test: menutour stopped: not on our page (page %d)", m.m_nCurrentMenuPage);
            m.m_bShutDownFrontEndRequested = true;
            g_test.menuTour = 0;
            return;
        }
        m.m_nCurrentMenuEntry = entry;
        m.ProcessMenuOptions(static_cast<char>(leftRight), &exit, enter ? 1 : 0);
    };
    if (t == 5) m.SwitchToNewScreen(settings_menu::kBorrowedPage); // as GTA has it, never borrowed yet
    if (t == 15) MenuPhoto("menu-borrowed-page-untouched");
    if (t == 20) m.SwitchToNewScreen(settings_menu::kOptionsPage);
    if (t == 40) MenuPhoto("menu-options");
    if (t == 45) { // Enter on our Options entry (only on the Options page)
        settings_menu::Page* pages = settings_menu::Table();
        for (int i = 0; pages && m.m_nCurrentMenuPage == settings_menu::kOptionsPage && i < 12; i++) {
            if (settings_menu::Named(pages[settings_menu::kOptionsPage].items[i], settings_menu::kEntryKey)) {
                m.m_nCurrentMenuEntry = i;
                m.ProcessMenuOptions(0, &exit, 1);
                break;
            }
        }
        Log("Test: menu page %d (ours %d, filled %d)", m.m_nCurrentMenuPage, settings_menu::g.page, onOurs() ? 1 : 0);
    }
    if (t == 65) MenuPhoto("menu-page");
    if (t == 70) press(0, 1, false);  // difficulty: Easy -> Normal
    if (t == 72) press(1, 1, false);  // camera: High -> Low
    if (t == 74) press(2, 1, false);  // trucks: one tighter
    if (t == 76) press(2, 1, false);  // and another
    if (t == 78 && g_test.menuTour) m.m_nCurrentMenuEntry = 2;
    if (!g_test.menuTour) return;
    if (t == 95) MenuPhoto("menu-changed");
    // Back on Options, the borrowed page must be GTA's question again (only
    // looked at), and Mod Loader's page must still open.
    if (t == 100) m.SwitchToNewScreen(-2); // back
    if (t == 115) m.SwitchToNewScreen(settings_menu::kBorrowedPage);
    if (t == 130) {
        MenuPhoto("menu-borrowed-page-restored");
        if (settings_menu::Page* pages = settings_menu::Table()) {
            const settings_menu::Page& p = pages[settings_menu::kBorrowedPage];
            for (int i = 0; i < 4; i++) {
                Log("Test: borrowed page item %d: action %d '%.8s' type %d target %d at %u,%u (original action %d '%.8s' at %u,%u)", i,
                    p.items[i].action, p.items[i].name, p.items[i].type, p.items[i].target, p.items[i].x, p.items[i].y,
                    settings_menu::g.original.items[i].action, settings_menu::g.original.items[i].name, settings_menu::g.original.items[i].x,
                    settings_menu::g.original.items[i].y);
            }
        }
    }
    if (t == 135) m.SwitchToNewScreen(settings_menu::kOptionsPage);
    if (t == 140) {
        settings_menu::Page* pages = settings_menu::Table();
        for (int i = 0; pages && m.m_nCurrentMenuPage == settings_menu::kOptionsPage && i < 12; i++) {
            if (settings_menu::Named(pages[settings_menu::kOptionsPage].items[i], "ML_FEO")) { // Mod Loader's entry: opens its page
                m.m_nCurrentMenuEntry = i;
                m.ProcessMenuOptions(0, &exit, 1);
                break;
            }
        }
    }
    if (t == 160) MenuPhoto("menu-mod-loader");
    if (t == 165) {
        m.m_bShutDownFrontEndRequested = true;
        g_test.menuTour = 0;
        Log("Test: menu closed: difficulty %s, camera %s, trucks %d", g_cfg.difficulty.c_str(), g_cfg.lowCamera ? "low" : "high", g_cfg.trucks);
    }
}

void StartCommand(const TestCommand& c, CPlayerPed* ped) {
    using Kind = TestCommand::Kind;
    switch (c.kind) {
    case Kind::Radio: {
        int id = c.name == "sk8fm" ? kStationSk8 : c.name == "off" ? kStationOff : std::atoi(c.name.c_str());
        if (!g_radio.built) BuildStations();
        auto it = std::find(g_radio.stations.begin(), g_radio.stations.end(), id);
        if (it == g_radio.stations.end()) {
            Log("Test: radio %s isn't one of the stations", c.name.c_str());
            return;
        }
        g_radio.index = static_cast<int>(it - g_radio.stations.begin());
        g_radio.tuneAt = 0;
        g_radio.nameUntil = GetTickCount64() + 2500;
        if (g_skate.active) Tune(id);
        Log("Test: radio %s", c.name.c_str());
        return;
    }
    case Kind::Find: { // where a map model stands (its area must be loaded: place near it first)
        int model = -1;
        if (!CModelInfo::GetModelInfo(c.name.c_str(), &model) || model < 0) {
            Log("Test: find: no model called %s", c.name.c_str());
            return;
        }
        int found = 0;
        auto report = [&](CEntity* e, const char* kind) {
            if (!e || e->m_nModelIndex != model) return;
            CVector at = e->GetPosition();
            Log("Test: find: %s %s at %.1f %.1f %.1f, heading %.0f degrees", kind, c.name.c_str(), at.x, at.y, at.z,
                e->GetHeading() * 180.f / kPi);
            found++;
        };
        for (int i = 0; CPools::ms_pBuildingPool && i < CPools::ms_pBuildingPool->m_nSize; i++) report(CPools::ms_pBuildingPool->GetAt(i), "building");
        for (int i = 0; CPools::ms_pDummyPool && i < CPools::ms_pDummyPool->m_nSize; i++) report(CPools::ms_pDummyPool->GetAt(i), "dummy");
        if (!found) Log("Test: find: %s isn't placed in the loaded areas", c.name.c_str());
        return;
    }
    case Kind::MenuTour: {
        for (int i = 0; i < kMenuOptions; i++) {
            if (g_test.menuSaved[i] < 0) g_test.menuSaved[i] = g_menuOptions[i].value;
        }
        // Start from the defaults: Easy, High, the middle bar.
        const int defaults[3] = {0, 0, 4};
        for (int i = 0; i < kMenuOptions; i++) {
            if (g_menuOptions[i].value != defaults[i]) OnMenuOption(i, g_menuOptions[i].value = defaults[i]);
        }
        g_test.running = &c;
        g_test.menuTour = 1;
        FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
        return;
    }
    case Kind::Wanted: {
        g_test.wanted = static_cast<int>(c.args[0]);
        CWanted::SetMaximumWantedLevel(g_test.wanted > 0 ? 6 : 0);
        if (CWanted* wanted = FindPlayerWanted()) wanted->CheatWantedLevel(g_test.wanted);
        Log("Test: wanted level %d", g_test.wanted);
        return;
    }
    case Kind::DumpWorld: { // the collision a skater here would get, for offline checks (skate-ffi smooth tests)
        CVector at = FindPlayerPed()->GetPosition();
        CStreaming::LoadSceneCollision(&at);
        std::vector<float> tris;
        int entities = GatherWorld(at, at.z, g_cfg.worldRadius, tris);
        std::wstring path = g_test.outDir + L"\\" + std::wstring(c.name.begin(), c.name.end()) + L".tris";
        std::ofstream f(path, std::ios::binary);
        uint32_t count = static_cast<uint32_t>(tris.size() / 9);
        float centre[3] = {at.x, at.y, at.z};
        f.write("SKTRIS01", 8);
        f.write(reinterpret_cast<const char*>(centre), sizeof centre);
        f.write(reinterpret_cast<const char*>(&count), sizeof count);
        f.write(reinterpret_cast<const char*>(tris.data()), static_cast<std::streamsize>(tris.size() * sizeof(float)));
        Log("Test: dumpworld %s: %u triangles from %d entities (%d not streamed in) around %.1f %.1f %.1f", Utf8(path).c_str(), count,
            entities, g_unloaded, at.x, at.y, at.z);
        return;
    }
    case Kind::Screen: {
        g_test.running = &c;
        g_test.screenDue = true;
        g_test.grabbed = false;
        g_test.grabError.clear();
        int width = 1280, height = static_cast<int>(width * SCREEN_HEIGHT / std::fmax(1.f, SCREEN_WIDTH));
        g_test.sheet.Begin(1, 1, width, height);
        return;
    }
    case Kind::Load: // done from the main menu
        return;
    case Kind::Place: {
        if (g_skate.active) Stop("test: placing CJ", true);
        // Out of any interior first, as walking out of the door would (a
        // save starts CJ inside his house), or the outside never streams in.
        if (CGame::currArea != 0 || ped->m_nAreaCode != 0) {
            CGame::currArea = 0;
            ped->m_nAreaCode = 0;
            ped->m_pEnex = nullptr;
            CEntryExitManager::ms_entryExitStackPosn = 0;
            CStreaming::RemoveBuildingsNotInArea(0);
            CTimeCycle::StopExtraColour(false);
            Log("Test: CJ left the interior");
        }
        CVector at(c.args[0], c.args[1], c.args[2]);
        CStreaming::LoadScene(&at); // models and collision there, right away
        if (c.ground) {
            bool found = false;
            float ground = CWorld::FindGroundZFor3DCoord(at.x, at.y, 1000.f, &found, nullptr);
            at.z = found ? ground + g_cfg.feetOffset : 50.f;
            if (!found) Log("Test: no ground found at %.1f %.1f", at.x, at.y);
        }
        ped->Teleport(at, false);
        float heading = c.args[3] * kPi / 180.f;
        ped->SetHeading(heading);
        ped->m_fHeadingCurrent = heading;
        ped->m_fHeadingGoal = heading;
        Log("Test: CJ placed at %.2f %.2f %.2f, heading %.0f", at.x, at.y, at.z, c.args[3]);
        g_test.running = &c;
        g_test.left = 30; // frames to settle
        return;
    }
    case Kind::Clock:
        CClock::SetGameClock(static_cast<unsigned char>(c.args[0]), static_cast<unsigned char>(c.args[1]), 0);
        CClock::ms_nMillisecondsPerGameMinute = 1000000; // and hold it there
        return;
    case Kind::Weather:
        CWeather::ForceWeatherNow(static_cast<short>(c.args[0]));
        return;
    case Kind::Tuning:
        g_cfg.elbowFollow = c.args[0];
        g_cfg.shoulderFollow = c.args[1];
        if (g_rig.ready) Check(g_api.rig_tuning(g_session, g_cfg.elbowFollow, g_cfg.shoulderFollow), "sk_rig_tuning");
        Log("Test: tuning elbow %.2f shoulder %.2f", c.args[0], c.args[1]);
        return;
    case Kind::Env:
        SetEnvironmentVariableA(c.name.c_str(), std::to_string(c.args[0]).c_str());
        Log("Test: %s = %g", c.name.c_str(), c.args[0]);
        return;
    case Kind::Wait:
        if (c.count > 0) {
            g_test.running = &c;
            g_test.left = c.count;
        }
        return;
    case Kind::Skate:
        if (!g_skate.active) Start(ped);
        if (!g_skate.active) FinishTest("skating did not start");
        return;
    case Kind::Unskate:
        Stop("test: unskate", true);
        return;
    case Kind::Restart: // the next command waits until the engine is ready (TestFrame)
        RestartEngine();
        return;
    case Kind::Car: {
        int model = static_cast<int>(c.args[0]);
        CStreaming::RequestModel(model, 0);
        CStreaming::LoadAllRequestedModels(false);
        CVector feet, forward;
        TestSubject(ped, feet, forward);
        CVector at = feet + forward * c.args[1] + Cross(CVector(0.f, 0.f, 1.f), forward) * c.args[2];
        bool found = false;
        float ground = CWorld::FindGroundZFor3DCoord(at.x, at.y, feet.z + 3.f, &found, nullptr);
        at.z = (found ? ground : feet.z) + 1.f;
        CVector toward = Normalised(CVector(feet.x - at.x, feet.y - at.y, 0.f), forward * -1.f);
        auto* car = new CAutomobile(model, MISSION_VEHICLE, true);
        car->SetPosn(at);
        car->SetHeading(std::atan2(-toward.x, toward.y)); // SA heading 0 faces +Y
        car->m_nStatus = STATUS_ABANDONED;
        car->m_vecMoveSpeed = toward * (c.args[3] / 50.f);
        CWorld::Add(car);
        Log("Test: car %d at %.1f %.1f %.1f rolling at %.0f m/s", model, at.x, at.y, at.z, c.args[3]);
        if (c.args[3] > 0.f) {
            g_test.cruise = car;
            g_test.cruiseSpeed = car->m_vecMoveSpeed;
            g_test.cruiseHits = g_skate.hits;
            g_test.cruiseLeft = 2.f * c.args[1] / c.args[3] + 1.f;
        }
        return;
    }
    case Kind::Respawn: {
        if (!g_skate.active) {
            FinishTest("respawn while not skating");
            return;
        }
        float at[3] = {c.args[0], c.args[1], c.args[2]};
        Check(g_api.activate(g_session, at, c.args[3] * kPi / 180.f + kPi * 0.5f), "sk_activate");
        Check(g_api.get_pose(g_session, &g_skate.pose), "sk_get_pose");
        PlaceCJ(ped);
        Log("Test: skater respawned at %.1f %.1f %.1f", at[0], at[1], at[2]);
        return;
    }
    case Kind::Pad:
        if (!g_skate.active) {
            FinishTest("pad input while not skating");
            return;
        }
        if (c.count > 0) {
            g_test.running = &c;
            g_test.left = c.count;
        }
        return;
    case Kind::Shoot: {
        g_test.running = &c;
        g_test.angle = 0;
        g_test.angleFrames = 0;
        g_test.grabbed = false;
        g_test.grabDue = false;
        g_test.grabError.clear();
        int n = static_cast<int>(c.angles.size());
        g_test.sheet.Begin(std::min(n, 3), (n + 2) / 3, kCellWidth, kCellHeight);
        // The game's world holds still too (cars, peds), so a photo is one moment.
        g_test.timeScale = CTimer::ms_fTimeScale;
        CTimer::ms_fTimeScale = 0.f;
        return;
    }
    case Kind::Quit:
        FinishTest("the script quit");
        return;
    }
}

// Each gameplay frame in test mode, before the skater steps.
void TestFrame(CPlayerPed* ped) {
    if (!ped || g_test.done) return;
    Load load = g_load.load(std::memory_order_acquire);
    if (load == Load::Failed) FinishTest("the Skate engine failed to load");
    if (load != Load::Ready) return;
    // Nothing else on the roads, and no police.
    CPopulation::PedDensityMultiplier = 0.f;
    CCarCtrl::CarDensityMultiplier = 0.f;
    CWanted::SetMaximumWantedLevel(g_test.wanted > 0 ? 6 : 0);
    if (CVehicle* car = g_test.cruise) {
        g_test.cruiseLeft -= CTimer::ms_fTimeStep / 50.f;
        bool going = CPools::ms_pVehiclePool->IsObjectValid(car) && g_skate.hits == g_test.cruiseHits && g_test.cruiseLeft > 0.f;
        if (going) {
            car->m_vecMoveSpeed.x = g_test.cruiseSpeed.x; // and it falls and bounces as it likes
            car->m_vecMoveSpeed.y = g_test.cruiseSpeed.y;
        } else {
            g_test.cruise = nullptr;
        }
    }
    if (++g_test.gameFrames < kSettleFrames) return;
    using Kind = TestCommand::Kind;
    if (const TestCommand* c = g_test.running) {
        bool finished = true;
        switch (c->kind) {
        case Kind::Place:
        case Kind::Wait:
            finished = --g_test.left <= 0;
            break;
        case Kind::Pad: // TestStep counts it down
            if (!g_skate.active) Log("Test: skating stopped during line %d", c->line);
            finished = !g_skate.active || g_test.left <= 0;
            break;
        case Kind::Shoot:
            finished = ShootFrame(ped);
            break;
        case Kind::MenuTour: // MenuTourFrame runs it while the menu is up
            finished = g_test.menuTour == 0 && !FrontEndMenuManager.m_bMenuActive;
            break;
        case Kind::Screen:
            finished = g_test.grabbed || !g_test.grabError.empty();
            if (finished) {
                wchar_t number[16];
                swprintf(number, 16, L"\\%02d-", ++g_test.shots);
                std::wstring path = g_test.outDir + number + std::wstring(c->name.begin(), c->name.end()) + L".png";
                std::string error = g_test.grabError;
                if (error.empty() && g_test.sheet.Save(path, error)) Log("Test: saved %s", Utf8(path).c_str());
                else Log("Test: screen %s failed: %s", c->name.c_str(), error.c_str());
                if (g_skate.active) { // where the skater is and faces, for comparing photos
                    const float* r = g_skate.pose.root;
                    Log("Test: skater at %.1f %.1f %.1f facing %.0f degrees (state %u)", r[12], r[13], r[14],
                        (std::atan2(-r[5], -r[4]) - kPi * 0.5f) * 180.f / kPi, g_sfx.last.state);
                }
            }
            break;
        default:
            break;
        }
        if (!finished) return;
        g_test.running = nullptr;
    }
    while (!g_test.running && !g_test.done) {
        if (g_test.next >= g_test.commands.size()) {
            FinishTest("the script ended");
            return;
        }
        StartCommand(g_test.commands[g_test.next++], ped);
    }
    // A shot aims its first angle on the frame it starts.
    if (g_test.running && g_test.running->kind == Kind::Shoot && g_test.angleFrames == 0) ShootFrame(ped);
}

// The skater's steps in test mode: in game time (as sk_update would) while a
// pad command runs, none otherwise, so he holds still for photos.
int TestStep() {
    const TestCommand* c = g_test.running;
    if (!c || c->kind != TestCommand::Kind::Pad || g_test.left <= 0) {
        g_test.stepTime = 0.f;
        return 0;
    }
    SkControls in{c->buttons, {c->triggers[0], c->triggers[1]}, {c->left[0], c->left[1]}, {c->right[0], c->right[1]}};
    float period = g_api.period(g_session);
    if (period <= 0.f) return -1;
    g_test.stepTime += CTimer::ms_fTimeStep / 50.f;
    int total = 0;
    while (g_test.left > 0 && g_test.stepTime >= period) {
        int steps = g_api.step(g_session, &in);
        if (steps < 0) return steps;
        total += steps;
        g_test.left--;
        g_test.stepTime -= period;
    }
    return total;
}

// What the menu does when a save is picked (gta-reversed's
// CMenuManager::SimulateGameLoad); the frontend loads it on its next frame.
// The screen is written last, so the menu never acts on half of it.
void RequestSaveLoad(int slot) {
    FrontEndMenuManager.m_bShutDownFrontEndRequested = false; // m_bDontDrawFrontEnd
    FrontEndMenuManager.m_nSelectedSaveGame = static_cast<char>(slot - 1);
    CGame::bMissionPackGame = 0;
    FrontEndMenuManager.field_1B3C = 1; // m_CurrentlyLoading
    std::atomic_thread_fence(std::memory_order_seq_cst);
    FrontEndMenuManager.m_nCurrentMenuPage = 13; // SCREEN_LOAD_FIRST_SAVE
    Log("Test: loading save slot %d", slot);
}

// Test mode, off the game thread: skips the intro movies, loads the script's
// save once the main menu is up, and makes sure the game ends.
void TestWatchdog() {
    ULONGLONG started = GetTickCount64(), menuSince = 0;
    bool loadRequested = false;
    int lastState = -1;
    for (;;) {
        Sleep(50);
        ULONGLONG now = GetTickCount64();
        int state = gGameState;
        if (state != lastState) {
            DWORD pid = 0;
            if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
            Log("Test: game state %d (menu active %d, textures %d, in front %d)", state,
                FrontEndMenuManager.m_bMenuActive, FrontEndMenuManager.m_bTexturesLoaded, pid == GetCurrentProcessId());
            lastState = state;
        }
        if (state == 2) gGameState = 3;      // playing Logo.mpg: on to the title movie
        else if (state == 4) gGameState = 5; // playing GTAtitles.mpg: on to the menu
        if (state == 7 && !loadRequested && FrontEndMenuManager.m_bMenuActive && FrontEndMenuManager.m_bTexturesLoaded) {
            if (!menuSince) {
                menuSince = now;
            } else if (now - menuSince > 1000) {
                loadRequested = true;
                if (g_test.loadSlot > 0) RequestSaveLoad(g_test.loadSlot);
                else Log("Test: the script loads no save; waiting for one to be loaded");
            }
        }
        if (now - started > kTestTimeoutMs) FinishTest("timed out");
        ULONGLONG quitAt = g_test.quitAt;
        if (quitAt && now - quitAt > 20000) {
            Log("Test: the game did not quit; ending it");
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

// -sktest [script]: reads the script, clears old photos and starts the
// watchdog. Without the switch, nothing here runs.
void StartTestMode() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring script;
    bool wanted = false;
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"-sktest") != 0) continue;
        wanted = true;
        script = i + 1 < argc && argv[i + 1][0] != L'-' ? argv[i + 1] : g_dir + L"\\SanAnskateas-test.txt";
    }
    if (argv) LocalFree(argv);
    if (!wanted) return;
    std::ifstream file(script, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string error = file ? "" : "cannot read it";
    if (error.empty()) ParseTestScript(text.str(), g_test.commands, error);
    if (!error.empty()) {
        Log("Test mode: cannot use %s: %s", Utf8(script).c_str(), error.c_str());
        TerminateProcess(GetCurrentProcess(), 1);
    }
    for (const TestCommand& c : g_test.commands) {
        if (c.kind == TestCommand::Kind::Load && !g_test.loadSlot) g_test.loadSlot = c.count;
    }
    g_test.outDir = g_dir + L"\\SanAnskateas-test";
    CreateDirectoryW(g_test.outDir.c_str(), nullptr);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((g_test.outDir + L"\\*.png").c_str(), &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((g_test.outDir + L"\\" + found.cFileName).c_str());
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
    g_test.enabled = true;
    g_logSounds = true;
    Log("Test mode: %zu commands from %s", g_test.commands.size(), Utf8(script).c_str());
    std::thread(TestWatchdog).detach();
}

void OnFrame() {
    CPlayerPed* ped = FindPlayerPed(-1);
    UpdateAudioMix();
    bool key = ToggleKeyPressed();
    if (key && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) { // Ctrl + J: restart the engine instead
        RestartEngine();
        key = false;
    }
    bool combo = false;
    if (g_cfg.padToggle) combo = g_sticks.Update(ped, g_skate.active, g_cfg.comboWindowMs);
    bool toggle = key || combo;
    if (g_test.enabled) TestFrame(ped);
    Load load = g_load.load(std::memory_order_acquire);

    if (load == Load::Ready && !g_readyAnnounced && ped) {
        g_readyAnnounced = true;
        Message("Skate 3 is ready. Press J or L3 + R3 to skate.");
    }

    if (g_skate.active) {
        if (ped != g_skate.ped) {
            Stop("the player changed", false); // the old ped may be gone
        } else if (Blocked b = Blocker(ped); b.why) {
            Stop(b.why, true, b.momentum ? Exit::Momentum : Exit::Freeze);
        } else if ((ped->GetPosition() - g_skate.placed).Magnitude() > 2.f) {
            Stop("the game moved the player", true);
        } else {
            BlockPlayerPad();
            float dt = CTimer::ms_fTimeStep / 50.f; // time step is in 50 fps frames
            int steps = g_test.enabled ? TestStep() : g_api.update(g_session, dt);
            bool failed = !Check(steps, "sk_update") || (steps > 0 && !Check(g_api.get_pose(g_session, &g_skate.pose), "sk_get_pose"));
            if (failed && RecoverEngine(ped)) failed = false;
            if (failed) {
                Stop("the Skate engine reported an error", true);
                Message("Skate stopped after an engine error (see SanAnskateas.log).");
            } else {
                PlaceCJ(ped);
                StreamWorld();
                CheckCarHits();
                LogStats(ped);
                CheckPad();
                SkFeedback feedback{};
                if (g_api.feedback(g_session, &feedback) == 0) {
                    UpdateSounds(dt, feedback);
                    UpdateHud(dt, feedback);
                    LogBail(feedback);
                }
                UpdateRadio();
                // Skate has no water: below the surface, CJ swims instead.
                const float* r = g_skate.pose.root;
                float water = 0.f;
                if (CWaterLevel::GetWaterLevel(r[12], r[13], r[14], &water, false, nullptr) && r[14] < water - 0.4f) {
                    Stop("rode into water", true, Exit::Momentum);
                }
            }
        }
        if (toggle && g_skate.active) {
            Stop(combo ? "toggled off (L3 + R3)" : "toggled off (key)", true, Exit::Hop);
            Message("Off the board.");
            return;
        }
    }

    if (!toggle || g_skate.active) return;
    switch (load) {
    case Load::NotStarted:
    case Load::Loading: Message("Skate 3 is still loading..."); return;
    case Load::Failed: Message("Skate 3 failed to load (see SanAnskateas.log)."); return;
    case Load::Ready: break;
    }
    if (Blocked b = Blocker(ped); b.why) {
        Log("Can't skate: %s", b.why);
        return;
    }
    Start(ped);
}

class SanAnskateas {
public:
    SanAnskateas() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Log), &self);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(self, path, MAX_PATH);
        g_dir = path;
        g_dir.resize(g_dir.find_last_of(L"\\/"));
        LoadConfig();
        Log("SanAnskateas loaded from %s", Utf8(path).c_str());
        StartTestMode();
        if (g_test.enabled) Events::drawingEvent += [] { TestGrab(); };

        Events::initRwEvent += [] {
            Load expected = Load::NotStarted;
            if (g_load.compare_exchange_strong(expected, Load::Loading)) {
                std::thread(LoadEngine).detach();
            }
            StartAudio();
        };
        Events::drawHudEvent += [] {
            DrawHud();
            if (g_test.enabled) TestScreen();
        };
        // Options > SAN ANSKATEAS in the pause menu.
        SyncMenuOptions();
        settings_menu::Install(g_menuOptions, kMenuOptions, OnMenuOption);
        Log("Settings %s", settings_menu::g.log);
        // Our sounds hold still in the pause menu and while the game is in the background.
        Events::menuDrawingEvent += [] {
            UpdateAudioMix();
            static std::string lastNote = settings_menu::g.log;
            settings_menu::EnsureTable(); // again if something rebuilt the menu
            if (lastNote != settings_menu::g.log) Log("Settings %s", lastNote.assign(settings_menu::g.log).c_str());
            settings_menu::Draw(FrontEndMenuManager);
            if (g_test.enabled) MenuTourFrame();
        };
        Events::onPauseAllSounds += [] {
            g_windowPaused = true;
            UpdateAudioMix();
        };
        Events::onResumeAllSounds += [] {
            g_windowPaused = false;
            UpdateAudioMix();
        };
        // A new game or a loaded save replaces the player ped.
        Events::reInitGameEvent += [] { Stop("a game was loaded", false); };
        Events::processScriptsEvent += [] { OnFrame(); };
        Events::gameProcessEvent += [] {
            if (g_skate.active && FindPlayerPed(-1) == g_skate.ped) PlaceCJ(g_skate.ped);
        };
        Events::pedRenderEvent += [](CPed* ped) {
            ApplyRig(ped);
            if (g_skate.active && ped == g_skate.ped) DrawBoard();
        };
        g_cameraHooked = HookCameraProcess();
        Log(g_cameraHooked ? "Camera hook installed" : "Camera hook NOT installed (unexpected code at 0x52B90A); Skate camera off");
    }
} g_plugin;

} // namespace
