//! PhysicalPlayer startup, PostInput, state selection and state publication.
//! Native control decisions use original TU3 functions and stock collections;
//! host ownership replaces the original player component pointers.
mod post_input;
mod pre_state;
mod publication;
mod registry;
mod selection;
mod transition;
mod wipeout_output;
use super::{GamePhysics, skater::SkaterRuntime};
use skate_core::{
    physics::filtered_state::{FilteredState, FilteredStateOutput},
    player::{
        lifecycle::PhysicalPlayerStateLifecycle,
        selector::{StateSelector, conditions::TwoStageThresholds},
        state::PhysicalStateId,
    },
};
use skate_data::collections::Collections;

pub(crate) struct PlayerState {
    pub registry: registry::StateRegistry,
    pub lifecycle: PhysicalPlayerStateLifecycle,
    pub selector: StateSelector,
    pub requested_state: PhysicalStateId,
    pub filtered: FilteredState,
    pub filtered_output: Option<FilteredStateOutput>,
    pub ground_output: Option<skate_core::riding::grounded::state::output::PhysicsGroundOutput>,
    pub post: post_input::PostInputState,
    pub state_flags: [bool; 36],
    pub state_count: u32,
    pub update_count: u32,
    normal_off_ground: TwoStageThresholds,
    skitching_off_ground: TwoStageThresholds,
    animated_board_threshold: f32,
    initialized: bool,
}
impl PlayerState {
    pub fn load(data: &Collections, _mode: &str) -> Result<Self, String> {
        let thresholds = |class| -> Result<TwoStageThresholds, String> {
            Ok(TwoStageThresholds {
                field_856_primary: data.float(class, "default", "NaturalAirMaxDist")?,
                field_856_secondary: data.float(class, "default", "NaturalAirMinDist")?,
                field_7692: data.float(class, "default", "NaturalAirTime")?,
            })
        };
        Ok(Self {
            registry: registry::StateRegistry::new(),
            //82DB3008 selects the owned Sleeping object before SetPhysicsState100.
            lifecycle: PhysicalPlayerStateLifecycle::new(PhysicalStateId::Sleeping),
            selector: StateSelector::default(),
            requested_state: PhysicalStateId::Sleeping,
            filtered: FilteredState::default(),
            filtered_output: None,
            ground_output: None,
            post: post_input::PostInputState::new(),
            state_flags: [false; 36],
            state_count: 0,
            update_count: 0,
            //82D8BD50 reads Globals260; its layout392/396/400 is physics_airstates.
            normal_off_ground: thresholds("physics_airstates")?,
            skitching_off_ground: thresholds("physics_state_skitching")?,
            //Skeleton+24 physics_animation, original82BDC6C8 layout800.
            animated_board_threshold: data.float(
                "physics_animation",
                "default",
                "MaxDeckZAxisYForAnimatedDeck",
            )?,
            initialized: false,
        })
    }
    pub fn current(&self) -> PhysicalStateId {
        self.lifecycle.active().state
    }
    ///ResetSystems82DB92D0 resets the selector and physical-output filter;
    ///the coordinator owns board/skeleton resets and the following Ground entry.
    pub fn reset_for_teleport(&mut self) {
        self.filtered.reset();
        self.filtered_output = None;
        self.ground_output = None;
        //ResetSystems82DB92D0 clears20..44, sets48=10 and52=0.
        //Current pointer16 and flags56/57 survive until Calculate writes them.
        self.selector.nonspecific_collision_free_frames = 0;
        self.selector.nonspecific_collision_frames = 0;
        self.selector.something_colliding_frames = 0;
        self.selector.two_wheel_counter = 0;
        self.selector.three_wheel_counter = 0;
        self.selector.post_grind_jump_counter = 0;
        self.selector.air_frames = 0;
        self.selector.teleport_countdown = 10;
        self.selector.skitch_exit_countdown = 0;
        //TrajectorySelector::Reset82D67228 clears9658, preserving9657.
        self.post.trajectory_available = false;
    }
}
///Call once before first input; startup constructs the toolkit from live board/reset inputs.
pub(crate) fn initialize(
    physics: &mut GamePhysics,
    skater: &mut SkaterRuntime,
) -> Result<(), String> {
    if skater.player_state.initialized {
        return Ok(());
    }
    transition::set(physics, skater, PhysicalStateId::PhysicsGround)?;
    skater.player_state.initialized = true;
    Ok(())
}
///Runs after ProcessInput, before the chosen state's pre-update/force phase.
pub(crate) fn post_input_and_select(
    physics: &mut GamePhysics,
    skater: &mut SkaterRuntime,
) -> Result<(), String> {
    complete_post_input(physics, skater)?;
    let processed = skater.player_input.processed_snapshot(physics.ticks);
    selection::advance(physics, skater, processed)
}

