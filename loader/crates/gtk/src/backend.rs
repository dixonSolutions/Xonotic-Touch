// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the UI comes from: the engine as UI backend (`-uibridge stdio`,
//! docs/UI_BRIDGE.md 9.2), or the fixtures of tests/uibridge/fixtures played
//! in-process (development and screenshots), or nothing (the offline status:
//! no game data or no engine).
//!
//! In fixture mode the player answers like the engine does for windows,
//! values and images; navigation between screens is not something fixtures
//! can do, so action ids that open a screen load the fixture showing it
//! (Home -> Multiplayer -> a server...), and back returns to the previous one.

use crate::app::{self, App};
use godnotic_core::bridge::fixture::{pk3, Fixture, Player};
use godnotic_core::bridge::Props;
use gtk::glib;
use serde_json::{Map, Value};
use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::rc::Rc;

pub enum Backend {
    Fixture(Box<FixtureBackend>),
    /// The engine as UI backend (`-uibridge stdio`), wired when work/uibridge lands.
    #[allow(dead_code)]
    Engine(EngineBackend),
    Offline(String),
}

pub struct FixtureBackend {
    /// Built-in screens (data/builtin): what the app shows before the bridge is live.
    pub builtin: bool,
    pub dir: PathBuf,
    pub player: Player,
    pub current: String,
    pub history: Vec<String>,
    pub incoming: VecDeque<Map<String, Value>>,
}

pub struct EngineBackend {
    pub proc: godnotic_core::session::Backend,
}

impl Backend {
    pub fn send(&mut self, line: &str) {
        match self {
            Backend::Fixture(f) => {
                if let Some(m) = serde_json::from_str::<Value>(line).ok().and_then(|v| v.as_object().cloned()) {
                    let replies = f.player.handle(&m);
                    f.incoming.extend(replies);
                }
            }
            Backend::Engine(e) => {
                if e.proc.send(line).is_err() {
                    eprintln!("godnotic-loader: the engine backend stopped reading");
                }
            }
            Backend::Offline(_) => {}
        }
    }

    pub fn take(&mut self) -> Vec<Map<String, Value>> {
        match self {
            Backend::Fixture(f) => f.incoming.drain(..).collect(),
            Backend::Engine(e) => e.proc.messages.try_iter().collect(),
            Backend::Offline(_) => vec![],
        }
    }
}

/// The fixture that shows the screen an action opens.
fn route(action: &str) -> Option<&'static str> {
    Some(match action {
        "sp" | "map.choose" => "singleplayer.json",
        "campaign" => "campaign.json",
        "host.popup" => "lan-popup.json",
        "mp" => "servers.json",
        "settings" => "settings.json",
        "credits" => "credits.json",
        "create" => "singleplayer-create-lan.json",
        "settings.video" => "settings-video.json",
        "settings.mods" => "settings-mods.json",
        "modpage.radar.main" | "mod.open" => "mod-page.json",
        "map.preview" => "map-preview.json",
        "server.join" | "join" | "join.address" => "password-dialog.json",
        _ => return None,
    })
}

/// The app's own screens, used while the engine's UI bridge is not live.
pub fn builtin_text(name: &str) -> Option<&'static str> {
    Some(match name {
        "home.json" => include_str!("../data/builtin/home.json"),
        "mp.json" => include_str!("../data/builtin/mp.json"),
        "credits.json" => include_str!("../data/builtin/credits.json"),
        _ => return None,
    })
}

pub fn load_builtin(name: &str) -> Result<Player, String> {
    let text = builtin_text(name).ok_or_else(|| format!("no built-in screen {name}"))?;
    let mut p = Player::new(Fixture::parse(text)?);
    p.images = Some(image_resolver());
    Ok(p)
}

pub fn load_fixture(dir: &std::path::Path, name: &str) -> Result<Player, String> {
    let text = std::fs::read_to_string(dir.join(name)).map_err(|e| format!("{name}: {e}"))?;
    let mut p = Player::new(Fixture::parse(&text)?);
    p.images = Some(image_resolver());
    Ok(p)
}

