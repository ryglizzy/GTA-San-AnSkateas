//! Original physical WipeoutGround300. Off-board recovery destinations are
//! separate states; this owner retains the native request and timing fields.
mod contact;
mod drive_settings;
mod lifecycle;
mod output;
mod prediction;
pub(crate) mod ragdoll;
mod settings;
mod skeleton;
mod update;
use skate_core::player::wipeout_state::{
    State, contact_response::ContactResponse, drives, profiles::Profile,
};
use skate_data::collections::Collections;
use std::path::Path;

pub(crate) struct WipeoutState {
    pub(crate) ragdoll: ragdoll::RagdollSetup,
    pub state: State,
    settings: settings::Settings,
    drives: drives::Settings,
    profiles: [Profile; 5],
    contact: ContactResponse,
    prediction: prediction::Prediction,
    /// SanAnskateas addition: a host vehicle threw the skater tumbling; the
    /// air control (which steadies the body) waits until he first touches
    /// something, so the tumble carries.
    pub(crate) free_tumble: bool,
    /// SanAnskateas addition: updates left in which a vehicle ejection's
    /// speed isn't taken for a glitch launch (see update.rs).
    pub(crate) ejected_frames: u8,
    /// SanAnskateas addition: the board's own velocity for the next vehicle
    /// ejection (None: the skater's).
    pub(crate) board_throw: Option<[f32; 3]>,
    /// SanAnskateas addition: a host car hit on its way to becoming an
    /// ordinary bail (see `Session::knock`).
    pub(crate) knock: Option<PendingKnock>,
}

/// SanAnskateas addition: a host car's hit, in Skate space.
#[derive(Clone, Copy, Debug)]
pub(crate) struct PendingKnock {
    pub push: [f32; 3],          // added to the skater's motion, m/s
    pub spin: Option<[f32; 3]>,  // rad/s, tumbling freely until he touches something
    pub lift: f32,               // metres up (onto a car's hood)
    pub board: Option<[f32; 3]>, // added to the deck's motion instead of `push`
    pub ticks: u8,               // since the bail was requested
}
impl WipeoutState {
    pub fn load(data: &Collections, assets: &Path, primary_bank_sha: &str) -> Result<Self, String> {
        //Ctor82D3B1EC..B25C full64 handle hashes, verified against the stock keys.
        let names = [
            "free_fall",
            "cannon_ball",
            "judo_kick",
            "swan_dive",
            "torpedo",
        ];
        let profiles = names.map(|name| settings::load_profile(data, name));
        let [a, b, c, d, e] = profiles;
        Ok(Self {
            ragdoll: ragdoll::RagdollSetup::load(data, assets, primary_bank_sha)?,
            state: State::default(),
            settings: settings::Settings::load(data)?,
            drives: drive_settings::load(data, assets, primary_bank_sha)?,
            profiles: [a?, b?, c?, d?, e?],
            contact: ContactResponse::new(),
            prediction: prediction::Prediction::new(),
            free_tumble: false,
            ejected_frames: 0,
            board_throw: None,
            knock: None,
        })
    }
}
pub(crate) use lifecycle::{enter, exit, post_physics};
pub(crate) use output::fill;
pub(crate) use update::advance;
