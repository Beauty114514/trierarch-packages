//! A generic Unix PTY host for Trierarch.
//!
//! This crate is intentionally below all runtime and UI concepts. A caller
//! supplies a [`LaunchSpec`]; this crate owns the PTY and reports byte output
//! and process exit through [`SessionEvents`].

mod chroot;
mod droidspaces;
mod privileged;
mod proot;
mod pty;
pub mod rootfs;
mod session;
mod virgl;

#[cfg(target_os = "android")]
mod jni_bridge;

pub use chroot::ChrootSpec;
pub use droidspaces::DroidspacesSpec;
pub use proot::ProotSpec;
pub use session::{LaunchSpec, SessionEvents, SessionId, SessionManager, SessionResult};
