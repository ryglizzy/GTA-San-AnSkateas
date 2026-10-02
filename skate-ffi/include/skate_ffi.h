/* skate_ffi.h - C interface to the Skate 3 Rust engine (32-bit skate_ffi.dll).
 *
 * Link against skate_ffi.dll.lib and ship skate_ffi.dll beside the game.
 *
 * Threads: the engine runs on its own thread and every call waits for it.
 * Use a session from one thread at a time.
 *
 * Space: positions and directions are in the host game's world space, Z up,
 * in host units; meters_per_unit converts them to Skate's metres (1.0 for
 * GTA San Andreas, 0.0254 for MW2). Yaw is radians counter-clockwise from
 * the host's +X axis around +Z. For San Andreas, yaw = ped heading + pi/2.
 *
 * Errors: functions returning int give >= 0 on success and -1 on failure;
 * sk_last_error() then describes it (valid until the next failure on the
 * same thread).
 */
#ifndef SKATE_FFI_H
#define SKATE_FFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SK_API
#define SK_API __declspec(dllimport)
#endif
#define SK_CALL __cdecl

typedef struct SkSession SkSession;

/* Raw Xbox 360 pad state; same layout as XInput's XINPUT_GAMEPAD. */
typedef struct SkControls {
    uint16_t buttons;
    uint8_t triggers[2];
    int16_t left[2];
    int16_t right[2];
} SkControls;

typedef struct SkPose {
    uint64_t tick;            /* engine steps since the session started */
    float root[16];           /* skater root transform, column-major 4x4 */
    float velocity[3];        /* board velocity, host units per second */
    uint32_t bone_count;      /* matrices available from sk_get_bones */
    int32_t has_camera;       /* nonzero when the camera fields are set */
    float camera_position[3];
    float camera_forward[3];  /* unit vector the Skate camera looks along */
    float camera_up[3];
    float camera_fov;         /* degrees */
} SkPose;

typedef struct SkWorldInfo {
    uint32_t generation; /* compare with what sk_install_world/sk_queue_world returned */
    uint32_t triangles;
    uint32_t rails;      /* grind rails found in the triangles */
    uint32_t failures;   /* background builds that failed so far */
    float build_ms;
} SkWorldInfo;

/* A host skeleton bone for sk_rig_setup. */
typedef struct SkRigBone {
    const char* skate;       /* Skate bone it follows; NULL: rides on its parent */
    const char* skate_child; /* Skate bone at the end of its segment; NULL: none */
    int32_t parent;          /* index earlier in the array, or -1 */
    int32_t child;           /* host bone at the end of its segment, or -1 */
    float bind[16];          /* bind pose in the host model's space, column-major */
} SkRigBone;

/* Compile-time checks that this header matches the DLL. */
typedef char sk_controls_size_check[sizeof(SkControls) == 12 ? 1 : -1];
typedef char sk_pose_size_check[sizeof(SkPose) == 136 ? 1 : -1];
typedef char sk_world_info_size_check[sizeof(SkWorldInfo) == 20 ? 1 : -1];

/* Starts a session. assets_dir (UTF-8) is the skate-data folder converted
 * from your own Skate 3 default.xex. triangles: triangle_count * 9 floats.
 * rails: rail_count polylines; rail_lengths[i] points each, all packed in
 * rail_points (3 floats per point). Pass NULL/0 for no rails.
 * Returns NULL on failure. */
SK_API SkSession* SK_CALL sk_session_new(const char* assets_dir,
                                         const float* triangles, uint32_t triangle_count,
                                         const float* rail_points, const uint32_t* rail_lengths,
                                         uint32_t rail_count, float meters_per_unit);
/* sk_session_new uses Skate's "easy" physics; this picks the difficulty:
 * "easy", "normal" (Skate 3's default; also used for NULL) or "hardcore". */
SK_API SkSession* SK_CALL sk_session_new_mode(const char* assets_dir,
                                              const float* triangles, uint32_t triangle_count,
                                              const float* rail_points, const uint32_t* rail_lengths,
                                              uint32_t rail_count, float meters_per_unit,
                                              const char* difficulty);
SK_API void SK_CALL sk_session_free(SkSession* session);

/* Optional: decode animation banks early (e.g. on a loading thread). */
SK_API int SK_CALL sk_preload(const char* assets_dir);

/* Swap in new collision, e.g. as the world streams around the player. */
SK_API int SK_CALL sk_install_collision(SkSession* session,
                                        const float* triangles, uint32_t triangle_count,
                                        const float* rail_points, const uint32_t* rail_lengths,
                                        uint32_t rail_count);

/* The world the skater rides, from host triangles (9 floats each), with
 * grind rails found automatically. sk_install_world blocks until it is in;
 * sk_queue_world builds it in the background and the next sk_update/sk_step
 * after it finishes installs it (a newer request replaces an older one).
 * Both return the world's generation, or -1. */
