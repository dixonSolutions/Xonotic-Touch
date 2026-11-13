// SPDX-License-Identifier: GPL-3.0-or-later
//! The Godnotic loader's core: everything that is not a UI toolkit.
//!
//! - [`bridge`]: the UI-bridge client. The engine's QuakeC menu describes each
//!   screen once; this parses snapshots and diffs into a node tree with stable
//!   keys, tracks list windows and image requests, and produces events.
//! - [`render`]: the operations a platform UI implements (GTK on Linux, the
//!   Kotlin views on Android), so both are one interface.
//! - [`session`]: starting the engine, the secret fd, how a session ended.
//! - [`fetch`]: the resumable, CRC-checked pack downloader (same files on disk
//!   as the Kotlin launcher and the Android app).
//! - [`config`], [`paths`], [`platform`], [`link`]: settings file, folders,
//!   what this device is, `godnotic://` links.
#![forbid(unsafe_op_in_unsafe_fn)]

pub mod bridge;
pub mod config;
pub mod fetch;
pub mod link;
pub mod paths;
pub mod platform;
pub mod render;
pub mod session;
