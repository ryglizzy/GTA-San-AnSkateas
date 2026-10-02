//! C interface to the Skate 3 engine (`skate_host::bridge::Session`) so a
//! non-Rust host, such as a GTA San Andreas ASI plugin, can run it.
//!
//! The session runs on a worker thread with a 32 MB stack, as IW4L runs it;
//! each call hands work to that thread and waits for the answer. Positions
//! crossing this boundary are in the host's space: Z up, in host units, with
//! `meters_per_unit` converting to Skate's Y-up metres (1.0 for San Andreas,
//! 0.0254 for MW2). Functions returning `c_int` give a value >= 0 on success
//! and -1 on failure; `sk_last_error` then says why. See include/skate_ffi.h.

mod board_export;
mod rails;

use bevy::math::{Mat4, Vec3, Vec4};
use skate_host::bridge::{CollisionBuilder, ControllerTransport, Controls, Pose, PreparedCollision, Session};
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_int};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::{Mutex, Once, mpsc};
use std::thread::JoinHandle;

/// IW4L gives every thread that runs Skate code this much stack.
const WORKER_STACK: usize = 32 * 1024 * 1024;
/// Most fixed steps one `sk_update` runs, so a long hitch can't stall the host.
const MAX_STEPS: c_int = 16;

/// Raw Xbox 360 pad state, laid out like XInput's `XINPUT_GAMEPAD`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SkControls {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}

/// The latest skater pose, in host space.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SkPose {
    pub tick: u64,
    pub root: [f32; 16],
    pub velocity: [f32; 3],
    pub bone_count: u32,
    pub has_camera: c_int,
    pub camera_position: [f32; 3],
    pub camera_forward: [f32; 3],
    pub camera_up: [f32; 3],
    pub camera_fov: f32,
}

// The header declares the same sizes; a mismatch would corrupt every call.
const _: () = assert!(size_of::<SkControls>() == 12);
const _: () = assert!(size_of::<SkPose>() == 136);

/// What the installed world holds (see `sk_world_info`).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SkWorldInfo {
    pub generation: u32,
    pub triangles: u32,
    pub rails: u32,
    pub failures: u32,
    pub build_ms: f32,
}
const _: () = assert!(size_of::<SkWorldInfo>() == 20);

/// A world ready to install: its generation, collision and stats.
type Built = (u32, PreparedCollision, SkWorldInfo);

struct Worker {
    session: Session,
    transport: ControllerTransport,
    accumulated: f32,
    pending: Option<mpsc::Receiver<Result<Built, String>>>,
    world: SkWorldInfo,
}

impl Worker {
    fn install(&mut self, (generation, prepared, info): Built) -> Result<(), String> {
        if generation <= self.world.generation {
            return Ok(()); // a newer world is already in
        }
        self.session.install_collision(prepared)?;
        self.world = SkWorldInfo { failures: self.world.failures, ..info };
        Ok(())
    }

    /// Installs a background build if one has finished. A failed build keeps
    /// the current world and is only counted.
    fn install_built(&mut self) -> Result<(), String> {
        let Some(receiver) = &self.pending else { return Ok(()) };
        match receiver.try_recv() {
            Ok(Ok(built)) => {
                self.pending = None;
                self.install(built)
            }
            Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                self.pending = None;
                self.world.failures += 1;
                Ok(())
            }
            Err(mpsc::TryRecvError::Empty) => Ok(()),
        }
    }
}

// ------------------------------------------------------------------- rig

/// A host skeleton bone for `sk_rig_setup`.
#[repr(C)]
pub struct SkRigBone {
    pub skate: *const c_char,       // Skate bone it follows; null: rides on its parent
    pub skate_child: *const c_char, // Skate bone at the end of its segment; null: none
    pub parent: i32,                // index earlier in the array, or -1
    pub child: i32,                 // host bone at the end of its segment, or -1
    pub bind: [f32; 16],            // bind pose in the host model's space, column-major
}

struct RigBone {
    skate: Option<String>,
    skate_child: Option<String>,
    parent: Option<usize>,
    child: Option<usize>,
    bind: Mat4,
}

/// A host skeleton fitted to the Skate skater, like the mashup's MW2 soldier
/// (render_anim/src/skate/rig.rs): each mapped bone's bind segment is turned
/// onto the skater's reference segment, then the skater's solved pose drives it.
/// One arm solved from the skater's joint positions instead of rotations (see
/// `sk_rig_pose`): host bone indices, and the host's bind directions.
struct Arm {
    side: &'static str, // "LEFT" or "RIGHT"
    upper: usize,
    fore: usize,
    hand: usize,
    clavicle: Option<usize>, // the upper arm's parent, if it follows the skater
    helper: Option<usize>,   // an unmapped bone on the shoulder joint, which takes part of the upper arm's turn
    seat: Vec3,              // a carried board's shift in this hand's own axes (host units), see RIGHT_HAND_SEAT
    upper_dir: Vec3,    // shoulder to elbow, host bind (model space)
    fore_dir: Vec3,     // elbow to wrist
    hinge: Vec3,        // elbow axis: bending around it swings the forearm forward
    upper_len: f32,     // host bone lengths
    fore_len: f32,
    reach_ratio: f32,   // host arm length / the skater's
}

/// How strongly the host's arms follow the skater (see `sk_rig_tuning`).
#[derive(Clone, Copy)]
struct ArmTuning {
    elbow_follow: f32,    // 0: elbows hang naturally, 1: point like the skater's
    clavicle_follow: f32, // 0: shoulders ride the chest, 1: move like the skater's
}

struct Rig {
    bones: Vec<RigBone>,
    facing: bevy::math::Quat,              // host model forward onto the skater's
    height_ratio: f32,                     // host hips-over-toes height / the skater's
    arms: Vec<Arm>,
    chest: Option<usize>,                  // the host bone following SPINE2
    spine: Option<usize>,                  // the host bone following SPINE (a grab leans the body about it)
    tuning: ArmTuning,
    reference: std::collections::HashMap<String, Mat4>, // skater bind pose (Skate space), from rig.json
}

impl Rig {
    /// Whether host bone `i` is `ancestor` or hangs below it.
    fn below(&self, i: usize, ancestor: usize) -> bool {
        let mut at = Some(i);
        while let Some(b) = at {
            if b == ancestor {
                return true;
            }
            at = self.bones[b].parent;
        }
        false
    }
}

#[derive(serde::Deserialize)]
struct ReferenceBone {
    name: String,
    bind: [f32; 16],
}

/// A skeleton's body frame, from bone positions by Skate name: up runs hips
/// to chest, forward heel to toe (both feet), right = forward x up.
fn body_frame(pos: impl Fn(&str) -> Option<Vec3>) -> Option<bevy::math::Mat3> {
    let up = (pos("SPINE2")? - pos("HIPS")?).try_normalize()?;
    let toes = (pos("LEFTTOEBASE")? - pos("LEFTFOOT")?) + (pos("RIGHTTOEBASE")? - pos("RIGHTFOOT")?);
    let forward = (toes - up * toes.dot(up)).try_normalize()?;
    Some(bevy::math::Mat3::from_cols(forward.cross(up), forward, up))
}

/// A rig constant, which test runs can override through the environment
/// variable SK_<name> (the plugin's -sktest scripts set them).
fn tuned(name: &str, default: f32) -> f32 {
    std::env::var(format!("SK_{name}")).ok().and_then(|v| v.parse::<f32>().ok()).filter(|v| v.is_finite()).unwrap_or(default)
}