/// Opens the fixture for an action, as if the engine had navigated.
pub fn fixture_route(app: &App, action: &str) {
    let builtin = matches!(&*app.backend.borrow(), Backend::Fixture(f) if f.builtin);
    if builtin {
        builtin_route(app, action);
        return;
    }
    let Some(next) = route(action) else { return };
    switch_fixture(app, next, true);
}

/// Built-in screens: Multiplayer and Credits are the app's; the rest opens the game
/// on the matching dialog of its own menu, so no button is dead.
fn builtin_route(app: &App, action: &str) {
    use godnotic_core::session::Action;
    let menu = |d: &str| Action::Menu { dialog: d.to_string() };
    match action {
        "mp" => switch_fixture(app, "mp.json", true),
        "credits" => switch_fixture(app, "credits.json", true),
        "play" | "sp" | "campaign" => crate::session_ui::start(app, Action::Play),
        "settings" => crate::session_ui::start(app, menu("settings")),
        "menu.servers" => crate::session_ui::start(app, menu("servers")),
        "menu.host" => crate::session_ui::start(app, menu("TouchLanHost")),
        "menu.mods" => crate::session_ui::start(app, menu("touchmods")),
        "menu.credits" => crate::session_ui::start(app, menu("Credits")),
        "join.address" => {
            let text = app.core.borrow().store.node("mp/join/address").and_then(|n| n.str("value")).unwrap_or_default();
            match godnotic_core::link::parse_join(&text) {
                Some(a) => crate::session_ui::start(app, Action::Connect { address: a, password: None }),
                None => {
                    if let Some(u) = app.ui.borrow().as_ref() {
                        u.toasts.add_toast(adw::Toast::new("Type an address (host:port) or paste a godnotic:// link"));
                    }
                }
            }
        }
        _ => {}
    }
}

fn switch_fixture(app: &App, name: &str, push: bool) {
    let ok = {
        let mut b = app.backend.borrow_mut();
        let Backend::Fixture(f) = &mut *b else { return };
        let loaded = if f.builtin { load_builtin(name) } else { load_fixture(&f.dir, name) };
        match loaded {
            Ok(mut p) => {
                if push {
                    let cur = std::mem::replace(&mut f.current, name.to_string());
                    f.history.push(cur);
                } else {
                    f.current = name.to_string();
                }
                // A different snapshot of the same app: new rev line, same session.
                let snap = p.snapshot();
                f.player = p;
                f.incoming.push_back(snap);
                true
            }
            Err(e) => {
                eprintln!("godnotic-loader: fixture {e}");
                false
            }
        }
    };
    if ok {
        run_steps(app);
        app::schedule(app);
    }
}

pub fn fixture_back(app: &App) {
    let prev = {
        let mut b = app.backend.borrow_mut();
        let Backend::Fixture(f) = &mut *b else { return };
        f.history.pop()
    };
    if let Some(p) = prev {
        switch_fixture(app, &p, false);
    }
}

pub fn fixture_dismiss(app: &App, _id: &str) {
    fixture_back(app);
}

/// Plays a sequence fixture's scripted messages at their delays.
pub fn run_steps(app: &App) {
    let next = {
        let mut b = app.backend.borrow_mut();
        let Backend::Fixture(f) = &mut *b else { return };
        let cur = f.current.clone();
        f.player.next_step().map(|s| (s, cur))
    };
    let Some(((delay, msg), cur)) = next else { return };
    let w = Rc::downgrade(app);
    glib::timeout_add_local_once(std::time::Duration::from_secs_f64(delay.max(0.0)), move || {
        let Some(a) = w.upgrade() else { return };
        let same = matches!(&*a.backend.borrow(), Backend::Fixture(f) if f.current == cur);
        if same {
            if let Backend::Fixture(f) = &mut *a.backend.borrow_mut() {
                f.incoming.push_back(msg);
            }
            app::schedule(&a);
            run_steps(&a);
        }
    });
}

