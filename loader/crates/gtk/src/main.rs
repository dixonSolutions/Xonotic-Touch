// SPDX-License-Identifier: GPL-3.0-or-later
//! godnotic-loader: the Godnotic app on Linux. One window, up at once with the
//! native skeleton (banner and placeholders); then the engine's UI bridge (or
//! a fixture) fills it with the described screens, rendered with libadwaita.

mod app;
mod backend;
mod data_ui;
mod images;
mod lists;
mod script;
mod session_ui;
mod widgets;

use adw::prelude::*;
use backend::{Backend, FixtureBackend};
use gtk::{gdk, gio, glib};
use serde_json::json;
use std::collections::VecDeque;
use std::path::PathBuf;
use std::rc::Rc;

const APP_ID: &str = "io.github.dixonSolutions.Godnotic";

const HELP: &str = "Usage: godnotic-loader [OPTION]... [godnotic://join/HOST:PORT]
  --fixture=NAME|PATH   render a UI-bridge fixture (tests/uibridge/fixtures) instead of the engine
  --fixtures-dir=DIR    where fixture names are looked up
  --size=WxH            window size
  --dark, --light       force the colour scheme (default: the system's)
  --reduced-motion      no animations
  --snapshot=FILE.png   render the window to a PNG after --snapshot-delay ms and quit
  --script=FILE         drive the UI (wait, click, row, back, snapshot, size, quit): tests
  --time                print start-up timings
";

#[derive(Default, Clone)]
struct Opts {
    fixture: Option<String>,
    fixtures_dir: Option<PathBuf>,
    size: Option<(i32, i32)>,
    scheme: Option<adw::ColorScheme>,
    reduced: bool,
    snapshot: Option<String>,
    snapshot_delay: u64,
    script: Option<String>,
    time: bool,
    link: Option<String>,
}

fn parse_args() -> Opts {
    let mut o = Opts { snapshot_delay: 1200, ..Default::default() };
    for a in std::env::args().skip(1) {
        let (k, v) = a.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())).unwrap_or((a.clone(), String::new()));
        match k.as_str() {
            "--help" | "-h" => {
                print!("{HELP}");
                std::process::exit(0);
            }
            "--fixture" => o.fixture = Some(v),
            "--fixtures-dir" => o.fixtures_dir = Some(v.into()),
            "--size" => o.size = v.split_once('x').and_then(|(w, h)| Some((w.parse().ok()?, h.parse().ok()?))),
            "--dark" => o.scheme = Some(adw::ColorScheme::ForceDark),
            "--light" => o.scheme = Some(adw::ColorScheme::ForceLight),
            "--reduced-motion" => o.reduced = true,
            "--snapshot" => o.snapshot = Some(v),
            "--snapshot-delay" => o.snapshot_delay = v.parse().unwrap_or(1200),
            "--script" => o.script = Some(v),
            "--time" => o.time = true,
            _ if a.starts_with("godnotic:") => o.link = Some(a),
            _ => eprintln!("godnotic-loader: unknown argument {a}"),
        }
    }
    o
}

fn fixtures_dir(o: &Opts) -> PathBuf {
    if let Some(d) = &o.fixtures_dir {
        return d.clone();
    }
    if let Some(f) = &o.fixture {
        let p = PathBuf::from(f);
        if p.is_file() {
            return p.parent().map(PathBuf::from).unwrap_or_default();
        }
    }
    std::env::var_os("GODNOTIC_FIXTURES").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../tests/uibridge/fixtures")))
}

fn make_backend(o: &Opts) -> Backend {
    if let Some(f) = &o.fixture {
        let dir = fixtures_dir(o);
        let name = PathBuf::from(f).file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| f.clone());
        let name = if name.ends_with(".json") { name } else { format!("{name}.json") };
        return match backend::load_fixture(&dir, &name) {
            Ok(player) => Backend::Fixture(Box::new(FixtureBackend { builtin: false, dir, player, current: name, history: vec![], incoming: VecDeque::new() })),
            Err(e) => Backend::Offline(format!("Could not read the fixture: {e}")),
        };
    }
    // The engine's UI-bridge backend mode lands with work/uibridge; until then the
    // app shows its own screens and opens the game's menu for the rest.
    match backend::load_builtin("home.json") {
        Ok(player) => Backend::Fixture(Box::new(FixtureBackend { builtin: true, dir: PathBuf::new(), player, current: "home.json".into(), history: vec![], incoming: VecDeque::new() })),
        Err(e) => Backend::Offline(e),
    }
}

