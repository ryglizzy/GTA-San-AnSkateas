// Loads skate_ffi.dll at runtime and binds its functions, so a missing or
// broken DLL becomes a logged message instead of the game refusing to start.
#pragma once

#include <windows.h>
#include <string>

#define SK_API // declarations only: every call goes through the pointers below
#include "../../skate-ffi/include/skate_ffi.h"

struct SkateApi {
    decltype(&sk_session_new_mode) session_new_mode = nullptr;
    decltype(&sk_session_free) session_free = nullptr;
    decltype(&sk_install_collision) install_collision = nullptr;
    decltype(&sk_install_world) install_world = nullptr;
    decltype(&sk_queue_world) queue_world = nullptr;
    decltype(&sk_world_info) world_info = nullptr;
    decltype(&sk_rig_setup) rig_setup = nullptr;
    decltype(&sk_rig_pose) rig_pose = nullptr;
    decltype(&sk_rig_facing) rig_facing = nullptr;
    decltype(&sk_rig_tuning) rig_tuning = nullptr;
    decltype(&sk_rig_debug) rig_debug = nullptr;
    decltype(&sk_board_mesh) board_mesh = nullptr;
    decltype(&sk_board_pose) board_pose = nullptr;
    decltype(&sk_activate) activate = nullptr;
    decltype(&sk_set_velocity) set_velocity = nullptr;
    decltype(&sk_knock) knock = nullptr;
    decltype(&sk_update) update = nullptr;
    decltype(&sk_step) step = nullptr;
    decltype(&sk_suspend_input) suspend_input = nullptr;
    decltype(&sk_period) period = nullptr;
    decltype(&sk_set_aspect_ratio) set_aspect_ratio = nullptr;
    decltype(&sk_get_pose) get_pose = nullptr;
    decltype(&sk_get_bones) get_bones = nullptr;
    decltype(&sk_bone_name) bone_name = nullptr;
    decltype(&sk_state) state = nullptr;
    decltype(&sk_controller) controller = nullptr;
    decltype(&sk_last_error) last_error = nullptr;

    // Returns an empty string on success, otherwise what went wrong.
    std::string Load(const std::wstring& path) {
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            return "could not load skate_ffi.dll (Windows error " + std::to_string(GetLastError()) + ")";
        }
        std::string missing;
        auto bind = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
            if (!fn) missing += std::string(missing.empty() ? "" : ", ") + name;
        };
        bind(session_new_mode, "sk_session_new_mode");
        bind(session_free, "sk_session_free");
        bind(install_collision, "sk_install_collision");
        bind(install_world, "sk_install_world");
        bind(queue_world, "sk_queue_world");
        bind(world_info, "sk_world_info");
        bind(rig_setup, "sk_rig_setup");
        bind(rig_pose, "sk_rig_pose");
        bind(rig_facing, "sk_rig_facing");
        bind(rig_tuning, "sk_rig_tuning");
        bind(rig_debug, "sk_rig_debug");
        bind(board_mesh, "sk_board_mesh");
        bind(board_pose, "sk_board_pose");
        bind(activate, "sk_activate");
        bind(set_velocity, "sk_set_velocity");
        bind(knock, "sk_knock");
        bind(update, "sk_update");
        bind(step, "sk_step");
        bind(suspend_input, "sk_suspend_input");
        bind(period, "sk_period");
        bind(set_aspect_ratio, "sk_set_aspect_ratio");
        bind(get_pose, "sk_get_pose");
        bind(get_bones, "sk_get_bones");
        bind(bone_name, "sk_bone_name");
        bind(state, "sk_state");
        bind(controller, "sk_controller");
        bind(last_error, "sk_last_error");
        if (!missing.empty()) {
            return "skate_ffi.dll is missing functions (wrong version?): " + missing;
        }
        return {};
    }
};