unsafe fn optional_name(p: *const c_char) -> Option<String> {
    (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
}

/// The rail finder was tuned on MW2 maps, in inches.
const RAIL_UNITS_PER_METER: f32 = 1.0 / 0.0254;

/// Finds rails in host-space triangles and builds Skate collision from both.
fn prepare_world(builder: CollisionBuilder, host: Vec<[[f32; 3]; 3]>, meters_per_unit: f32, generation: u32) -> Result<Built, String> {
    let started = std::time::Instant::now();
    let to_rail_units = meters_per_unit * RAIL_UNITS_PER_METER;
    let rail_space: Vec<[Vec3; 3]> = host.iter().map(|t| t.map(|p| Vec3::from_array(p) * to_rail_units)).collect();
    let (found, _) = rails::find(&rail_space);
    let rails: Vec<Vec<[f32; 3]>> = found
        .iter()
        .map(|rail| rail.iter().map(|p| to_skate(*p / to_rail_units, meters_per_unit).to_array()).collect())
        .collect();
    let tris: Vec<[[f32; 3]; 3]> = host
        .iter()
        .filter_map(|t| {
            let p = t.map(|v| to_skate(Vec3::from_array(v), meters_per_unit));
            let area = (p[1] - p[0]).cross(p[2] - p[0]).length_squared();
            (area > 1e-12).then(|| p.map(|v| v.to_array()))
        })
        .collect();
    let mut info = SkWorldInfo { generation, triangles: tris.len() as u32, rails: rails.len() as u32, ..SkWorldInfo::default() };
    let prepared = builder.build(tris, rails)?;
    info.build_ms = started.elapsed().as_secs_f32() * 1000.0;
    Ok((generation, prepared, info))
}

/// Finite host-space triangles from 9 floats each.
unsafe fn host_triangles(triangles: *const f32, triangle_count: u32) -> Result<Vec<[[f32; 3]; 3]>, String> {
    let flat = unsafe { slice(triangles, count(triangle_count, 9)?) }?;
    Ok(flat
        .chunks_exact(9)
        .map(|t| [[t[0], t[1], t[2]], [t[3], t[4], t[5]], [t[6], t[7], t[8]]])
        .filter(|t| t.iter().flatten().all(|v| v.is_finite()))
        .collect())
}

type Job = Box<dyn FnOnce(&mut Worker) + Send>;

pub struct SkSession {
    jobs: Option<mpsc::Sender<Job>>,
    thread: Option<JoinHandle<()>>,
    meters_per_unit: f32,
    generation: u32, // of the last world requested
    root: PathBuf,   // the converted Skate 3 data
    rig: Option<Rig>,
    rig_debug: Vec<f32>, // per arm, from the last sk_rig_pose (see sk_rig_debug)
    board: Option<Board>, // loaded by sk_board_mesh
    board_shift: Mat4, // moves the board into the host's hand (root-relative host space), from the last sk_rig_pose
    controller: c_int,
    pose: Option<SkPose>,
    bones: Vec<[f32; 16]>,
    names: Vec<String>,
    c_names: Vec<CString>,
    state: CString,
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}
static PANIC: Mutex<String> = Mutex::new(String::new());

/// Remembers the last engine panic so a crashed worker can say why.
fn install_panic_hook() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let default = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            if let Ok(mut last) = PANIC.lock() {
                *last = info.to_string();
            }
            default(info);
        }));
    });
}

fn crashed() -> String {
    let why = PANIC.lock().map(|p| p.clone()).unwrap_or_default();
    format!("the skate engine stopped: {why}")
}

fn cstring(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).unwrap_or_default()
}

fn report<T>(result: Result<T, String>, failed: T) -> T {
    result.unwrap_or_else(|e| {
        LAST_ERROR.with(|last| *last.borrow_mut() = cstring(&e));
        failed
    })
}

fn to_skate(p: Vec3, meters_per_unit: f32) -> Vec3 {
    Vec3::new(p.x, p.z, -p.y) * meters_per_unit
}

fn from_skate(p: Vec3, meters_per_unit: f32) -> Vec3 {
    Vec3::new(p.x, -p.z, p.y) / meters_per_unit
}

/// Skate's Y-up axes written in the host's Z-up axes.
fn basis() -> Mat4 {
    Mat4::from_cols(Vec4::X, Vec4::Z, -Vec4::Y, Vec4::W)
}

fn count(n: u32, per: usize) -> Result<usize, String> {
    (n as usize).checked_mul(per).ok_or_else(|| "count is too large".into())
}

unsafe fn slice<'a, T>(p: *const T, n: usize) -> Result<&'a [T], String> {
    if n == 0 {
        return Ok(&[]);
    }
    if p.is_null() {
        return Err("null pointer passed with a non-zero count".into());
    }
    Ok(unsafe { std::slice::from_raw_parts(p, n) })
}

unsafe fn path(p: *const c_char) -> Result<PathBuf, String> {
    if p.is_null() {
        return Err("assets_dir is null".into());
    }
    let s = unsafe { CStr::from_ptr(p) }
        .to_str()
        .map_err(|_| "assets_dir is not UTF-8")?;
    Ok(PathBuf::from(s))
}

unsafe fn handle<'a>(session: *mut SkSession) -> Result<&'a mut SkSession, String> {
    unsafe { session.as_mut() }.ok_or_else(|| "session is null".into())
}

type Collision = (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>);

/// Host-space triangles (9 floats each) and rails (points, with one length
/// per rail) as the Skate-space arrays the engine takes.
unsafe fn collision(
    triangles: *const f32,
    triangle_count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
    meters_per_unit: f32,
) -> Result<Collision, String> {
    let flat = unsafe { slice(triangles, count(triangle_count, 9)?) }?;
    let tris = flat
        .chunks_exact(9)
        .filter_map(|t| {
            let p = [0, 3, 6].map(|i| to_skate(Vec3::from_slice(&t[i..i + 3]), meters_per_unit));
            let area = (p[1] - p[0]).cross(p[2] - p[0]).length_squared();
            (p.iter().all(|v| v.is_finite()) && area > 1e-12).then(|| p.map(|v| v.to_array()))
        })
        .collect();
    let lengths = unsafe { slice(rail_lengths, rail_count as usize) }?;
    let total = lengths
        .iter()
        .try_fold(0usize, |sum, &n| sum.checked_add(n as usize))
        .ok_or("rail lengths are too large")?;
    let points = unsafe { slice(rail_points, total.checked_mul(3).ok_or("rails are too large")?) }?;
    let mut rails = Vec::with_capacity(lengths.len());
    let mut at = 0;
    for &n in lengths {
        let end = at + n as usize * 3;
        let rail: Vec<[f32; 3]> = points[at..end]
            .chunks_exact(3)
            .map(|p| to_skate(Vec3::from_slice(p), meters_per_unit).to_array())
            .collect();
        at = end;
        if rail.len() >= 2 && rail.iter().flatten().all(|v| v.is_finite()) {
            rails.push(rail);
        }
    }
    Ok((tris, rails))
}

fn run(mut worker: Worker, jobs: mpsc::Receiver<Job>) {
    while let Ok(job) = jobs.recv() {
        // After a panic the engine's state is unknown: stop, and let every
        // later call report the crash.
        if catch_unwind(AssertUnwindSafe(|| job(&mut worker))).is_err() {
            return;
        }
    }
}

impl SkSession {
    /// Runs `job` on the engine thread and waits for its answer.
    fn call<R: Send + 'static>(
        &self,
        job: impl FnOnce(&mut Worker) -> Result<R, String> + Send + 'static,
    ) -> Result<R, String> {
        let jobs = self.jobs.as_ref().ok_or("the session is closed")?;
        let (reply, answer) = mpsc::sync_channel(1);
        jobs.send(Box::new(move |worker| {
            let _ = reply.send(job(worker));
        }))
        .map_err(|_| crashed())?;
        answer.recv().map_err(|_| crashed())?
    }

    /// Converts a pose to host space and keeps it for the getters.
    fn store(&mut self, p: Pose) -> Result<(), String> {
        if !p.root.is_finite() || p.bones.iter().any(|b| !b.is_finite()) {
            return Err("the skate engine produced a non-finite pose".into());
        }
        let mpu = self.meters_per_unit;
        let b = basis();
        let mut root = b * p.root * b.inverse();
        root.w_axis = from_skate(p.root.w_axis.truncate(), mpu).extend(1.);
        let mut pose = SkPose {
            tick: p.tick,
            root: root.to_cols_array(),
            velocity: from_skate(p.velocity, mpu).to_array(),
            bone_count: p.bones.len() as u32,
            ..SkPose::default()
        };
        if let Some((position, axes, fov)) = p.camera {
            pose.has_camera = 1;
            pose.camera_position = from_skate(position, mpu).to_array();
            pose.camera_forward = b.transform_vector3(axes.z_axis).normalize().to_array();
            pose.camera_up = b.transform_vector3(axes.y_axis).normalize().to_array();
            pose.camera_fov = fov;
        }
        self.pose = Some(pose);
        self.bones = p.bones.iter().map(Mat4::to_cols_array).collect();
        if p.names != self.names {
            self.c_names = p.names.iter().map(|n| cstring(n)).collect();
            self.names = p.names;
        }
        self.state = cstring(&p.state);
        Ok(())
    }
}