/// Completes the mandatory PostInput half of the current ProcessInput pass.
/// Host-owned transitions may need to retain their selected state, but they
/// must still drain the query work produced by ProcessInput this tick.
pub(crate) fn complete_post_input(
    physics: &mut GamePhysics,
    skater: &mut SkaterRuntime,
) -> Result<(), String> {
    post_input::advance(physics, skater)
}
///The reset/board/skeleton publications precede this selected-state FillPhysOut.
pub(crate) fn publish(physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
    publication::publish(physics, skater)
}
///Reset continuation uses the same native SetPhysicsState path and actual entry.
pub(crate) fn enter_after_teleport(
    physics: &mut GamePhysics,
    skater: &mut SkaterRuntime,
) -> Result<(), String> {
    let target = if skater.teleport_state.take_manual_on_board() == Some(false) {
        PhysicalStateId::BipedGround
    } else { PhysicalStateId::PhysicsGround };
    transition::set(physics, skater, target)
}

///Original82DB6050 prefix; coordinator calls the selected state pre-update next.
pub(crate) fn pre_state(
    physics: &mut GamePhysics,
    skater: &mut SkaterRuntime,
) -> Result<(), String> {
    pre_state::advance(physics, skater)
}

/// Custom traversal bypasses native queries; discard old work before re-entry.
pub(crate) fn resume_after_climb(physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
    super::biped_ground::exit(skater);
    skater.offboard_air_selector.reset();
    skater.collision_extra_errors = [[0.; 4]; 2];
    skater.animated_skeleton.motion.velocity_world = [0.; 4];
    skater.player_input.processed.vectors_544_560_592_608[3] = [0; 4];
    if skater.player_state.current() == PhysicalStateId::BipedGround {
        super::biped_ground::enter(physics, skater)?;
    } else {
        transition::set(physics, skater, PhysicalStateId::BipedGround)?;
    }
    super::foot_ik_queries::query(&physics.world, &skater.skeleton)?
        .publish(&mut skater.player_input.player);
    let frame = skater.biped_ground.ground.frame_80;
    skater.offboard_contact.submit(
        skate_core::player::offboard::contact_toolkit::Input {
            position: frame[3], right: frame[0], up: frame[1], forward: frame[2],
            animation_right: frame[0], animation_up: frame[1], velocity: [0.; 4],
        },
        skater.player_input.processed.actor_query_2952 as i32,
        &super::offboard::contact_toolkit::StaticScene::new(&physics.world)?,
    )?;

    Ok(())
}