/// The engine closed the connection: show the offline status (the session layer restarts it when asked).
pub fn engine_gone(_app: &App, reason: &str) {
    eprintln!("godnotic-loader: engine said bye: {reason}");
}

/// `app.context`: in a match the engine's window takes over (see docs/LAUNCHER.md, one window).
pub fn context_changed(_app: &App, _context: &str) {}

/// pk3 image refs answered from the installed game data (fixture mode only; the engine does this itself).
fn image_resolver() -> Box<godnotic_core::bridge::fixture::ImageResolver> {
    thread_local! {
        static INDEX: std::cell::RefCell<Option<HashMap<String, (PathBuf, usize)>>> = const { std::cell::RefCell::new(None) };
        static ENTRIES: std::cell::RefCell<HashMap<PathBuf, Vec<pk3::Entry>>> = std::cell::RefCell::new(HashMap::new());
    }
    Box::new(|r: &str, _w: u32, _h: u32| {
        let path = r.strip_prefix("pk3:")?.to_ascii_lowercase();
        // Loose files first (a pk3dir, or previews unpacked for development).
        for d in data_dirs() {
            for ext in ["", ".jpg", ".png", ".tga", ".webp"] {
                let f = d.join(format!("{path}{ext}"));
                if f.is_file() {
                    let mime = if ext == ".png" { "image/png" } else { "image/jpeg" };
                    return std::fs::read(&f).ok().map(|b| (b, mime.to_string()));
                }
            }
        }
        INDEX.with(|idx| {
            let mut idx = idx.borrow_mut();
            if idx.is_none() {
                let mut m = HashMap::new();
                for d in data_dirs() {
                    let Ok(rd) = std::fs::read_dir(&d) else { continue };
                    let mut names: Vec<PathBuf> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
                    names.sort();
                    for p in names {
                        let s = p.to_string_lossy().to_lowercase();
                        if s.ends_with(".pk3") {
                            let es = pk3::entries(&p);
                            for (i, e) in es.iter().enumerate() {
                                m.insert(e.name.to_ascii_lowercase(), (p.clone(), i));
                            }
                            ENTRIES.with(|en| en.borrow_mut().insert(p.clone(), es));
                        }
                    }
                }
                *idx = Some(m);
            }
            let m = idx.as_ref()?;
            for ext in ["", ".jpg", ".png", ".tga", ".webp"] {
                let key = format!("{path}{ext}");
                if let Some((pk, i)) = m.get(&key) {
                    let bytes = ENTRIES.with(|en| en.borrow().get(pk).and_then(|es| pk3::read(pk, &es[*i])))?;
                    let mime = if key.ends_with(".png") { "image/png" } else if key.ends_with(".tga") { "image/x-tga" } else { "image/jpeg" };
                    return Some((bytes, mime.to_string()));
                }
            }
            None
        })
    })
}

/// Where fixture mode looks for pk3s: GODNOTIC_DATA_DIRS (colon-separated), else the usual data folders.
/// Set but empty counts as unset: loader/scripts/shot.sh used to pass it empty, which read nothing.
fn data_dirs() -> Vec<PathBuf> {
    if let Ok(v) = std::env::var("GODNOTIC_DATA_DIRS") {
        let dirs: Vec<PathBuf> = v.split(':').filter(|s| !s.is_empty()).map(PathBuf::from).collect();
        if !dirs.is_empty() {
            return dirs;
        }
    }
    let env: HashMap<String, String> = std::env::vars().collect();
    let p = godnotic_core::paths::GamePaths::resolve(&env, None, &|f| std::path::Path::new(f).exists(), "");
    p.data_dirs().into_iter().map(PathBuf::from).collect()
}

/// Any message type the fixture's action ids could carry.
#[allow(dead_code)]
pub fn action_id(m: &Map<String, Value>) -> Option<String> {
    m.str_of("action")
}