impl Drop for SkSession {
    fn drop(&mut self) {
        drop(self.jobs.take());
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Starts a session on Skate's "easy" physics: loads the converted Skate 3
/// data from `assets_dir` and builds the world from host-space collision.
/// Returns null on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_session_new(
    assets_dir: *const c_char,
    triangles: *const f32,
    triangle_count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
    meters_per_unit: f32,
) -> *mut SkSession {
    unsafe {
        create(assets_dir, triangles, triangle_count, rail_points, rail_lengths, rail_count, meters_per_unit, "easy".into())
    }
}

/// `sk_session_new` with a physics difficulty: "easy", "normal" (Skate 3's
/// default; also used for null) or "hardcore".
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_session_new_mode(
    assets_dir: *const c_char,
    triangles: *const f32,
    triangle_count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
    meters_per_unit: f32,
    difficulty: *const c_char,
) -> *mut SkSession {
    let difficulty = if difficulty.is_null() {
        "normal".to_owned()
    } else {
        unsafe { CStr::from_ptr(difficulty) }.to_string_lossy().trim().to_ascii_lowercase()
    };
    unsafe {
        create(assets_dir, triangles, triangle_count, rail_points, rail_lengths, rail_count, meters_per_unit, difficulty)
    }
}

#[allow(clippy::too_many_arguments)]
unsafe fn create(
    assets_dir: *const c_char,
    triangles: *const f32,
    triangle_count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
    meters_per_unit: f32,
    difficulty: String,
) -> *mut SkSession {
    install_panic_hook();
    let made = (|| -> Result<SkSession, String> {
        if !(meters_per_unit.is_finite() && meters_per_unit > 0.) {
            return Err("meters_per_unit must be a positive number".into());
        }
        let root = unsafe { path(assets_dir) }?;
        let rig_root = root.clone();
        let (tris, rails) = unsafe {
            collision(triangles, triangle_count, rail_points, rail_lengths, rail_count, meters_per_unit)
        }?;
        let (jobs, inbox) = mpsc::channel::<Job>();
        let (ready, started) = mpsc::sync_channel(1);
        let thread = std::thread::Builder::new()
            .name("skate-engine".into())
            .stack_size(WORKER_STACK)
            .spawn(move || {
                let made = catch_unwind(AssertUnwindSafe(|| {
                    Session::with_difficulty(&root, tris, rails, [0.; 3], 0., &difficulty)
                }));
                match made {
                    Ok(Ok(session)) => {
                        let _ = ready.send(Ok(()));
                        let transport = ControllerTransport::default();
                        let worker = Worker {
                            session,
                            transport,
                            accumulated: 0.,
                            pending: None,
                            world: SkWorldInfo::default(),
                        };
                        run(worker, inbox);
                    }
                    Ok(Err(e)) => {
                        let _ = ready.send(Err(e));
                    }
                    Err(_) => {
                        let _ = ready.send(Err(crashed()));
                    }
                }
            })
            .map_err(|e| format!("could not start the skate thread: {e}"))?;
        match started.recv().unwrap_or_else(|_| Err(crashed())) {
            Ok(()) => Ok(SkSession {
                jobs: Some(jobs),
                thread: Some(thread),
                meters_per_unit,
                generation: 0,
                root: rig_root,
                rig: None,
                rig_debug: Vec::new(),
                board: None,
                board_shift: Mat4::IDENTITY,
                controller: -1,
                pose: None,
                bones: Vec::new(),
                names: Vec::new(),
                c_names: Vec::new(),
                state: CString::default(),
            }),
            Err(e) => {
                let _ = thread.join();
                Err(e)
            }
        }
    })();
    report(made.map(|s| Box::into_raw(Box::new(s))), std::ptr::null_mut())
}

/// Stops the engine thread and frees the session. Null is ignored.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_session_free(session: *mut SkSession) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

/// Optional: decodes the animation banks ahead of time so `sk_session_new`
/// is faster. Blocks until done; safe to call from a loading thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_preload(assets_dir: *const c_char) -> c_int {
    install_panic_hook();
    let done = (|| -> Result<c_int, String> {
        let root = unsafe { path(assets_dir) }?;
        std::thread::Builder::new()
            .name("skate-preload".into())
            .stack_size(WORKER_STACK)
            .spawn(move || Session::preload(&root))
            .map_err(|e| format!("could not start the preload thread: {e}"))?
            .join()
            .map_err(|_| crashed())??;
        Ok(0)
    })();
    report(done, -1)
}

/// Replaces the collision the skater rides and grinds, e.g. as the host
/// streams the world in around the player.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_install_collision(
    session: *mut SkSession,
    triangles: *const f32,
    triangle_count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let (tris, rails) = unsafe {
            collision(triangles, triangle_count, rail_points, rail_lengths, rail_count, s.meters_per_unit)
        }?;
        s.call(move |w| {
            let prepared = w.session.collision_builder().build(tris, rails)?;
            w.session.install_collision(prepared)
        })?;
        Ok(0)
    })();
    report(done, -1)
}

/// Builds the world the skater rides from host-space triangles (9 floats
/// each), with grind rails found in them, and installs it before returning.
/// Blocks while building; use when the skater needs ground right now.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_install_world(session: *mut SkSession, triangles: *const f32, triangle_count: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let host = unsafe { host_triangles(triangles, triangle_count) }?;
        let builder = s.call(|w| Ok(w.session.collision_builder()))?;
        s.generation += 1;
        let (generation, mpu) = (s.generation, s.meters_per_unit);
        // Built on its own big-stack thread, like everything else Skate runs.
        let built = std::thread::Builder::new()
            .name("skate-world".into())
            .stack_size(WORKER_STACK)
            .spawn(move || prepare_world(builder, host, mpu, generation))
            .map_err(|e| format!("could not start the world thread: {e}"))?
            .join()
            .map_err(|_| crashed())??;
        s.call(move |w| w.install(built))?;
        Ok(generation as c_int)
    })();
    report(done, -1)
}

/// `sk_install_world` on a background thread: returns at once with the
/// world's generation, and the first `sk_update`/`sk_step` after it finishes
/// installs it. A newer request replaces one still building.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_queue_world(session: *mut SkSession, triangles: *const f32, triangle_count: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let host = unsafe { host_triangles(triangles, triangle_count) }?;
        let builder = s.call(|w| Ok(w.session.collision_builder()))?;
        s.generation += 1;
        let (generation, mpu) = (s.generation, s.meters_per_unit);
        let (send, receive) = mpsc::sync_channel(1);
        std::thread::Builder::new()
            .name("skate-world".into())
            .stack_size(WORKER_STACK)
            .spawn(move || {
                let built = catch_unwind(AssertUnwindSafe(|| prepare_world(builder, host, mpu, generation)))
                    .unwrap_or_else(|_| Err(crashed()));
                let _ = send.send(built); // fails only if a newer request replaced this one
            })
            .map_err(|e| format!("could not start the world thread: {e}"))?;
        s.call(move |w| {
            w.pending = Some(receive);
            Ok(())
        })?;
        Ok(generation as c_int)
    })();
    report(done, -1)
}

/// What the installed world holds: its generation (compare with what
/// `sk_install_world`/`sk_queue_world` returned), triangles, rails, failed
/// background builds so far, and how long it took to build.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_world_info(session: *mut SkSession, out: *mut SkWorldInfo) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let out = unsafe { out.as_mut() }.ok_or("out is null")?;
        *out = s.call(|w| Ok(w.world))?;
        Ok(0)
    })();
    report(done, -1)
}