SK_API int SK_CALL sk_install_world(SkSession* session, const float* triangles, uint32_t triangle_count);
SK_API int SK_CALL sk_queue_world(SkSession* session, const float* triangles, uint32_t triangle_count);
SK_API int SK_CALL sk_world_info(SkSession* session, SkWorldInfo* out);

/* Fit a host skeleton (parents before children) to the skater; facing_yaw
 * turns the host model's forward onto the skater's (pi for SA peds). Then
 * sk_rig_pose writes one world matrix per bone (16 floats, column-major)
 * for the latest pose and returns how many it wrote. */
SK_API int SK_CALL sk_rig_setup(SkSession* session, const SkRigBone* bones, uint32_t count, float facing_yaw);
SK_API int SK_CALL sk_rig_pose(SkSession* session, float* out, uint32_t count);
/* facing_yaw = NAN measures the turn from both skeletons' body frames
 * (hips to chest, heels to toes); this returns it as x, y, z, w. */
SK_API int SK_CALL sk_rig_facing(SkSession* session, float* out);
/* Arms, each 0..1: elbow_follow is how far (times 90 degrees) an upper arm
 * may roll round itself to point its elbow like the skater's (0: never);
 * clavicle_follow 0 keeps shoulders on the chest, 1 moves them like the
 * skater's. */
SK_API int SK_CALL sk_rig_tuning(SkSession* session, float elbow_follow, float clavicle_follow);
/* Diagnostics: 6 floats per arm from the last sk_rig_pose (solved, roll
 * before and after the limit, clavicle lift, arm and bind angle from down,
 * in degrees). Returns how many floats there are. */
SK_API int SK_CALL sk_rig_debug(SkSession* session, float* out, uint32_t count);

/* The skateboard (deck, trucks, wheels), from board.json in the converted
 * data. One surface per texture. */
typedef struct SkBoardSurface {
    uint32_t first_vertex;   /* its vertices in sk_board_pose's output */
    uint32_t vertex_count;
    const uint16_t* indices; /* triangle list, relative to first_vertex */
    uint32_t index_count;
    const float* uvs;        /* u, v per vertex of this surface */
    const uint8_t* rgba;     /* its texture, width * height * 4 bytes, top row first */
    uint32_t width, height;
} SkBoardSurface;
typedef char sk_board_surface_size_check[sizeof(void*) != 4 || sizeof(SkBoardSurface) == 32 ? 1 : -1];

/* Loads the board on the first call; writes up to max surfaces to out and
 * returns how many there are. Pointers stay valid until the session is freed. */
SK_API int SK_CALL sk_board_mesh(SkSession* session, SkBoardSurface* out, uint32_t max);
/* The board as the skater holds it: 6 floats per vertex (position, normal)
 * in host world space. Returns the vertex count. */
SK_API int SK_CALL sk_board_pose(SkSession* session, float* out, uint32_t max_vertices);

/* Drop the skater onto the board at position[3], facing yaw. */
SK_API int SK_CALL sk_activate(SkSession* session, const float* position, float yaw);

/* Give the board and skater a velocity (host units per second), e.g. to
 * carry the host character's momentum onto the board. Call right after
 * sk_activate. */
SK_API int SK_CALL sk_set_velocity(SkSession* session, const float* velocity);

/* A host vehicle hit the skater: Skate's own vehicle bail (and its respawn
 * after), with velocity (host units per second) added to the body and board
 * (or board_velocity to the board, when not NULL), spinning at spin (rad/s,
 * host space; NULL: a tumble about the push; given, he tumbles freely until
 * he first touches something), starting lift host units higher (onto a
 * car's hood). */
SK_API int SK_CALL sk_knock(SkSession* session, const float* velocity, const float* spin, float lift,
                            const float* board_velocity);

/* Advance by dt seconds of game time, reading the Xbox pad itself.
 * Returns the number of fixed engine steps run (0 is normal). */
SK_API int SK_CALL sk_update(SkSession* session, float dt);

/* Run exactly one fixed step with the given pad state instead. */
SK_API int SK_CALL sk_step(SkSession* session, const SkControls* controls);

SK_API int SK_CALL sk_suspend_input(SkSession* session);
SK_API float SK_CALL sk_period(SkSession* session); /* seconds; -1 on failure */
SK_API int SK_CALL sk_set_aspect_ratio(SkSession* session, float aspect_ratio);

/* Latest pose, refreshed by sk_activate, sk_update and sk_step. */
SK_API int SK_CALL sk_get_pose(SkSession* session, SkPose* out);
/* Up to max_bones matrices (16 floats each, column-major, Skate skater
 * space). Returns how many were copied. */
SK_API int SK_CALL sk_get_bones(SkSession* session, float* out, uint32_t max_bones);
SK_API const char* SK_CALL sk_bone_name(SkSession* session, uint32_t index);
SK_API const char* SK_CALL sk_state(SkSession* session);
SK_API int SK_CALL sk_controller(SkSession* session); /* pad 0-3, or -1 */

SK_API const char* SK_CALL sk_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* SKATE_FFI_H */