/// Vehicle ownership has ended; reset first, then enter the native ragdoll and seed momentum.
pub(crate) fn apply_vehicle_ejection(physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<bool, String> {
    let Some((velocity, angular)) = skater.teleport_state.take_vehicle_ejection() else { return Ok(false); };
    transition::set(physics, skater, PhysicalStateId::WipeoutGround)?;
    let velocity=skate_core::math::Vector3::new(velocity[0],velocity[1],velocity[2]);
    let angular=skate_core::math::Vector3::new(angular[0],angular[1],angular[2]);
    throw(skater.skeleton.bodies_mut(), velocity, angular);
    // SanAnskateas addition: the board may be given its own velocity (see
    // Session::knock); it rolls about its length rather than tumbling with him.
    let board_velocity = skater.wipeout_state.board_throw.take()
        .map_or(velocity, |v| skate_core::math::Vector3::new(v[0], v[1], v[2]));
    let length = physics.board.part_transforms()[skate_core::physics::board::BodyId::Deck.index()].basis.columns[2];
    let rate = (angular.x * angular.x + angular.y * angular.y + angular.z * angular.z).sqrt() * 0.7;
    let board_spin = skate_core::math::Vector3::new(length[0] * rate, length[1] * rate, length[2] * rate);
    throw(physics.board.bodies_mut(), board_velocity, board_spin);
    skater.animated_skeleton.motion.velocity_world=[velocity.x,velocity.y,velocity.z,0.];
    skater.player_input.processed.vectors_544_560_592_608[3]=[velocity.x,velocity.y,velocity.z,0.].map(f32::to_bits);
    skater.wipeout_state.state.velocity=[velocity.x,velocity.y,velocity.z,0.];
    skater.wipeout_state.ejected_frames = 3; // SanAnskateas addition, see update.rs
    Ok(true)
}

/// SanAnskateas addition: a host car's hit (see `Session::knock`), once per
/// tick after state selection. The vehicle-contact request it filed has made
/// an ordinary bail: the ragdoll, which starts from his pose of the moment
/// as in any bail, and the board take the hit's motion. No bail two ticks on
/// (he's on foot): Skate's vehicle ejection throws him instead.
pub(crate) fn advance_knock(physics: &mut GamePhysics, skater: &mut SkaterRuntime) {
    use skate_core::math::Vector3;
    let Some(mut knock) = skater.wipeout_state.knock else { return };
    let deck = physics.board.bodies()[skate_core::physics::board::BodyId::Deck.index()].rates.linear_velocity;
    let add = |v: [f32; 3]| Vector3::new(deck.x + v[0], deck.y + v[1], deck.z + v[2]);
    let velocity = add(knock.push);
    const TUMBLE: f32 = 4.0; // rad/s at a full-speed hit
    let push = bevy::math::Vec3::from_array(knock.push);
    let spin = knock.spin.map(bevy::math::Vec3::from_array).unwrap_or_else(|| {
        bevy::math::Vec3::Y.cross(push.normalize_or_zero()) * TUMBLE * (push.length() / 10.0).min(1.0)
    });
    let angular = Vector3::new(spin.x, spin.y, spin.z);
    skater.wipeout_state.free_tumble = knock.spin.is_some();
    if skater.player_state.current() == PhysicalStateId::WipeoutGround {
        skater.wipeout_state.knock = None;
        for body in skater.skeleton.bodies_mut() {
            body.rates.position.y += knock.lift;
        }
        throw(skater.skeleton.bodies_mut(), velocity, angular);
        let board_velocity = knock.board.map_or(velocity, add);
        let length = physics.board.part_transforms()[skate_core::physics::board::BodyId::Deck.index()].basis.columns[2];
        let rate = spin.length() * 0.7;
        throw(physics.board.bodies_mut(), board_velocity, Vector3::new(length[0] * rate, length[1] * rate, length[2] * rate));
        let v = [velocity.x, velocity.y, velocity.z, 0.];
        skater.animated_skeleton.motion.velocity_world = v;
        skater.player_input.processed.vectors_544_560_592_608[3] = v.map(f32::to_bits);
        skater.wipeout_state.state.velocity = v;
        skater.wipeout_state.ejected_frames = 3; // see update.rs
    } else if knock.ticks >= 2 {
        skater.wipeout_state.knock = None;
        skater.wipeout_state.board_throw = knock.board.map(|b| [deck.x + b[0], deck.y + b[1], deck.z + b[2]]);
        let mut transform = skater.animated_skeleton.roots.animation_to_world;
        transform[3][1] += knock.lift;
        skater.teleport_state.request_vehicle_ejection(transform, [velocity.x, velocity.y, velocity.z], spin.to_array());
    } else {
        knock.ticks += 1;
        skater.wipeout_state.knock = Some(knock);
    }
}

/// SanAnskateas addition: jointed bodies (the skater's, the board's parts)
/// thrown at `velocity` and turning as a whole at `angular`, each part moving
/// with the turn about their centre of mass. (The same velocity and spin on
/// every part turned each one in place: the joints cancelled most of the
/// skater's tumble and pushed the board's deck off faster.)
fn throw(bodies: &mut [skate_core::physics::assembly::BodySnapshot], velocity: skate_core::math::Vector3, angular: skate_core::math::Vector3) {
    let (mut centre, mut mass) = ([0f32; 3], 0f32);
    for body in bodies.iter() {
        let m = if body.inertia.inverse_mass > 0. { 1. / body.inertia.inverse_mass } else { 0. };
        let p = body.rates.position;
        centre = [centre[0] + p.x * m, centre[1] + p.y * m, centre[2] + p.z * m];
        mass += m;
    }
    if mass > 0. {
        centre = centre.map(|c| c / mass);
    }
    for body in bodies.iter_mut() {
        let p = body.rates.position;
        let r = if mass > 0. { [p.x - centre[0], p.y - centre[1], p.z - centre[2]] } else { [0.; 3] };
        body.rates.linear_velocity = skate_core::math::Vector3::new(
            velocity.x + angular.y * r[2] - angular.z * r[1],
            velocity.y + angular.z * r[0] - angular.x * r[2],
            velocity.z + angular.x * r[1] - angular.y * r[0],
        );
        body.rates.angular_velocity = angular;
    }
}