/// Describes the host character's skeleton (parents before children) so
/// `sk_rig_pose` can fit it to the skater. `facing_yaw`: radians around the
/// host's up axis that turn the host model's forward onto the skater's (pi
/// for a model facing +Y, like San Andreas peds). Reads rig.json from the
/// converted data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_rig_setup(session: *mut SkSession, bones: *const SkRigBone, count: u32, facing_yaw: f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let given = unsafe { slice(bones, count as usize) }?;
        board_export::ensure(&s.root)?; // made from the player's own skater.glb on first use
        let file = s.root.join("rig.json");
        let data = std::fs::read(&file).map_err(|e| format!("cannot read {}: {e}", file.display()))?;
        let reference: Vec<ReferenceBone> =
            serde_json::from_slice(&data).map_err(|e| format!("{} is not a Skate rig: {e}", file.display()))?;
        let mut rig_bones = Vec::with_capacity(given.len());
        for (i, b) in given.iter().enumerate() {
            let index = |v: i32, what: &str| -> Result<Option<usize>, String> {
                match v {
                    -1 => Ok(None),
                    v if v >= 0 && (v as usize) < given.len() && (what == "child" || (v as usize) < i) => Ok(Some(v as usize)),
                    v => Err(format!("bone {i} has an invalid {what} {v}")),
                }
            };
            rig_bones.push(RigBone {
                skate: unsafe { optional_name(b.skate) },
                skate_child: unsafe { optional_name(b.skate_child) },
                parent: index(b.parent, "parent")?,
                child: index(b.child, "child")?,
                bind: Mat4::from_cols_array(&b.bind),
            });
        }
        let missing: Vec<&str> = rig_bones
            .iter()
            .flat_map(|b| [b.skate.as_deref(), b.skate_child.as_deref()])
            .flatten()
            .filter(|name| !reference.iter().any(|r| r.name == *name))
            .collect();
        if !missing.is_empty() {
            return Err(format!("rig.json has no bones named {}", missing.join(", ")));
        }
        let reference: std::collections::HashMap<String, Mat4> =
            reference.into_iter().map(|r| (r.name, Mat4::from_cols_array(&r.bind))).collect();
        let mpu = s.meters_per_unit;
        let skate_pos = |name: &str| reference.get(name).map(|m| from_skate(m.w_axis.truncate(), mpu));
        // The deepest host bone following a name (pelvis over root).
        let host_pos = |name: &str| {
            rig_bones.iter().rev().find(|b| b.skate.as_deref() == Some(name)).map(|b| b.bind.w_axis.truncate())
        };
        let host = body_frame(&host_pos).ok_or("the host skeleton has no hips, chest or feet to measure")?;
        let skate = body_frame(&skate_pos).ok_or("rig.json has no hips, chest or feet to measure")?;
        // NaN: the turn between the two skeletons' body frames, rather than an
        // assumption about how the host model is oriented.
        let facing = if facing_yaw.is_nan() {
            bevy::math::Quat::from_mat3(&(skate * host.transpose())).normalize()
        } else {
            bevy::math::Quat::from_rotation_z(facing_yaw)
        };
        let hips_height = |pos: &dyn Fn(&str) -> Option<Vec3>, up: Vec3| {
            Some((pos("HIPS")? - (pos("LEFTTOEBASE")? + pos("RIGHTTOEBASE")?) * 0.5).dot(up))
        };
        let height_ratio = hips_height(&host_pos, host.z_axis)
            .zip(hips_height(&skate_pos, skate.z_axis))
            .map(|(h, s)| (h / s).clamp(0.5, 2.0))
            .filter(|r| r.is_finite())
            .unwrap_or(1.0);
        // Arms: the host's bind directions and its elbow hinge. The hinge is
        // taken as the axis that swings the forearm toward the body's front,
        // which is how an elbow bends from a T- or A-pose.
        let mut arms = Vec::new();
        for side in ["LEFT", "RIGHT"] {
            let find = |name: String| rig_bones.iter().rposition(|b| b.skate.as_deref() == Some(name.as_str()));
            let (Some(upper), Some(fore), Some(hand)) =
                (find(format!("{side}ARM")), find(format!("{side}FOREARM")), find(format!("{side}HAND")))
            else {
                continue;
            };
            let at = |i: usize| rig_bones[i].bind.w_axis.truncate();
            let (Some(upper_dir), Some(fore_dir)) = ((at(fore) - at(upper)).try_normalize(), (at(hand) - at(fore)).try_normalize())
            else {
                continue;
            };
            let Some(hinge) = upper_dir.cross(host.y_axis).try_normalize() else { continue };
            let (upper_len, fore_len) = ((at(fore) - at(upper)).length(), (at(hand) - at(fore)).length());
            let skate_at = |part: &str| skate_pos(&format!("{side}{part}"));
            let skate_reach = skate_at("ARM")
                .zip(skate_at("FOREARM"))
                .zip(skate_at("HAND"))
                .map(|((s, e), w)| (e - s).length() + (w - e).length())
                .unwrap_or(upper_len + fore_len);
            let reach_ratio = ((upper_len + fore_len) / skate_reach).clamp(0.5, 2.0);
            let clavicle = rig_bones[upper].parent.filter(|&p| rig_bones[p].skate.is_some());
            let helper = (0..rig_bones.len())
                .find(|&i| i != upper && rig_bones[i].skate.is_none() && at(i).distance(at(upper)) < 0.01);
            let seat = Vec3::ZERO; // set below, once both hands are known
            arms.push(Arm { side, upper, fore, hand, clavicle, helper, seat, upper_dir, fore_dir, hinge, upper_len, fore_len, reach_ratio });
        }
        // A carried board sits where the skater holds it, shifted to seat its
        // edge between the host's thumb and fingers: RIGHT_HAND_SEAT, found by
        // photographing CJ's right hand (tests/grip-tune.txt), in that hand's
        // own axes. The left hand gets its mirror image through the bind pose
        // (the host's left is +Y), as its bone axes are mirrored too.
        const RIGHT_HAND_SEAT: Vec3 = Vec3::new(0.0, 0.040, 0.0); // metres; +Y is toward the palm
        let rotation = |i: usize| bevy::math::Mat3::from_mat4(rig_bones[i].bind);
        let right = arms.iter().find(|a| a.side == "RIGHT").map(|a| a.hand);
        let mirror = bevy::math::Mat3::from_diagonal(Vec3::new(1.0, -1.0, 1.0));
        for arm in &mut arms {
            arm.seat = match (arm.side, right) {
                ("RIGHT", _) => RIGHT_HAND_SEAT,
                (_, Some(r)) => rotation(arm.hand).transpose() * (mirror * (rotation(r) * RIGHT_HAND_SEAT)),
                _ => Vec3::ZERO,
            } / mpu;
        }
        let chest = rig_bones.iter().rposition(|b| b.skate.as_deref() == Some("SPINE2"));
        let spine = rig_bones.iter().rposition(|b| b.skate.as_deref() == Some("SPINE"));
        let tuning = ArmTuning { elbow_follow: 0.5, clavicle_follow: 0.5 };
        s.rig = Some(Rig { bones: rig_bones, facing, height_ratio, arms, chest, spine, tuning, reference });
        Ok(given.len() as c_int)
    })();
    report(done, -1)
}

/// How the host's arms follow the skater, each 0..1: `elbow_follow` is how
/// far (times 90 degrees) an upper arm may roll round itself to point its
/// elbow like the skater's (0: never, the elbow keeps its natural bend);
/// `clavicle_follow` 0 keeps the shoulders on the chest, 1 moves them like
/// the skater's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_rig_tuning(session: *mut SkSession, elbow_follow: f32, clavicle_follow: f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let rig = s.rig.as_mut().ok_or("call sk_rig_setup first")?;
        let unit = |v: f32| if v.is_finite() { v.clamp(0.0, 1.0) } else { 0.5 };
        rig.tuning = ArmTuning { elbow_follow: unit(elbow_follow), clavicle_follow: unit(clavicle_follow) };
        Ok(0)
    })();
    report(done, -1)
}

/// The turn `sk_rig_setup` settled on, as a quaternion (x, y, z, w) in `out`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_rig_facing(session: *mut SkSession, out: *mut f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let rig = s.rig.as_ref().ok_or("call sk_rig_setup first")?;
        if out.is_null() {
            return Err("out is null".into());
        }
        unsafe { std::slice::from_raw_parts_mut(out, 4) }.copy_from_slice(&rig.facing.to_array());
        Ok(0)
    })();
    report(done, -1)
}

