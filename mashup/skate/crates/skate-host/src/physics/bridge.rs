use super::{GamePhysics, PlayerControls, SkaterRuntime};
use crate::{camera::CameraRuntime, graph_runtime::StockGraphs, input::ControllerInput};
use bevy::prelude::*;
use skate_data::skate_map::{Collision, Geometry, Rail, SkateMap};
use std::path::Path;

#[derive(Clone, Copy, Default)]
pub struct Controls {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}
pub struct Session {
    physics: GamePhysics,
    skater: SkaterRuntime,
    controls: PlayerControls,
    graphs: StockGraphs,
    input: ControllerInput,
    camera: CameraRuntime,
    markers: crate::session_marker::Runtime,
}
pub struct Pose {
    pub root: Mat4,
    pub bones: Vec<Mat4>,
    pub names: Vec<String>,
    pub camera: Option<(Vec3, Mat3, f32)>,
    pub velocity: Vec3,
    pub tick: u64,
    pub state: String,
}
/// SanAnskateas addition: the score HUD's view of the last tick (see `Session::score`).
pub use crate::scoring_runtime::View as ScoreView;
/// SanAnskateas addition: what a host's skateboard sounds follow, read
/// after a tick, in Skate space (Y up, metres).
#[derive(Clone, Copy, Debug, Default)]
pub struct Feedback {
    /// PhysicalStateId: 100s riding, 200s air, 300 bail, 400s grinds,
    /// 500s off the board, 600s plants, 700s other.
    pub state: u32,
    /// Wheels touching the ground (0-4).
    pub wheels: u32,
    /// Strongest closing speed against this tick's board contacts (m/s).
    pub closing_speed: f32,
    pub board_velocity: [f32; 3],
    pub board_spin: [f32; 3],
    pub rider_velocity: [f32; 3],
    /// The last landing as Skate 3 grades it (CalcLandingQuality): 0 none
    /// yet, 1 clean (straight), 2 okay (a little off), 3 sketchy (landed
    /// sideways or spinning and had to be saved).
    pub landing_grade: u32,
    /// How hard the foot brake is on (0-1, the animation's brake attribute).
    pub brake: f32,
}
/// SanAnskateas addition: the session marker menu (held LB) as Skate 3's
/// PlayerUI shows it.
#[derive(Clone, Copy, Debug, Default)]
pub struct MarkerView {
    pub visible: bool,
    pub can_place: bool,
    pub can_return: bool,
    pub progress: f32,
}
impl Session {
    pub fn new(
        root: &Path,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
        spawn: [f32; 3],
        heading: f32,
    ) -> Result<Self, String> {
        Self::with_difficulty(root, triangles, rails, spawn, heading, "easy")
    }
    /// SanAnskateas addition: `new` with a chosen physics difficulty ("easy",
    /// "normal" or "hardcore"; Skate 3's own default is normal).
    pub fn with_difficulty(
        root: &Path,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
        spawn: [f32; 3],
        heading: f32,
        difficulty: &str,
    ) -> Result<Self, String> {
        let difficulty = crate::difficulty::Difficulty::parse(difficulty)?;
        let started = std::time::Instant::now();
        eprintln!("IW4L_SKATE_LOAD begin");
        skate_data::input_config::StockGameplayConfig::load(root).map_err(|e| e.to_string())?;
        let assets = skate_data::GameAssets::load(root).map_err(|e| e.to_string())?;
        let graphs = StockGraphs::load(root, &assets)?;
        let map = collision_map(triangles, rails, spawn, heading);
        eprintln!("IW4L_SKATE_LOAD graphs {}ms", started.elapsed().as_millis());
        let physics = GamePhysics::load_with_difficulty(root, Some(&map), difficulty)?;
        eprintln!(
            "IW4L_SKATE_LOAD physics {}ms",
            started.elapsed().as_millis()
        );
        let skater = SkaterRuntime::load(root, &graphs, &physics, difficulty.key())?;
        eprintln!("IW4L_SKATE_LOAD skater {}ms", started.elapsed().as_millis());
        Ok(Self {
            physics,
            skater,
            controls: PlayerControls::load(root)?,
            graphs,
            input: ControllerInput::default(),
            camera: CameraRuntime::load(root)?,
            markers: crate::session_marker::Runtime::load(root)?,
        })
    }
    /// A builder for collision to swap in later, usable on another thread.
    pub fn collision_builder(&self) -> CollisionBuilder {
        CollisionBuilder {
            material: self.physics.floor_material(),
        }
    }
    /// Swaps in collision built by `collision_builder`: the world the skater
    /// rides, climbs and grinds from the next tick.
    pub fn install_collision(&mut self, prepared: PreparedCollision) -> Result<(), String> {
        self.physics
            .install_world(prepared.world, std::sync::Arc::clone(&prepared.grind))?;
        self.skater.trajectory.bind_grind_world(prepared.grind);
        Ok(())
    }
    pub fn period(&self) -> f32 {
        self.physics.period().as_secs_f32()
    }
    pub fn set_aspect_ratio(&mut self, aspect_ratio: f32) {
        if aspect_ratio.is_finite() && aspect_ratio > 0. {
            self.camera.set_aspect_ratio(aspect_ratio);
        }
    }
    /// Eagerly decode immutable animation banks before a map is ready.
    pub fn preload(root: &Path) -> Result<(), String> {
        crate::skater_animation::AnimationSource::load(root).map(|_| ())
    }
    /// Reuse the complete world and animation session. The original teleport path
    /// resets physical bodies and animation state at the new MW2 position.
    pub fn activate(&mut self, spawn: [f32; 3], heading: f32) -> Result<Pose, String> {
        self.input = ControllerInput::default();
        self.markers.suspend();
        if self.physics.ticks == 0 {
            self.tick(Controls::default())?;
        }
        let mut transform = Mat4::from_rotation_translation(
            Quat::from_rotation_y(heading),
            Vec3::from_array(spawn),
        )
        .to_cols_array_2d();
        transform[3][3] = 0.;
        self.skater.travel_to(transform)?;
        for _ in 0..4 {
            self.tick(Controls::default())?;
        }
        self.input = ControllerInput::default();
        Ok(self.pose())
    }
    /// SanAnskateas addition: gives the board and the skater's body a
    /// starting velocity (Skate space, m/s), so a host character's momentum
    /// carries onto the board. Call right after `activate`; the wheels spin
    /// up through their ground contacts.
    pub fn set_velocity(&mut self, velocity: [f32; 3]) {
        let v = skate_core::math::Vector3::new(velocity[0], velocity[1], velocity[2]);
        for body in self.physics.board.bodies_mut() {
            body.rates.linear_velocity = v;
        }
        for body in self.skater.skeleton.bodies_mut() {
            body.rates.linear_velocity = v;
        }
    }
    /// SanAnskateas addition: a host vehicle hit the skater (moving host cars
    /// aren't in Skate's world). It becomes Skate's ordinary bail, from
    /// whatever he was doing: during the next tick his vehicle-contact
    /// wipeout request (7) is filed where the state checks file theirs, and
    /// once the bail begins his ragdoll and board get his motion plus
    /// `velocity` (Skate space, m/s), spinning at `spin` (rad/s; he tumbles
    /// freely until he first touches something), or by default tumbling a
    /// little about the push's sideways axis, `lift` metres higher (onto a
    /// car's hood). The board gets `board` added instead when given. If no
    /// bail follows (on foot), Skate's vehicle ejection throws him instead;
    /// that one starts the ragdoll from the standing pose, which held him
    /// stiff (arms down, legs straight) all through the flight.
    pub fn knock(&mut self, velocity: [f32; 3], spin: Option<[f32; 3]>, lift: f32, board: Option<[f32; 3]>) {
        self.skater.wipeout_state.knock = Some(super::wipeout_states::PendingKnock {
            push: velocity,
            spin,
            lift: lift.max(0.0),
            board,
            ticks: 0,
        });
    }
    pub fn collect(&mut self, frame: InputFrame, dt: f32) {
        self.input.collect(frame.samples);
        self.markers.collect_time(f64::from(dt));
    }
    /// SanAnskateas addition: changes the physics difficulty ("easy",
    /// "normal" or "hardcore") right away. Every mode's settings are loaded
    /// up front and the physics picks them by mode each tick, so the board,
    /// the trick under way and the equipment carry on.
    pub fn set_difficulty(&mut self, difficulty: &str) -> Result<(), String> {
        self.physics.set_difficulty(crate::difficulty::Difficulty::parse(difficulty)?);
        Ok(())
    }
    /// SanAnskateas addition: Skate 3's camera option, Low (the older games'
    /// "OG" camera) or High (Skate 3's default).
    pub fn set_camera_low(&mut self, low: bool) {
        self.physics.camera_type = if low { 0 } else { 1 };
    }
    /// SanAnskateas addition: Skate 3's truck tightness, 0 loosest to 1
    /// tightest (Skate 3's stock setting is 0.7). Read by the steering and
    /// speed-wobble every tick.
    pub fn set_truck_tightness(&mut self, tightness: f32) {
        self.physics.set_truck_tightness(tightness);
    }
    pub fn truck_tightness(&self) -> f32 {
        self.physics.truck_tightness()
    }
    /// SanAnskateas addition: a host stepping the engine itself (`tick`)
    /// gives the session marker's hold timer its time this way.
    pub fn collect_marker_time(&mut self, dt: f32) {
        self.markers.collect_time(f64::from(dt));
    }
    pub fn suspend_input(&mut self) {
        self.input = ControllerInput::default();
        self.markers.suspend();
    }
    pub fn advance(&mut self) -> Result<(), String> {
        self.input.publish_actions();
        self.advance_published()
    }
    fn advance_published(&mut self) -> Result<(), String> {
        let published = self.input.tick_input();
        self.markers
            .advance(&self.input, &self.physics, &mut self.skater);
        let mut actions = published.actions();
        self.controls.update_for_physics(
            &mut actions,
            &self.physics,
            &self.skater,
            &self.camera,
        )?;
        self.controls.publish_gestures(
            self.physics.animation_profile.physics_mode,
            self.skater.player_input.physical.state.state_16,
        );
        super::frame::advance(
            &mut self.physics,
            &mut self.skater,
            &mut self.controls,
            &self.graphs,
            &mut actions,
            published.controller_available(),
            &mut self.camera,
        )
    }
    /// Deterministic raw-packet entry point for playback/diagnostics.
    pub fn tick(&mut self, input: Controls) -> Result<(), String> {
        crate::input::sample(
            &mut self.input,
            skate_core::input::xbox::XboxState {
                buttons: input.buttons,
                triggers: input.triggers,
                left: input.left,
                right: input.right,
            },
        );
        self.advance_published()
    }
    pub fn pose(&self) -> Pose {
        let v = self.physics.board.bodies()[skate_core::physics::board::BodyId::Deck.index()]
            .rates
            .linear_velocity;
        Pose {
            root: crate::animation::native_matrix(
                self.skater.animated_skeleton.roots.animation_to_world,
            ),
            bones: self
                .skater
                .render_pose
                .iter()
                .map(|m| crate::animation::native_matrix(*m))
                .collect(),
            names: self.skater.animation.evaluator.frames.bone_names.clone(),
            camera: self.camera.frame.as_ref().map(|f| {
                (
                    Vec3::new(f.position[0], f.position[1], f.position[2]),
                    Mat3::from_cols_array_2d(&f.basis.columns),
                    f.field_of_view_degrees,
                )
            }),
            velocity: Vec3::new(v.x, v.y, v.z),
            tick: self.physics.ticks,
            state: format!("{:?}", self.skater.player_state.current()),
        }
    }
    /// SanAnskateas addition: the wipeout checks that fired since the last
    /// call, a bit per Skate request number (2: the board hit something at
    /// speed, 0: the body did, 17: fell off a grind...), for a bail log.
    pub fn take_wipeout_checks(&mut self) -> u64 {
        std::mem::take(&mut self.skater.wipeout.state.seen)
    }
    /// SanAnskateas addition: see `Feedback`.
    pub fn feedback(&self) -> Feedback {
        let deck = &self.physics.board.bodies()[skate_core::physics::board::BodyId::Deck.index()].rates;
        let ground = &self.physics.riding.ground;
        let rider = self.skater.skeleton.bodies().first().map_or(skate_core::math::Vector3::ZERO, |b| b.rates.linear_velocity);
        let v = |a: skate_core::math::Vector3| [a.x, a.y, a.z];
        let landing = &self.skater.landing_quality;
        // Type 0 lands straight (Skate's scoring calls it clean), 3 a little
        // off; 1 and 2 land sideways or spinning, and a big correction
        // (spin over half its scale) is a sketchy landing.
        let landing_grade = match (landing.landing_data_167, landing.landing_type_96) {
            (false, _) => 0,
            (true, 0) => 1,
            (true, 1 | 2) if landing.spin_92.abs() > 0.5 => 3,
            _ => 2,
        };
        Feedback {
            state: self.skater.player_state.current() as u32,
            wheels: u32::from(ground.wheel_contact_count),
            closing_speed: ground.maximum_closing_speed,
            board_velocity: v(deck.linear_velocity),
            board_spin: v(deck.angular_velocity),
            rider_velocity: v(rider),
            landing_grade,
            brake: self.skater.animation_input.fields.brake,
        }
    }
    /// SanAnskateas addition: the score HUD's view (Skate 3's ScoreModule).
    pub fn score(&self) -> ScoreView {
        self.skater.scoring.view()
    }
    /// SanAnskateas addition: see `MarkerView`.
    pub fn marker(&self) -> MarkerView {
        let (visible, can_place, can_return, progress) = self.markers.view();
        MarkerView { visible, can_place, can_return, progress }
    }
}

