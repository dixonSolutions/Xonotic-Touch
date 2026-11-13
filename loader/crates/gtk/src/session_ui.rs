// SPDX-License-Identifier: GPL-3.0-or-later
//! Starting the game from the app and coming back when the match ends.
//!
//! Until the engine's UI-bridge backend mode lands (work/uibridge), the game
//! is started through packaging/start.sh, in native front end mode for a join
//! (engine/darkplaces/touch_nativefe.c). The app window hides while the game
//! runs (one window on screen), the engine quits by itself if the app goes
//! (GODNOTIC_FE_WATCH_PID), and the app reads how the session ended: a kick,
//! a refused join (asks for the password again), a crash (with the log path).

use crate::app::App;
use adw::prelude::*;
use godnotic_core::config::Config;
use godnotic_core::link::ServerAddress;
use godnotic_core::paths::GamePaths;
use godnotic_core::session::{self, Action, Outcome};
use gtk::{gio, glib};
use std::collections::HashMap;
use std::path::Path;
use std::rc::Rc;

fn paths() -> (GamePaths, Config) {
    let env: HashMap<String, String> = std::env::vars().collect();
    let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_string_lossy().to_string()));
    let cfg = std::fs::read_to_string(GamePaths::config_file(&env)).map(|t| Config::parse(&t)).unwrap_or_default();
    let p = GamePaths::resolve(&env, exe.as_deref(), &|f| Path::new(f).exists(), cfg.get(godnotic_core::config::keys::USER_BASE).unwrap_or(""));
    (p, cfg)
}

fn toast(app: &App, text: &str) {
    if let Some(u) = app.ui.borrow().as_ref() {
        u.toasts.add_toast(adw::Toast::new(text));
    }
}

/// Starts a session; the window hides until it ends.
pub fn start(app: &App, action: Action) {
    let (p, cfg) = paths();
    let Some(start) = p.start_script.clone() else {
        toast(app, "The game was not found next to the app");
        return;
    };
    // No game without its data: the storage button downloads it first.
    let ready = Path::new(&p.user_data()).join(godnotic_core::fetch::datasets::READY_MARKER).exists()
        || p.data_dirs().iter().any(|d| std::fs::read_dir(d).into_iter().flatten().flatten().any(|e| {
            let n = e.file_name().to_string_lossy().to_string();
            n.starts_with("xonotic-") && n.ends_with("-data.pk3")
        }));
    if !ready && std::env::var_os("GODNOTIC_SKIP_DATA_CHECK").is_none() {
        toast(app, "Download the game data first: the storage button at the top right");
        return;
    }
    let _ = std::fs::create_dir_all(&p.user_base);
    let result = format!("{}/session-result.txt", p.user_base);
    let log = format!("{}/last-session.log", p.user_base);
    let _ = std::fs::remove_file(&result);
    let mut plan = session::plan(&action, &cfg, &start, &result);
    plan.env.insert("GODNOTIC_FE_WATCH_PID".into(), std::process::id().to_string());
    let child = match session::spawn(&plan, &log) {
        Ok(c) => c,
        Err(e) => {
            toast(app, &format!("Could not start the game: {e}"));
            return;
        }
    };
    let Some(win) = app.ui.borrow().as_ref().map(|u| u.win.clone()) else { return };
    let gapp = win.application();
    let hold = std::cell::RefCell::new(gapp.as_ref().map(|a| a.hold()));
    win.set_visible(false);
    println!("session started: window hidden");
    let pid = glib::Pid(child.id() as i32);
    std::mem::forget(child); // reaped by the child watch below, not by std
    let w = Rc::downgrade(app);
    glib::child_watch_add_local(pid, move |_, status| {
        hold.borrow_mut().take();
        let Some(app) = w.upgrade() else { return };
        let (code, signalled) = if libc::WIFSIGNALED(status) { (128 + libc::WTERMSIG(status), true) } else { (libc::WEXITSTATUS(status), false) };
        let text = std::fs::read_to_string(&result).ok();
        let out = Outcome::parse(text.as_deref(), code, signalled, &log);
        println!("session ended: kind={} exit={} reason={}", out.kind, out.exit_code, out.reason);
        if let Some(u) = app.ui.borrow().as_ref() {
            u.win.present();
        }
        if let Some(m) = out.message() {
            toast(&app, &m);
        }
        if out.wants_password() {
            if let Action::Connect { address, .. } = &action {
                ask_password(&app, address.clone(), out.kind == "password_wrong");
            }
        }
    });
}

/// "Join <address>?" for a godnotic:// link; the password is asked only when the server wants one.
pub fn confirm_join(app: &App, address: ServerAddress) {
    let Some(win) = app.ui.borrow().as_ref().map(|u| u.win.clone()) else { return };
    let d = adw::AlertDialog::new(Some("Join this game?"), Some(&address.to_string()));
    d.add_response("cancel", "Cancel");
    d.add_response("join", "Join");
    d.set_response_appearance("join", adw::ResponseAppearance::Suggested);
    d.set_default_response(Some("join"));
    let w = Rc::downgrade(app);
    d.choose(Some(&win), gio::Cancellable::NONE, move |r| {
        if let (Some(a), "join") = (w.upgrade(), r.as_str()) {
            start(&a, Action::Connect { address, password: None });
        }
    });
}

fn ask_password(app: &App, address: ServerAddress, wrong: bool) {
    let Some(win) = app.ui.borrow().as_ref().map(|u| u.win.clone()) else { return };
    let d = adw::AlertDialog::new(Some(if wrong { "Wrong password" } else { "This game needs a password" }), Some(&address.to_string()));
    let row = adw::PasswordEntryRow::builder().title("Password").build();
    row.add_prefix(&gtk::Image::from_icon_name("changes-prevent-symbolic"));
    let g = adw::PreferencesGroup::new();
    g.add(&row);
    d.set_extra_child(Some(&g));
    d.add_response("cancel", "Cancel");
    d.add_response("join", "Join");
    d.set_response_appearance("join", adw::ResponseAppearance::Suggested);
    d.set_default_response(Some("join"));
    let w = Rc::downgrade(app);
    let r2 = row.clone();
    d.choose(Some(&win), gio::Cancellable::NONE, move |r| {
        let pw = r2.text().to_string();
        r2.set_text("");
        if let (Some(a), "join") = (w.upgrade(), r.as_str()) {
            start(&a, Action::Connect { address, password: Some(pw) });
        }
    });
}
