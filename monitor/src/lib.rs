//! DeskHUD monitoring service: system stats and coding-agent activity for the DeskHUD Wi-Fi display.
//! See `docs/PROTOCOL.md` at the repository root for the wire protocol.

pub mod agents;
pub mod config;
pub mod daemon;
pub mod demo;
pub mod discover;
pub mod http;
pub mod install;
pub mod link;
pub mod ota;
pub mod stats;
pub mod sysview;
