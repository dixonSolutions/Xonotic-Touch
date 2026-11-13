// SPDX-License-Identifier: GPL-3.0-or-later
//! Godnotic loader on Android (stub; the Android agent fills it on work/androidui).
//!
//! The same core runs as a native library: the Kotlin UI implements the
//! renderer operations (crates/core `render::Renderer`) and the core talks to
//! the engine through the in-process C API of docs/UI_BRIDGE.md section 10
//! (`TouchUIB_Open/Post/Take`). The intended JNI surface:
//!
//! ```text
//! Java_..._Loader_nativeStart(env, this, renderer: Object) -> long   // handle; renderer gets callbacks
//! Java_..._Loader_nativeEvent(env, this, handle: long, json: String)  // ev/window/image from the UI
//! Java_..._Loader_nativePump(env, this, handle: long)                 // on the UI thread after a wake-up
//! Java_..._Loader_nativeStop(env, this, handle: long)
//! ```
//!
//! HTTP for the pack downloader is supplied by Kotlin through `fetch::Http`
//! (OkHttp/HttpURLConnection), so no TLS stack is compiled for Android here.

pub use godnotic_core as core;

/// The protocol this build speaks, for the Kotlin side to check.
pub fn protocol() -> u32 {
    core::bridge::PROTOCOL
}
