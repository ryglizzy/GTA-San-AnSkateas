// SanAnskateas: Skate 3 skating in GTA San Andreas, driven by the Skate 3
// Rust engine through skate_ffi.dll.
//
// Frame order (CGame::Process): pads update -> scripts -> [processScriptsEvent:
// we block the pad, step Skate and place CJ] -> CWorld::Process (physics,
// with CJ frozen) -> camera -> [gameProcessEvent: we re-place CJ so nothing
// the frame did can drift him] -> render.
#include "plugin.h"
#include "common.h"
#include "CCamera.h"
#include "CCarCtrl.h"
#include "CClock.h"
#include "CCutsceneMgr.h"
#include "CDraw.h"
#include "CEntryExitManager.h"
#include "CGame.h"
#include "CHud.h"
#include "CMenuManager.h"
#include "CPad.h"
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

#include "capture.h"
#include "skate_api.h"
#include "stick_combo.h"
#include "test_script.h"

#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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
    ULONGLONG comboWindowMs = 400; // how long a lone L3/R3 waits for the other stick
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
    std::string difficulty = "normal"; // Skate physics: easy, normal or hardcore
    bool skateCamera = true;   // Skate 3's own camera while skating
    float cameraBlendIn = 0.5f, cameraBlendOut = 0.5f; // seconds
    float exitAirPitch = 15.f; // getting off mid-air tilts the game camera down this much
    bool carryMomentum = true; // CJ's velocity onto the board and back
    float hopSpeed = 2.5f;   // getting off on the ground faster than this (m/s) hops off
    float hopUp = 3.0f;      // upward speed of that hop (m/s)
    float boardBrightness = 0.45f; // the board's share of the game's lights, to match CJ
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
    float window = IniFloat(ini.c_str(), L"ComboWindow", 0.4f);
    g_cfg.comboWindowMs = static_cast<ULONGLONG>(std::fmin(std::fmax(window, 0.f), 2.f) * 1000.f);
    g_cfg.feetOffset = IniFloat(ini.c_str(), L"FeetOffset", 1.0f);
    GetPrivateProfileStringW(L"Skate", L"Difficulty", L"normal", buf, MAX_PATH, ini.c_str());
    g_cfg.difficulty = Utf8(buf);
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
}

// ---------------------------------------------------------- engine loading

enum class Load { NotStarted, Loading, Ready, Failed };
std::atomic<Load> g_load{Load::NotStarted};
SkateApi g_api;
SkSession* g_session = nullptr; // set by the loader before Ready; game thread only after
std::string g_loadError;        // set by the loader before Failed

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
} g_world;

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
    int generation = g_api.queue_world(g_session, g_world.tris.data(), static_cast<uint32_t>(g_world.tris.size() / 9));
    if (!Check(generation, "sk_queue_world")) return;
    g_world.pending = generation;
    g_world.centre = centre;
    g_world.cars = g_gatheredCars;
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
            return true;
        }
    }
    Log("No San Andreas collision loaded here (%d entities); using a flat floor", entities);
    g_world.cars.clear();
    return InstallFloor(centre);
}

// Whether the cars in the skater's world are still as gathered: none moved
// off or sped up, and no car near him slowed into it (stopped at a light,
// say). Otherwise the world is rebuilt, so stopped cars are solid where they
// are and a car that drove off leaves nothing behind.
bool CarsChanged(const CVector& skater) {
    if (!g_cfg.parkedCarsCollide || !CPools::ms_pVehiclePool) return false;
    const float range = g_cfg.worldRadius * 0.7f; // ("near" is a Windows macro)
    std::vector<bool> seen(g_world.cars.size(), false);
    for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; i++) {
        CVehicle* car = CPools::ms_pVehiclePool->GetAt(i);
        if (!car) continue;
        CVector at = car->GetPosition();
        bool slow = car->m_vecMoveSpeed.Magnitude() * 50.f < kCarMovingSpeed;
        auto known = std::find_if(g_world.cars.begin(), g_world.cars.end(), [car](const auto& c) { return c.first == car; });
        if (known != g_world.cars.end()) {
            seen[known - g_world.cars.begin()] = true;
            if (!slow || (at - known->second).Magnitude() > 0.4f) return true;
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
    if (now >= g_world.carsCheckAt) {
        g_world.carsCheckAt = now + 250;
        carsChanged = CarsChanged(skater);
    }
    if (std::sqrt(dx * dx + dy * dy) < g_cfg.worldRebuild && now < g_world.refreshAt && !carsChanged) return;
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
} g_test;

constexpr ULONGLONG kTestTimeoutMs = 6 * 60 * 1000; // the whole run, from launch
constexpr int kSettleFrames = 90;                    // after the save loads
constexpr int kShotHoldFrames = 3;                   // frames an angle holds before its photo
constexpr int kCellWidth = 400, kCellHeight = 533;  // one view on a contact sheet

void FinishTest(const char* why) {
    if (g_test.done.exchange(true)) return;
    Log("Test finished: %s (%d photos)", why, g_test.shots);
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

void StartCommand(const TestCommand& c, CPlayerPed* ped) {
    using Kind = TestCommand::Kind;
    switch (c.kind) {
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
    CWanted::SetMaximumWantedLevel(0);
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
    Log("Test mode: %zu commands from %s", g_test.commands.size(), Utf8(script).c_str());
    std::thread(TestWatchdog).detach();
}

void OnFrame() {
    CPlayerPed* ped = FindPlayerPed(-1);
    bool key = ToggleKeyPressed();
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
            if (!Check(steps, "sk_update") || (steps > 0 && !Check(g_api.get_pose(g_session, &g_skate.pose), "sk_get_pose"))) {
                Stop("the Skate engine reported an error", true);
                Message("Skate stopped after an engine error (see SanAnskateas.log).");
            } else {
                PlaceCJ(ped);
                StreamWorld();
                CheckCarHits();
                LogStats(ped);
                CheckPad();
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