/// The host skeleton posed like the skater: one world-space matrix (host
/// space, column-major, 16 floats) per `sk_rig_setup` bone, written to
/// `out`. Returns how many were written.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_rig_pose(session: *mut SkSession, out: *mut f32, count: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let rig = s.rig.as_ref().ok_or("call sk_rig_setup first")?;
        if s.pose.is_none() {
            return Err("no pose yet: call sk_activate first".into());
        }
        let n = rig.bones.len();
        if (count as usize) < n {
            return Err(format!("out holds {count} matrices; the rig has {n}"));
        }
        if out.is_null() {
            return Err("out is null".into());
        }
        let mpu = s.meters_per_unit;
        let b = basis();
        let convert = |m: Mat4| {
            let mut o = b * m * b.inverse();
            o.w_axis = from_skate(m.w_axis.truncate(), mpu).extend(1.);
            o
        };
        let bind_of = |name: &str| rig.reference.get(name).map(|m| convert(*m));
        let posed_of = |name: &str| {
            let k = s.names.iter().position(|n| n == name)?;
            Some(convert(Mat4::from_cols_array(s.bones.get(k)?)))
        };
        let turn = |host_from: Vec3, host_to: Vec3, skate_from: Vec3, skate_to: Vec3| {
            Some(
                bevy::math::Quat::from_rotation_arc(
                    (rig.facing * (host_to - host_from)).try_normalize()?,
                    (skate_to - skate_from).try_normalize()?,
                ) * rig.facing,
            )
        };
        // Each mapped bone takes the skater's rotation (the mashup's fit), but
        // sits at the host's own bone length from its parent, so a differently
        // proportioned body bends instead of stretching. The top of the mapped
        // chain sits where the skater's hips are, raised or lowered by the
        // leg-length ratio so the host's feet still reach the board.
        let mut fits: Vec<Option<Mat4>> = Vec::with_capacity(n);
        for bone in &rig.bones {
            let fitted = bone.skate.as_deref().and_then(|name| {
                let bind = bind_of(name)?;
                let posed = posed_of(name)?;
                let from = bone.bind.w_axis.truncate();
                let to = bind.w_axis.truncate();
                // Along its own segment; a terminal joint keeps its parent's
                // alignment so wrists and toes stay attached.
                let rotation = bone
                    .child
                    .zip(bone.skate_child.as_deref())
                    .and_then(|(c, sc)| turn(from, rig.bones[c].bind.w_axis.truncate(), to, bind_of(sc)?.w_axis.truncate()))
                    .or_else(|| {
                        let p = bone.parent?;
                        let parent_name = rig.bones[p].skate.as_deref()?;
                        turn(rig.bones[p].bind.w_axis.truncate(), from, bind_of(parent_name)?.w_axis.truncate(), to)
                    })
                    .unwrap_or(rig.facing);
                let fit = Mat4::from_rotation_translation(rotation, to - rotation * from);
                Some(posed * bind.inverse() * fit)
            });
            fits.push(fitted);
        }
        // Shins bend fully but twist around their own axis at most
        // MAX_LIMB_TWIST against the thigh: the host has no twist bones. The
        // rest shows at the ankle, because feet keep the skater's rotation.
        // (Arms are solved separately below.)
        const MAX_LIMB_TWIST: f32 = 0.35; // radians, about 20 degrees
        for i in 0..n {
            let bone = &rig.bones[i];
            let limb = bone.skate.as_deref().is_some_and(|s| s == "LEFTLEG" || s == "RIGHTLEG");
            let (Some(p), Some(skin)) = (bone.parent, fits[i]) else { continue };
            let Some(parent_skin) = fits[p] else { continue };
            if !limb {
                continue;
            }
            let rotation = bevy::math::Quat::from_mat4(&skin).normalize();
            let parent_rotation = bevy::math::Quat::from_mat4(&parent_skin).normalize();
            // The bone's own axis, posed: toward its child, or away from its parent.
            let along = match bone.child {
                Some(c) => rig.bones[c].bind.w_axis.truncate() - bone.bind.w_axis.truncate(),
                None => bone.bind.w_axis.truncate() - rig.bones[p].bind.w_axis.truncate(),
            };
            let Some(axis) = (rotation * along).try_normalize() else { continue };
            let relative = rotation * parent_rotation.inverse();
            let projected = Vec3::new(relative.x, relative.y, relative.z).dot(axis) * axis;
            let twist = bevy::math::Quat::from_xyzw(projected.x, projected.y, projected.z, relative.w);
            if twist.length_squared() < 1e-12 {
                continue;
            }
            let twist = twist.normalize();
            let swing = relative * twist.inverse();
            let (twist_axis, angle) = twist.to_axis_angle();
            let angle = if angle > std::f32::consts::PI { angle - 2.0 * std::f32::consts::PI } else { angle };
            let limited = bevy::math::Quat::from_axis_angle(twist_axis, angle.clamp(-MAX_LIMB_TWIST, MAX_LIMB_TWIST));
            fits[i] = Some(Mat4::from_quat(swing * limited * parent_rotation));
        }
        // Arms: two-bone IK onto the skater's wrist. The hand goes where the
        // skater's is (relative to the shoulder, scaled to the host's reach)
        // and the elbow points like the skater's, bending around the host's
        // own hinge; the hand follows the forearm. The host has no twist
        // bones and its shoulders are a clavicle each, so two things a real
        // shoulder does are added: the clavicle lifts with an arm raised
        // above its bind height, and the upper arm rolls at most
        // MAX_ARM_TWIST * elbow_follow against the clavicle; past that the
        // elbow swings round the shoulder-to-wrist line instead (the hand
        // stays put). Otherwise the skin there collapses and stretches.
        const MAX_ARM_TWIST: f32 = std::f32::consts::FRAC_PI_2;
        let shoulder_lift = tuned("SHOULDER_LIFT", 0.3); // share of an arm's lift the clavicle takes...
        let lift_from = tuned("LIFT_FROM", 0.5); // ...once the arm is this far (radians) above its bind height
        let helper_share = tuned("HELPER", 0.5); // of the upper arm's turn, for a helper bone on the shoulder
        type Quat = bevy::math::Quat;
        let quat = |m: &Mat4| Quat::from_mat4(m).normalize();
        let chest = rig.chest.and_then(|c| fits[c]).map(|m| quat(&m));
        // The skater's own hand turns, for a board in a hand (below).
        let skater_hands: Vec<Option<Quat>> = rig.arms.iter().map(|a| fits[a.hand].map(|m| quat(&m))).collect();
        // Shoulders ride partly on the chest rather than copying the skater's
        // whole clavicle motion.
        for arm in &rig.arms {
            if let (Some(c), Some(chest)) = (arm.clavicle, chest)
                && let Some(own) = fits[c]
            {
                fits[c] = Some(Mat4::from_quat(chest.slerp(quat(&own), rig.tuning.clavicle_follow)));
            }
        }
        // Per arm, in degrees: reached the end (1), roll before and after the
        // limit, the clavicle's lift, the arm's and its bind's angle from down;
        // then the board grip (0..1), board under the feet (0..1) and how far
        // (cm) a grabbing hand stays short of the board.
        const DEBUG: usize = 9;
        let mut debug = vec![0.0f32; rig.arms.len() * DEBUG];
        // One arm onto `reach` (from its shoulder, host axes). `first`: the
        // first solve, which may still lift the clavicle.
        let solve_arm = |arm: &Arm, reach: Vec3, first: bool, fits: &mut [Option<Mat4>], debug: &mut [f32]| -> Option<()> {
            let clavicle = arm.clavicle.and_then(|c| fits[c]).map(|m| quat(&m));
            let joint = |part: &str| posed_of(&format!("{}{part}", arm.side)).map(|m| m.w_axis.truncate());
            let (shoulder, elbow) = (joint("ARM")?, joint("FOREARM")?);
            let toward = reach.try_normalize()?;
            let off_line = |v: Vec3| (v - toward * v.dot(toward)).try_normalize();
            let (a, b) = (arm.upper_len, arm.fore_len);
            let d = reach.length().clamp((a - b).abs() + 1e-3, a + b - 1e-3);
            let along = (a * a - b * b + d * d) / (2.0 * d);
            let out = (a * a - along * along).max(0.0).sqrt();
            // The arm with its elbow off the shoulder-wrist line along `pole`:
            // upper arm and forearm directions, and the elbow hinge.
            let place = |pole: Vec3| {
                let elbow_at = toward * along + pole * out;
                Some((elbow_at.try_normalize()?, (toward * d - elbow_at).try_normalize()?, pole.cross(toward).try_normalize()?))
            };
            let frame = |dir: Vec3, axis: Vec3| {
                let axis = (axis - dir * axis.dot(dir)).try_normalize()?;
                Some(bevy::math::Mat3::from_cols(dir, axis, dir.cross(axis)))
            };
            let solve = |bind_dir: Vec3, dir: Vec3, hinge: Vec3| Some(frame(dir, hinge)? * frame(bind_dir, arm.hinge)?.transpose());
            let reference = clavicle.or(chest).unwrap_or(rig.facing);
            let pole = off_line(elbow - shoulder).or_else(|| off_line(reference * arm.upper_dir.cross(arm.hinge)))?;
            let (first_up, _, _) = place(pole)?;
            // The clavicle lifts toward an arm raised above its bind height.
            let reference = match (arm.clavicle, clavicle, chest) {
                (Some(c), Some(own), Some(chest)) if first => {
                    let down = chest * Vec3::NEG_Z;
                    let bind_up = own * arm.upper_dir;
                    let lift = first_up.angle_between(down) - bind_up.angle_between(down) - lift_from;
                    debug[3] = (shoulder_lift * lift.max(0.0)).to_degrees();
                    debug[4] = first_up.angle_between(down).to_degrees();
                    debug[5] = bind_up.angle_between(down).to_degrees();
                    match bind_up.cross(first_up).try_normalize() {
                        Some(axis) if lift > 0.0 => {
                            let lifted = Quat::from_axis_angle(axis, shoulder_lift * lift) * own;
                            fits[c] = Some(Mat4::from_quat(lifted));
                            lifted
                        }
                        _ => own,
                    }
                }
                _ => reference,
            };
            // How far the upper arm rolls round itself against `reference`,
            // beyond the plain swing from its bind direction.
            let twist = |pole: Vec3| -> Option<f32> {
                let (up, _, hinge) = place(pole)?;
                let rolled = Quat::from_mat3(&solve(arm.upper_dir, up, hinge)?).normalize() * reference.inverse();
                let roll = rolled * Quat::from_rotation_arc((reference * arm.upper_dir).normalize(), up).inverse();
                let roll = if roll.w < 0.0 { -roll } else { roll };
                Some(2.0 * Vec3::new(roll.x, roll.y, roll.z).dot(up).atan2(roll.w))
            };
            let limit = MAX_ARM_TWIST * rig.tuning.elbow_follow;
            let turned = |angle: f32| Quat::from_axis_angle(toward, angle) * pole;
            let mut angle = 0.0;
            debug[1] = twist(pole).unwrap_or(f32::NAN).to_degrees();
            for _ in 0..4 {
                let Some(now) = twist(turned(angle)) else { break };
                let wanted = now.clamp(-limit, limit);
                if (now - wanted).abs() < 1e-3 {
                    break;
                }
                const H: f32 = 0.01;
                let (Some(ahead), Some(behind)) = (twist(turned(angle + H)), twist(turned(angle - H))) else { break };
                let slope = (ahead - behind) / (2.0 * H);
                if slope.abs() < 0.05 {
                    break;
                }
                angle += ((wanted - now) / slope).clamp(-1.0, 1.0);
            }
            let (up, fore, hinge) = place(turned(angle))?;
            let (upper, forearm) = (solve(arm.upper_dir, up, hinge)?, solve(arm.fore_dir, fore, hinge)?);
            fits[arm.upper] = Some(Mat4::from_mat3(upper));
            fits[arm.fore] = Some(Mat4::from_mat3(forearm));
            fits[arm.hand] = Some(Mat4::from_mat3(forearm));
            if let Some(h) = arm.helper {
                let turn = reference.slerp(Quat::from_mat3(&upper).normalize(), helper_share);
                fits[h] = Some(Mat4::from_quat(turn));
            }
            debug[0] = 1.0;
            debug[2] = twist(turned(angle)).unwrap_or(f32::NAN).to_degrees();
            Some(())
        };
        let wrist_of = |arm: &Arm| posed_of(&format!("{}HAND", arm.side)).map(|m| m.w_axis.truncate());
        for (arm, debug) in rig.arms.iter().zip(debug.chunks_exact_mut(DEBUG)) {
            let shoulder = posed_of(&format!("{}ARM", arm.side)).map(|m| m.w_axis.truncate());
            if let (Some(shoulder), Some(wrist)) = (shoulder, wrist_of(arm)) {
                solve_arm(arm, (wrist - shoulder) * arm.reach_ratio, true, &mut fits, debug);
            }
        }
        let place_all = |fits: &[Option<Mat4>]| -> Vec<Mat4> {
            let mut world: Vec<Mat4> = Vec::with_capacity(n);
            let mut follows: Vec<bool> = Vec::with_capacity(n); // the bone or an ancestor follows the skater
            for (i, bone) in rig.bones.iter().enumerate() {
                let fitted = fits[i];
                let local = match bone.parent {
                    Some(p) => rig.bones[p].bind.inverse() * bone.bind,
                    None => bone.bind,
                };
                let along_parent = bone.parent.filter(|&p| follows[p]).map(|p| world[p] * local);
                let placed = match (fitted, along_parent) {
                    (Some(skin), Some(chain)) => {
                        let posed = skin * bone.bind;
                        Mat4::from_cols(posed.x_axis, posed.y_axis, posed.z_axis, chain.w_axis)
                    }
                    (Some(skin), None) => {
                        let mut posed = skin * bone.bind;
                        posed.w_axis.z *= rig.height_ratio; // animation space: z up from the board
                        posed
                    }
                    (None, _) => bone.parent.map_or(bone.bind, |p| world[p] * local),
                };
                follows.push(fitted.is_some() || bone.parent.is_some_and(|p| follows[p]));
                world.push(placed);
            }
            world
        };
        let mut world = place_all(&fits);
        // A board in a hand. Each skater wrist within GRIP_NEAR of the board
        // grips it (fading out by GRIP_FAR, so taking and letting go don't
        // jump); the board is under the feet when a foot is within FEET_NEAR
        // (fading by FEET_FAR). Under the feet (a grab), the board stays and
        // the host's hand goes onto it where the skater's is, turned like the
        // skater's hand. Otherwise (carrying it), the board stays fixed in the
        // host's hand as the skater holds it, turning with the host's hand,
        // shifted by that hand's seat (see RIGHT_HAND_SEAT).
        const GRIP_NEAR: f32 = 0.10;
        const GRIP_FAR: f32 = 0.20;
        const FEET_NEAR: f32 = 0.15;
        const FEET_FAR: f32 = 0.35;
        let fade = |d: f32, near: f32, far: f32| {
            let t = ((d * mpu - near) / (far - near)).clamp(0.0, 1.0);
            1.0 - t * t * (3.0 - 2.0 * t)
        };
        let mut board_shift = Mat4::IDENTITY;
        if let Some(board) = s.board.as_ref()
            && let Ok(points) = board_points(board, &s.names, &s.bones, mpu)
        {
            let reach = |p: Vec3| points.iter().map(|(q, _)| q.distance_squared(p)).fold(f32::INFINITY, f32::min).sqrt();
            let feet = ["LEFTFOOT", "RIGHTFOOT", "LEFTTOEBASE", "RIGHTTOEBASE"]
                .iter()
                .filter_map(|f| posed_of(f).map(|m| reach(m.w_axis.truncate())))
                .fold(f32::INFINITY, f32::min);
            // Off the board (walking, running) the skater carries it; a board
            // swinging by the legs there is still being carried.
            let on_board = !s.state.to_bytes().starts_with(b"Biped");
            let under = if on_board { fade(feet, FEET_NEAR, FEET_FAR) } else { 0.0 };
            let grips: Vec<f32> = rig.arms.iter().map(|arm| wrist_of(arm).map_or(0.0, |w| fade(reach(w), GRIP_NEAR, GRIP_FAR))).collect();
            // The host is taller, so a grabbing hand can fall short of the
            // board: then the upper body leans toward it (about the lowest
            // spine bone, at most MAX_LEAN), as a person reaching down would.
            const MAX_LEAN: f32 = 0.45; // radians
            let grabbing = (0..rig.arms.len()).max_by(|&a, &b| (grips[a] * under).total_cmp(&(grips[b] * under)));
            if let Some(k) = grabbing.filter(|&k| grips[k] * under > 0.01)
                && let (Some(spine), Some(wrist)) = (rig.spine, wrist_of(&rig.arms[k]))
            {
                let arm = &rig.arms[k];
                let (pivot, shoulder) = (world[spine].w_axis.truncate(), world[arm.upper].w_axis.truncate());
                let (to_shoulder, to_wrist) = (shoulder - pivot, wrist - pivot);
                let arm_reach = (arm.upper_len + arm.fore_len) * 0.98;
                let (ls, lw) = (to_shoulder.length(), to_wrist.length());
                if (wrist - shoulder).length() > arm_reach
                    && ls > 1e-3
                    && lw > 1e-3
                    && let Some(axis) = to_shoulder.cross(to_wrist).try_normalize()
                {
                    let apart = to_shoulder.angle_between(to_wrist);
                    let wanted = ((lw * lw + ls * ls - arm_reach * arm_reach) / (2.0 * lw * ls)).clamp(-1.0, 1.0).acos();
                    let lean = Quat::from_axis_angle(axis, (apart - wanted).clamp(0.0, MAX_LEAN) * grips[k] * under);
                    for (i, fit) in fits.iter_mut().enumerate() {
                        if rig.below(i, spine)
                            && let Some(m) = fit
                        {
                            *m = Mat4::from_quat(lean) * *m;
                        }
                    }
                    world = place_all(&fits);
                }
            }
            let mut again = false;
            for (k, arm) in rig.arms.iter().enumerate() {
                let debug = &mut debug[k * DEBUG..(k + 1) * DEBUG];
                debug[6] = grips[k];
                debug[7] = under;
                let grab = grips[k] * under;
                let Some(wrist) = wrist_of(arm).filter(|_| grab > 0.01) else { continue };
                let target = world[arm.hand].w_axis.truncate().lerp(wrist, grab);
                if solve_arm(arm, target - world[arm.upper].w_axis.truncate(), false, &mut fits, debug).is_some() {
                    if let (Some(skater), Some(own)) = (skater_hands[k], fits[arm.hand]) {
                        fits[arm.hand] = Some(Mat4::from_quat(quat(&own).slerp(skater, grab)));
                    }
                    again = true;
                }
            }
            if again {
                world = place_all(&fits);
            }
            for (k, arm) in rig.arms.iter().enumerate() {
                let reached = world[arm.hand].w_axis.truncate();
                if let Some(wrist) = wrist_of(arm).filter(|_| grips[k] * under > 0.01) {
                    debug[k * DEBUG + 8] = reached.distance(wrist) * mpu * 100.0;
                }
            }
            let carry = rig
                .arms
                .iter()
                .enumerate()
                .map(|(k, arm)| (k, arm, grips[k] * (1.0 - under)))
                .max_by(|a, b| a.2.total_cmp(&b.2));
            if let Some((k, arm, weight)) = carry.filter(|c| c.2 > 0.01)
                && let (Some(wrist), Some(skater), Some(own)) = (wrist_of(arm), skater_hands[k], fits[arm.hand])
            {
                let hand = world[arm.hand];
                // GRIP_* (centimetres) nudge it further, for test runs.
                let seat = arm.seat + Vec3::new(tuned("GRIP_X", 0.0), tuned("GRIP_Y", 0.0), tuned("GRIP_Z", 0.0)) * 0.01 / mpu;
                let roll = Quat::from_axis_angle(hand.x_axis.truncate().normalize_or(Vec3::X), tuned("GRIP_ROLL", 0.0).to_radians());
                let turn = Quat::IDENTITY.slerp(roll * quat(&own) * skater.inverse(), weight);
                let to = wrist + (hand.w_axis.truncate() + hand.transform_vector3(seat) - wrist) * weight;
                board_shift = Mat4::from_translation(to) * Mat4::from_quat(turn) * Mat4::from_translation(-wrist);
            }
        }
        // Skate's bones are relative to the skater (animation space); the root
        // places them in the world.
        let root = Mat4::from_cols_array(&s.pose.as_ref().map(|p| p.root).unwrap_or_default());
        let out = unsafe { std::slice::from_raw_parts_mut(out, n * 16) };
        for (dst, w) in out.chunks_exact_mut(16).zip(&world) {
            let placed = root * *w;
            if !placed.is_finite() {
                return Err("the fitted pose is not finite".into());
            }
            dst.copy_from_slice(&placed.to_cols_array());
        }
        s.rig_debug = debug;
        s.board_shift = board_shift;
        Ok(n as c_int)
    })();
    report(done, -1)
}

