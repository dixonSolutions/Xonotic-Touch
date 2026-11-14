// SPDX-License-Identifier: GPL-3.0-or-later
//! Godnotic loader on Android: the core reads the engine's UI bridge and
//! drives the Kotlin views through their `UiRenderer` seam
//! (android/.../fe/UiRenderer.kt, docs/ANDROID.md "Rust core").
//!
//! The engine's messages reach Kotlin through the front-end bridge
//! (libnativeui's queue, topic "ui"); Kotlin hands each one to the core and
//! applies the operations it returns on the UI thread, then sends the
//! returned messages to the engine (`fe ui <json>`). Every call is a string
//! in and a string out, made on the UI thread: nothing in Rust holds a Java
//! reference or calls back into Java, so there is no thread or reference
//! bookkeeping across the boundary.
//!
//! ```text
//! RustCore.nativeStart() -> long                         a Driver
//! RustCore.nativeHello(h, capsJson) -> String            the hello message for the engine
//! RustCore.nativeFeed(h, engineJson) -> String           {"ops":[...],"send":[...],"state":{...}}
//! RustCore.nativeEvent(h, ev, bodyJson) -> String        a message for the engine
//! RustCore.nativeWindow(h, source, offset, limit, sort, desc, filter) -> String
//! RustCore.nativeImage(h, ref, w, h) -> String
//! RustCore.nativeSync(h) -> String
//! RustCore.nativeIgnoreRows(h, on)
//! RustCore.nativeStop(h)
//! RustCore.nativeProtocol() -> int
//! ```
//!
//! HTTP for the pack downloader stays in Kotlin (`fetch::Http`), so no TLS
//! stack is compiled for Android here.

pub use godnotic_core as core;

pub mod driver;

/// The protocol this build speaks, for the Kotlin side to check.
pub fn protocol() -> u32 {
    core::bridge::PROTOCOL
}

#[cfg(feature = "jni")]
mod jni_api {
    use super::driver::Driver;
    use jni::objects::{JClass, JString};
    use jni::sys::{jboolean, jint, jlong, jstring};
    use jni::JNIEnv;
    use serde_json::{Map, Value};

    fn text(env: &mut JNIEnv, s: &JString) -> String {
        if s.is_null() {
            return String::new();
        }
        env.get_string(s).map(|v| v.into()).unwrap_or_default()
    }

    fn out(env: &mut JNIEnv, s: &str) -> jstring {
        env.new_string(s)
            .map(|j| j.into_raw())
            .unwrap_or(std::ptr::null_mut())
    }

    /// The Driver behind a handle from nativeStart; None for 0.
    fn driver<'a>(h: jlong) -> Option<&'a mut Driver> {
        if h == 0 {
            None
        } else {
            // SAFETY: h came from Box::into_raw in nativeStart and is freed only by nativeStop;
            // Kotlin calls on one thread (the UI thread) and never after stop.
            Some(unsafe { &mut *(h as *mut Driver) })
        }
    }

    /// A panic must not unwind into the JVM: it becomes an empty answer (Kotlin falls back).
    fn guard<T>(f: impl FnOnce() -> T, fallback: T) -> T {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeProtocol(
        _env: JNIEnv,
        _c: JClass,
    ) -> jint {
        super::protocol() as jint
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeStart(
        _env: JNIEnv,
        _c: JClass,
    ) -> jlong {
        Box::into_raw(Box::new(Driver::new())) as jlong
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeStop(
        _env: JNIEnv,
        _c: JClass,
        h: jlong,
    ) {
        if h != 0 {
            // SAFETY: see driver(); this is the one place the box is freed.
            drop(unsafe { Box::from_raw(h as *mut Driver) });
        }
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeIgnoreRows(
        _env: JNIEnv,
        _c: JClass,
        h: jlong,
        on: jboolean,
    ) {
        if let Some(d) = driver(h) {
            d.ignore_rows = on != 0;
        }
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeHello<
        'l,
    >(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
        caps: JString<'l>,
    ) -> jstring {
        let caps = text(&mut env, &caps);
        let r = guard(
            || {
                driver(h)
                    .map(|d| d.hello(serde_json::from_str(&caps).unwrap_or(Value::Null)))
                    .unwrap_or_default()
            },
            String::new(),
        );
        out(&mut env, &r)
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeSync<'l>(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
    ) -> jstring {
        let r = guard(
            || driver(h).map(|d| d.sync()).unwrap_or_default(),
            String::new(),
        );
        out(&mut env, &r)
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeFeed<'l>(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
        msg: JString<'l>,
    ) -> jstring {
        let msg = text(&mut env, &msg);
        let r = guard(
            || {
                driver(h)
                    .map(|d| {
                        let b = d.feed(&msg);
                        b.to_json(d)
                    })
                    .unwrap_or_default()
            },
            String::new(),
        );
        out(&mut env, &r)
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeEvent<
        'l,
    >(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
        ev: JString<'l>,
        body: JString<'l>,
    ) -> jstring {
        let ev = text(&mut env, &ev);
        let body = text(&mut env, &body);
        let r = guard(
            || {
                let m = match serde_json::from_str::<Value>(&body) {
                    Ok(Value::Object(m)) => m,
                    _ => Map::new(),
                };
                driver(h).map(|d| d.event(&ev, &m)).unwrap_or_default()
            },
            String::new(),
        );
        out(&mut env, &r)
    }

    #[no_mangle]
    #[allow(clippy::too_many_arguments)]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeWindow<
        'l,
    >(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
        source: JString<'l>,
        offset: jint,
        limit: jint,
        sort: JString<'l>,
        desc: jboolean,
        filter: JString<'l>,
    ) -> jstring {
        let source = text(&mut env, &source);
        let sort = text(&mut env, &sort);
        let filter = text(&mut env, &filter);
        let r = guard(
            || {
                driver(h)
                    .map(|d| {
                        d.window(
                            &source,
                            offset.max(0) as usize,
                            limit.max(0) as usize,
                            &sort,
                            desc != 0,
                            &filter,
                        )
                    })
                    .unwrap_or_default()
            },
            String::new(),
        );
        out(&mut env, &r)
    }

    #[no_mangle]
    pub extern "system" fn Java_io_github_dixonsolutions_godnotic_fe_RustCore_nativeImage<
        'l,
    >(
        mut env: JNIEnv<'l>,
        _c: JClass<'l>,
        h: jlong,
        r: JString<'l>,
        w: jint,
        hh: jint,
    ) -> jstring {
        let r = text(&mut env, &r);
        let s = guard(
            || {
                driver(h)
                    .map(|d| d.image(&r, w.max(0) as u32, hh.max(0) as u32))
                    .unwrap_or_default()
            },
            String::new(),
        );
        out(&mut env, &s)
    }
}