fn css() {
    let p = gtk::CssProvider::new();
    p.load_from_string(include_str!("../data/style.css"));
    if let Some(d) = gdk::Display::default() {
        gtk::style_context_add_provider_for_display(&d, &p, gtk::STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

fn main() -> glib::ExitCode {
    let t0 = std::time::Instant::now();
    // WM_CLASS on X11 follows the program name: the app id, as the desktop file's StartupWMClass says.
    glib::set_prgname(Some(APP_ID));
    godnotic_core::paths::GamePaths::adopt_old_name(&std::env::vars().collect());
    let o = parse_args();
    let test_run = o.snapshot.is_some() || o.script.is_some() || std::env::var_os("GODNOTIC_EXIT_AFTER_FRAME").is_some() || std::env::var_os("GODNOTIC_NON_UNIQUE").is_some();
    let flags = if test_run { gio::ApplicationFlags::NON_UNIQUE } else { gio::ApplicationFlags::HANDLES_OPEN };
    let gapp = adw::Application::builder().application_id(APP_ID).flags(flags).build();
    let state: Rc<std::cell::RefCell<Option<app::App>>> = Rc::new(std::cell::RefCell::new(None));

    let (st, o2) = (state.clone(), o.clone());
    gapp.connect_activate(move |gapp| {
        if let Some(a) = st.borrow().as_ref() {
            if let Some(u) = a.ui.borrow().as_ref() {
                u.win.present();
            }
            return;
        }
        css();
        if let Some(s) = o2.scheme {
            adw::StyleManager::default().set_color_scheme(s);
        }
        let a = app::Shared::new(make_backend(&o2));
        a.reduced_motion.set(o2.reduced || !gtk::Settings::default().map(|s| s.is_gtk_enable_animations()).unwrap_or(true));
        a.images.borrow_mut().preload_brand();
        let win = app::build_window(&a, gapp, o2.size);
        if a.reduced_motion.get() {
            win.add_css_class("gn-reduced");
        }
        let (a2, o3) = (a.clone(), o2.clone());
        let first = std::cell::Cell::new(true);
        win.add_tick_callback(move |w, _| {
            if !first.replace(false) {
                return glib::ControlFlow::Break;
            }
            let ms = t0.elapsed().as_secs_f64() * 1000.0;
            if o3.time || std::env::var_os("GODNOTIC_TIMING").is_some() {
                eprintln!("godnotic-loader: first frame after {ms:.1} ms (process start)");
            }
            if std::env::var_os("GODNOTIC_EXIT_AFTER_FRAME").is_some() {
                if let Some(app) = w.application() {
                    app.quit();
                }
                return glib::ControlFlow::Break;
            }
            // The described UI comes after the first frame: nothing blocks the window.
            start_ui(&a2);
            if let Some(path) = o3.snapshot.clone() {
                let w = w.clone();
                glib::timeout_add_local_once(std::time::Duration::from_millis(o3.snapshot_delay), move || {
                    script::snapshot(&w, &path);
                    if let Some(app) = w.application() {
                        app.quit();
                    }
                });
            }
            if let Some(s) = o3.script.clone() {
                script::run(&a2, &s);
            }
            glib::ControlFlow::Break
        });
        win.present();
        *st.borrow_mut() = Some(a);
    });
    let st = state.clone();
    gapp.connect_open(move |gapp, files, _| {
        gapp.activate();
        for f in files {
            let uri = f.uri();
            let a = st.borrow().clone();
            if let (Some(a), Some(addr)) = (a, godnotic_core::link::parse_join(&uri)) {
                session_ui::confirm_join(&a, addr);
            }
        }
    });
    if let Some(l) = &o.link {
        eprintln!("godnotic-loader: link {l}");
    }
    // GApplication would parse our options; they are already read.
    gapp.run_with_args(&["godnotic-loader"])
}

/// Says hello to the backend; the snapshot arrives through the pump.
fn start_ui(a: &app::App) {
    if matches!(&*a.backend.borrow(), Backend::Offline(_)) {
        show_offline(a);
        return;
    }
    let caps = caps(a);
    let m = a.core.borrow_mut().client.hello("godnotic-gtk", env!("CARGO_PKG_VERSION"), caps);
    app::send(a, m);
    backend::run_steps(a);
}

/// Section 7's capabilities from the window and the system.
fn caps(a: &app::App) -> serde_json::Value {
    let (w, h) = a.ui.borrow().as_ref().map(|u| (u.win.width().max(u.win.default_width()), u.win.height().max(u.win.default_height()))).unwrap_or((1100, 760));
    let class = |v: i32, a: i32, b: i32| if v < a { "compact" } else if v < b { "medium" } else { "expanded" };
    let env: std::collections::HashMap<String, String> = std::env::vars().collect();
    let p = godnotic_core::platform::Platform::detect(&env, &|f| std::fs::read_to_string(f).ok(), &|d| std::fs::read_dir(d).map(|r| r.filter_map(|e| e.ok()?.file_name().into_string().ok()).collect()).unwrap_or_default());
    json!({
        "nodes": godnotic_core::bridge::NODE_TYPES, "hints": godnotic_core::bridge::HINTS,
        "width": w, "height": h, "widthClass": class(w, 600, 840), "heightClass": class(h, 480, 900),
        "input": if p.form == godnotic_core::platform::FormFactor::Phone { json!(["touch"]) } else { json!(["mouse", "keyboard"]) },
        "locale": std::env::var("LANG").ok().and_then(|l| l.split(['.', '_']).next().map(str::to_string)).unwrap_or_else(|| "en".into()),
        "reducedMotion": a.reduced_motion.get(), "powerSave": p.saving_power.unwrap_or(false), "thermal": "nominal",
        "insets": {"top": 0, "right": 0, "bottom": 0, "left": 0},
    })
}

/// No engine UI: the skeleton gives way to a status page.
fn show_offline(a: &app::App) {
    let why = match &*a.backend.borrow() {
        Backend::Offline(w) => w.clone(),
        _ => return,
    };
    let Some(nav) = a.ui.borrow().as_ref().map(|u| u.nav.clone()) else { return };
    let tv = adw::ToolbarView::new();
    let hb = adw::HeaderBar::new();
    hb.pack_end(&data_ui::button());
    tv.add_top_bar(&hb);
    let s = adw::StatusPage::builder().icon_name("input-gaming-symbolic").title("Godnotic").description(glib::markup_escape_text(&why)).build();
    let play = gtk::Button::with_label("Play");
    play.add_css_class("gn-play");
    play.set_halign(gtk::Align::Center);
    let w = Rc::downgrade(a);
    play.connect_clicked(move |_| {
        if let Some(a) = w.upgrade() {
            session_ui::start(&a, godnotic_core::session::Action::Play);
        }
    });
    s.set_child(Some(&play));
    tv.set_content(Some(&s));
    let page = adw::NavigationPage::builder().tag("offline").title("Godnotic").child(&tv).can_pop(false).build();
    nav.replace(&[page]);
}