// ----------------------------------------------------------------- board

/// One surface of the skateboard (deck, trucks, wheels), for the host to draw.
#[repr(C)]
pub struct SkBoardSurface {
    pub first_vertex: u32, // its vertices in sk_board_pose's output
    pub vertex_count: u32,
    pub indices: *const u16, // triangle list, relative to first_vertex
    pub index_count: u32,
    pub uvs: *const f32, // u, v per vertex of this surface
    pub rgba: *const u8, // its texture, width * height * 4 bytes, top row first
    pub width: u32,
    pub height: u32,
}
#[cfg(target_pointer_width = "32")]
const _: () = assert!(size_of::<SkBoardSurface>() == 32);

#[derive(serde::Deserialize)]
struct BoardFile {
    joints: Vec<BoardJointFile>,
    surfaces: Vec<BoardSurfaceFile>,
    textures: Vec<BoardTexture>,
}
#[derive(serde::Deserialize)]
struct BoardJointFile {
    target: String,
}
#[derive(serde::Deserialize)]
struct BoardSurfaceFile {
    texture: usize,
    vertices: Vec<BoardVertex>,
    indices: Vec<u32>,
}
#[derive(serde::Deserialize)]
struct BoardVertex {
    position: [f32; 3],
    normal: [f32; 3],
    uv: u32, // two IW4 halves: u high, v low
    joints: [usize; 4],
    weights: [f32; 4],
}
#[derive(serde::Deserialize)]
struct BoardTexture {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
}
#[derive(serde::Deserialize)]
struct InverseBind {
    name: String,
    inverse_bind: [f32; 16],
}