/// Collision for `Session::install_collision`, built off the simulation.
pub struct PreparedCollision {
    world: skate_core::physics::board_world::BoardWorld,
    grind: std::sync::Arc<crate::grind_world::StaticProvider>,
}

#[derive(Clone, Copy)]
pub struct CollisionBuilder {
    material: skate_core::physics::contact::RetailContactMaterial,
}

impl CollisionBuilder {
    pub fn build(
        &self,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
    ) -> Result<PreparedCollision, String> {
        let map = collision_map(triangles, rails, [0.; 3], 0.);
        Ok(PreparedCollision {
            world: crate::skate_world::collision_world(&map, self.material)?,
            grind: std::sync::Arc::new(crate::grind_world::StaticProvider::new(Some(&map))?),
        })
    }
}

/// IW4L's collision as a Skate map: one material, the triangles and rails.
fn collision_map(
    triangles: Vec<[[f32; 3]; 3]>,
    rails: Vec<Vec<[f32; 3]>>,
    spawn: [f32; 3],
    heading: f32,
) -> SkateMap {
    SkateMap {
            version: 14,
            name: "IW4L collision".into(),
            spawn,
            heading,
            environment: vec![],
            materials: vec![skate_data::skate_map::Material {
                name: "MW2".into(),
                flags: 0,
                friction: 0.8,
                restitution: 0.,
                color: [1.; 3],
                roughness: 1.,
                emissive: 0.,
                textures: [0; 5],
                indirect_strength: 1.,
                alpha_mode: 0,
                alpha_cutoff: 0.5,
                audio: 0,
                physics: 0,
                pattern: 0,
                depth_layer: None,
                retail_definition: None,
            }],
            textures: vec![],
            geometry: Geometry {
                vertices: vec![],
                indices: vec![],
                collision: triangles
                    .into_iter()
                    .map(|points| Collision {
                        points,
                        surface: 0,
                        material: 1,
                        native_edges: None,
                    })
                    .collect(),
            },
            rails: rails
                .into_iter()
                .enumerate()
                .map(|(i, p)| Rail {
                    name: format!("iw4_edge_{i}"),
                    closed: false,
                    points: p,
                    native: None,
                })
                .collect(),
            doors: vec![],
            lights: vec![],
            routes: vec![],
            extensions: vec![],
    }
}

/// The source engine's raw XInput transport. No Bevy deadzones, button remaps,
/// trigger reconstruction or rounding are inserted ahead of its native Pad.
#[derive(Default)]
pub struct ControllerTransport {
    capabilities: [crate::input::platform::CapabilityCache; 4],
}
pub struct InputFrame {
    samples: [Result<crate::input::platform::DevicePacket, crate::input::platform::DeviceError>; 4],
}
impl ControllerTransport {
    pub fn poll(&mut self) -> InputFrame {
        InputFrame {
            samples: std::array::from_fn(|i| {
                crate::input::platform::poll_cached(i, &mut self.capabilities[i])
            }),
        }
    }
}
impl InputFrame {
    pub fn neutral() -> Self {
        Self {
            samples: std::array::from_fn(|_|Err(crate::input::platform::DeviceError::Disconnected)),
        }
    }
    pub fn controller(&self) -> Option<usize> {
        self.samples.iter().position(Result::is_ok)
    }
    pub fn buttons(&self) -> u16 {
        self.samples
            .iter()
            .find_map(|s| s.as_ref().ok().map(|s| s.state.buttons))
            .unwrap_or(0)
    }
}