/// The skateboard from the converted data, skinned to the skater's board
/// bones as the mashup does it (render_anim/src/skate/rig.rs::board).
struct Board {
    joints: Vec<(String, Mat4)>, // the Skate bone, and what takes a vertex into its space
    vertices: Vec<BoardVertex>,
    uvs: Vec<[f32; 2]>,
    surfaces: Vec<(u32, u32, usize)>, // first vertex, vertex count, texture
    indices: Vec<Vec<u16>>,
    textures: Vec<BoardTexture>,
}

/// One half of IW4's packed texture coordinates (Vec2UnpackTexCoords).
fn unpack_half(h: u32) -> f32 {
    let h = h & 0xffff;
    if h == 0 {
        return 0.0;
    }
    let magnitude = (((h & 0x3fff) << 14).wrapping_sub(!(h << 14) & 0x1000_0000) ^ 0x8000_0001) >> 1;
    f32::from_bits(((h & 0x8000) << 16) | magnitude)
}

fn load_board(root: &std::path::Path) -> Result<Board, String> {
    board_export::ensure(root)?;
    let read = |name: &str| std::fs::read(root.join(name)).map_err(|e| format!("cannot read {name}: {e}"));
    let file: BoardFile = serde_json::from_slice(&read("board.json")?).map_err(|e| format!("board.json: {e}"))?;
    let binds: Vec<InverseBind> = serde_json::from_slice(&read("rig.json")?).map_err(|e| format!("rig.json: {e}"))?;
    // The inverse binds are Z up; the skater's bones are Skate's Y up.
    let rb = Mat4::from_cols(Vec4::X, -Vec4::Z, Vec4::Y, Vec4::W);
    let joints = file
        .joints
        .iter()
        .map(|j| {
            let bind = binds.iter().find(|b| b.name == j.target).ok_or_else(|| format!("rig.json has no bone {}", j.target))?;
            Ok((j.target.clone(), rb * Mat4::from_cols_array(&bind.inverse_bind)))
        })
        .collect::<Result<Vec<_>, String>>()?;
    for (i, t) in file.textures.iter().enumerate() {
        if t.rgba.len() != (t.width * t.height * 4) as usize {
            return Err(format!("board texture {i} is not {}x{} RGBA", t.width, t.height));
        }
    }
    let mut board = Board { joints, vertices: Vec::new(), uvs: Vec::new(), surfaces: Vec::new(), indices: Vec::new(), textures: file.textures };
    for (i, s) in file.surfaces.into_iter().enumerate() {
        let bad = s.texture >= board.textures.len()
            || s.vertices.len() > usize::from(u16::MAX)
            || s.indices.iter().any(|&k| k as usize >= s.vertices.len())
            || s.vertices.iter().any(|v| v.joints.iter().any(|&j| j >= board.joints.len()));
        if bad {
            return Err(format!("board surface {i} is malformed"));
        }
        board.surfaces.push((board.vertices.len() as u32, s.vertices.len() as u32, s.texture));
        board.indices.push(s.indices.iter().map(|&k| k as u16).collect());
        board.uvs.extend(s.vertices.iter().map(|v| [unpack_half(v.uv >> 16), unpack_half(v.uv)]));
        board.vertices.extend(s.vertices);
    }
    Ok(board)
}

/// The board's vertices (position, normal) where the skater holds it, in
/// root-relative host space.
fn board_points(board: &Board, names: &[String], bones: &[[f32; 16]], mpu: f32) -> Result<Vec<(Vec3, Vec3)>, String> {
    // Only the bones the vertices use need to exist in the skater.
    let mut used = vec![false; board.joints.len()];
    for v in &board.vertices {
        for (j, w) in v.joints.iter().zip(v.weights) {
            used[*j] |= w != 0.0;
        }
    }
    let matrices = board
        .joints
        .iter()
        .zip(&used)
        .map(|((name, to_joint), &used)| {
            if !used {
                return Ok(Mat4::IDENTITY);
            }
            let k = names.iter().position(|n| n == name).ok_or_else(|| format!("the skater has no bone {name}"))?;
            Ok(Mat4::from_cols_array(bones.get(k).ok_or("the pose has fewer bones than names")?) * *to_joint)
        })
        .collect::<Result<Vec<Mat4>, String>>()?;
    let b = basis();
    Ok(board
        .vertices
        .iter()
        .map(|v| {
            let (mut p, mut normal) = (Vec3::ZERO, Vec3::ZERO);
            for (&j, &w) in v.joints.iter().zip(&v.weights) {
                if w != 0.0 {
                    p += matrices[j].transform_point3(Vec3::from_array(v.position)) * w;
                    normal += matrices[j].transform_vector3(Vec3::from_array(v.normal)) * w;
                }
            }
            (from_skate(p, mpu), b.transform_vector3(normal))
        })
        .collect())
}

/// The skateboard's surfaces (loaded from board.json on the first call):
/// writes up to `max` into `out` and returns how many there are. The
/// pointers stay valid until the session is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_board_mesh(session: *mut SkSession, out: *mut SkBoardSurface, max: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        if s.board.is_none() {
            s.board = Some(load_board(&s.root)?);
        }
        let board = s.board.as_ref().expect("just loaded");
        for (i, &(first, count, texture)) in board.surfaces.iter().enumerate().take(max as usize) {
            if out.is_null() {
                break;
            }
            let t = &board.textures[texture];
            let uvs = &board.uvs[first as usize..(first + count) as usize];
            unsafe {
                *out.add(i) = SkBoardSurface {
                    first_vertex: first,
                    vertex_count: count,
                    indices: board.indices[i].as_ptr(),
                    index_count: board.indices[i].len() as u32,
                    uvs: uvs.as_ptr().cast(),
                    rgba: t.rgba.as_ptr(),
                    width: t.width,
                    height: t.height,
                };
            }
        }
        Ok(board.surfaces.len() as c_int)
    })();
    report(done, -1)
}

/// The board as the skater holds it now: 6 floats per vertex (position,
/// normal) in host world space, written to `out`. Returns the vertex count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_board_pose(session: *mut SkSession, out: *mut f32, max_vertices: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let board = s.board.as_ref().ok_or("call sk_board_mesh first")?;
        let pose = s.pose.as_ref().ok_or("no pose yet: call sk_activate first")?;
        let n = board.vertices.len();
        if (max_vertices as usize) < n || out.is_null() {
            return Err(format!("out holds {max_vertices} vertices; the board has {n}"));
        }
        let placed = board_points(board, &s.names, &s.bones, s.meters_per_unit)?;
        // In a hand, sk_rig_pose moved it to the host's (board_shift).
        let world = Mat4::from_cols_array(&pose.root) * s.board_shift;
        let out = unsafe { std::slice::from_raw_parts_mut(out, n * 6) };
        for ((p, normal), dst) in placed.iter().zip(out.chunks_exact_mut(6)) {
            let p = world.transform_point3(*p);
            let normal = world.transform_vector3(*normal).normalize_or_zero();
            dst.copy_from_slice(&[p.x, p.y, p.z, normal.x, normal.y, normal.z]);
        }
        Ok(n as c_int)
    })();
    report(done, -1)
}

/// What the last `sk_rig_pose` did with each arm, 6 floats per arm (see
/// the arm solver): copies up to `count` floats and returns how many there are.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_rig_debug(session: *mut SkSession, out: *mut f32, count: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let n = s.rig_debug.len().min(count as usize);
        if n > 0 && !out.is_null() {
            unsafe { std::slice::from_raw_parts_mut(out, n) }.copy_from_slice(&s.rig_debug[..n]);
        }
        Ok(s.rig_debug.len() as c_int)
    })();
    report(done, -1)
}

/// Puts the skater on the board at `position` (host space), facing `yaw`:
/// radians counter-clockwise from the host's +X axis around +Z.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_activate(session: *mut SkSession, position: *const f32, yaw: f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let at = Vec3::from_slice(unsafe { slice(position, 3) }?);
        let spawn = to_skate(at, s.meters_per_unit).to_array();
        let heading = yaw + std::f32::consts::FRAC_PI_2;
        let pose = s.call(move |w| {
            w.accumulated = 0.;
            w.session.activate(spawn, heading)
        })?;
        s.store(pose)?;
        Ok(0)
    })();
    report(done, -1)
}

/// Gives the board and skater a velocity (host units per second, host space),
/// e.g. to carry the host character's momentum onto the board. Call right
/// after `sk_activate`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_set_velocity(session: *mut SkSession, velocity: *const f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let v = Vec3::from_slice(unsafe { slice(velocity, 3) }?);
        if !v.is_finite() {
            return Err("velocity is not finite".into());
        }
        let v = to_skate(v, s.meters_per_unit).to_array();
        s.call(move |w| {
            w.session.set_velocity(v);
            Ok(())
        })?;
        Ok(0)
    })();
    report(done, -1)
}

/// A host vehicle hit the skater: Skate's own vehicle bail, with `velocity`
/// (host units per second, host space) added to the body and the board (or
/// `board_velocity` to the board, when not null), spinning at `spin` (rad/s,
/// host space; null: a tumble about the push), starting `lift` host units
/// higher (onto a car's hood).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_knock(
    session: *mut SkSession,
    velocity: *const f32,
    spin: *const f32,
    lift: f32,
    board_velocity: *const f32,
) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let v = Vec3::from_slice(unsafe { slice(velocity, 3) }?);
        let w = if spin.is_null() { None } else { Some(Vec3::from_slice(unsafe { slice(spin, 3) }?)) };
        let b = if board_velocity.is_null() { None } else { Some(Vec3::from_slice(unsafe { slice(board_velocity, 3) }?)) };
        if !v.is_finite() || w.is_some_and(|w| !w.is_finite()) || b.is_some_and(|b| !b.is_finite()) || !lift.is_finite() {
            return Err("velocity, spin, lift or board velocity is not finite".into());
        }
        let mpu = s.meters_per_unit;
        let v = to_skate(v, mpu).to_array();
        let w = w.map(|w| to_skate(w, 1.0).to_array()); // an axis: turned, not scaled
        let b = b.map(|b| to_skate(b, mpu).to_array());
        let lift = lift * mpu;
        s.call(move |worker| {
            worker.session.knock(v, w, lift, b);
            Ok(())
        })?;
        Ok(0)
    })();
    report(done, -1)
}

/// Advances the engine by `dt` seconds of host time, reading the Xbox pad
/// itself. Returns how many fixed engine steps ran (0 is normal on fast
/// frames); the pose updates whenever that is above 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_update(session: *mut SkSession, dt: f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let dt = if dt.is_finite() { dt.clamp(0., 0.1) } else { 0. };
        let (steps, controller, pose) = s.call(move |w| {
            w.install_built()?;
            let frame = w.transport.poll();
            let controller = frame.controller().map_or(-1, |i| i as c_int);
            w.session.collect(frame, dt);
            w.accumulated = (w.accumulated + dt).min(0.15);
            let mut steps = 0;
            // The engine's camera can change the period, so re-read it.
            while steps < MAX_STEPS && w.accumulated >= w.session.period() {
                let period = w.session.period();
                if !(period > 0.) {
                    return Err(format!("the skate engine reported a step period of {period}"));
                }
                w.accumulated -= period;
                w.session.advance()?;
                steps += 1;
            }
            Ok((steps, controller, (steps > 0).then(|| w.session.pose())))
        })?;
        s.controller = controller;
        if let Some(pose) = pose {
            s.store(pose)?;
        }
        Ok(steps)
    })();
    report(done, -1)
}

/// Runs exactly one fixed engine step with the given pad state, instead of
/// reading the pad. For keyboard-driven input, replays and testing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_step(session: *mut SkSession, controls: *const SkControls) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let c = *unsafe { controls.as_ref() }.ok_or("controls is null")?;
        let pose = s.call(move |w| {
            w.install_built()?;
            let input = Controls { buttons: c.buttons, triggers: c.triggers, left: c.left, right: c.right };
            w.session.tick(input)?;
            Ok(w.session.pose())
        })?;
        s.store(pose)?;
        Ok(1)
    })();
    report(done, -1)
}

/// Drops any held input, e.g. while a host menu is open.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_suspend_input(session: *mut SkSession) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        s.call(|w| {
            w.accumulated = 0.;
            w.session.suspend_input();
            Ok(())
        })?;
        Ok(0)
    })();
    report(done, -1)
}

/// The engine's fixed step length in seconds, or -1 on failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_period(session: *mut SkSession) -> f32 {
    let done = (|| -> Result<f32, String> {
        let s = unsafe { handle(session) }?;
        s.call(|w| Ok(w.session.period()))
    })();
    report(done, -1.)
}

/// Tells the Skate camera the host window's width / height.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_set_aspect_ratio(session: *mut SkSession, aspect_ratio: f32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        s.call(move |w| {
            w.session.set_aspect_ratio(aspect_ratio);
            Ok(())
        })?;
        Ok(0)
    })();
    report(done, -1)
}

/// Copies the latest pose into `out`. Fails until `sk_activate` has run.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_get_pose(session: *mut SkSession, out: *mut SkPose) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let out = unsafe { out.as_mut() }.ok_or("out is null")?;
        *out = s.pose.ok_or("no pose yet: call sk_activate first")?;
        Ok(0)
    })();
    report(done, -1)
}

/// Copies up to `max_bones` bone matrices (16 floats each, column-major, in
/// Skate's skater space) into `out`. Returns how many were copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_get_bones(session: *mut SkSession, out: *mut f32, max_bones: u32) -> c_int {
    let done = (|| -> Result<c_int, String> {
        let s = unsafe { handle(session) }?;
        let n = s.bones.len().min(max_bones as usize);
        if n > 0 {
            if out.is_null() {
                return Err("out is null".into());
            }
            let out = unsafe { std::slice::from_raw_parts_mut(out, n * 16) };
            for (dst, bone) in out.chunks_exact_mut(16).zip(&s.bones) {
                dst.copy_from_slice(bone);
            }
        }
        Ok(n as c_int)
    })();
    report(done, -1)
}

/// Name of bone `index`, or null if out of range. Valid until the session
/// is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_bone_name(session: *mut SkSession, index: u32) -> *const c_char {
    let Some(s) = (unsafe { session.as_ref() }) else {
        return std::ptr::null();
    };
    s.c_names
        .get(index as usize)
        .map_or(std::ptr::null(), |n| n.as_ptr())
}

/// The skater's current state name (e.g. riding, grinding, bailing). Valid
/// until the next update.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_state(session: *mut SkSession) -> *const c_char {
    match unsafe { session.as_ref() } {
        Some(s) => s.state.as_ptr(),
        None => c"".as_ptr(),
    }
}

/// Which XInput pad (0-3) `sk_update` last read, or -1 if none is connected.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sk_controller(session: *mut SkSession) -> c_int {
    unsafe { session.as_ref() }.map_or(-1, |s| s.controller)
}

/// Why the last failing call on this thread failed.
#[unsafe(no_mangle)]
pub extern "C" fn sk_last_error() -> *const c_char {
    LAST_ERROR.with(|last| last.borrow().as_ptr())
}
