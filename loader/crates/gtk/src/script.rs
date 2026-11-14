// SPDX-License-Identifier: GPL-3.0-or-later
//! Test aids: render the window to a PNG, and a small script that drives the
//! UI through its real widgets (a "click" activates the node's GtkButton or
//! row, exactly what a pointer click does after hit-testing), for headless
//! screenshots and end-to-end runs. Lines:
//!
//! ```text
//! wait 500            milliseconds
//! click <node id>     activate the widget of a node (or <node>:<action id> for an action button)
//! row <list id> <n>   activate row n of a list
//! scroll <list id> <n>  scroll a list so row n is visible
//! back                the back event
//! size 420 900        resize the window
//! dark | light        colour scheme
//! snapshot <file.png> render the window
//! count <file>        append the number of mapped top-level windows of this process
//! labels <file>       write the text of every label on screen (mapped and drawn), one per line
//! type <node> <text>  set an entry's text
//! join <addr> [pw]    start a session joining a server (as a godnotic:// link would)
//! play                start the game the classic way
//! waitvisible <ms>    wait until the window shows again (the session ended)
//! measure <file> [node...]  append the window's size and the width its content needs, the
//!                     number of map pictures that arrived, then each node's widget box in
//!                     window coordinates and its own minimum width
//! quit
//! ```

use crate::app::{self, App};
use adw::prelude::*;
use gtk::{gdk, glib, gsk};
use std::collections::VecDeque;
use std::rc::Rc;

pub fn snapshot(w: &impl IsA<gtk::Widget>, path: &str) {
    let w = w.as_ref();
    let (width, height) = (w.width(), w.height());
    let paintable = gtk::WidgetPaintable::new(Some(w));
    let snap = gtk::Snapshot::new();
    paintable.snapshot(&snap, width as f64, height as f64);
    let Some(node) = snap.to_node() else {
        eprintln!("snapshot: nothing drawn yet");
        return;
    };
    let Some(renderer) = w.native().and_then(|n| n.renderer()) else { return };
    let tex = renderer.render_texture(node, None::<&gsk::graphene::Rect>);
    match tex.save_to_png(path) {
        Ok(()) => println!("snapshot {width}x{height} -> {path}"),
        Err(e) => eprintln!("snapshot failed: {e}"),
    }
}

pub fn run(app: &App, file: &str) {
    let lines: VecDeque<String> = std::fs::read_to_string(file).unwrap_or_default().lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')).map(str::to_string).collect();
    step(Rc::downgrade(app), lines);
}

fn step(w: std::rc::Weak<app::Shared>, mut lines: VecDeque<String>) {
    let Some(app) = w.upgrade() else { return };
    let Some(line) = lines.pop_front() else { return };
    let mut parts = line.split_whitespace();
    let cmd = parts.next().unwrap_or("");
    let args: Vec<&str> = parts.collect();
    let mut delay = 60u64;
    let win = app.ui.borrow().as_ref().map(|u| u.win.clone());
    match cmd {
        "wait" => delay = args.first().and_then(|a| a.parse().ok()).unwrap_or(500),
        "click" => {
            let w = args.first().and_then(|id| app.ui.borrow().as_ref().and_then(|u| u.activators.get(*id).cloned()));
            match w {
                Some(w) => {
                    if let Some(r) = w.downcast_ref::<adw::ActionRow>() {
                        r.emit_by_name::<()>("activated", &[]);
                    } else if let Some(c) = w.downcast_ref::<gtk::CheckButton>() {
                        c.set_active(!c.is_active());
                    } else if let Some(s) = w.downcast_ref::<adw::SwitchRow>() {
                        s.set_active(!s.is_active());
                    } else {
                        w.activate();
                    }
                }
                None => eprintln!("script: no widget for {:?}", args.first()),
            }
            delay = 250;
        }
        "row" => {
            let list = args.first().copied().unwrap_or("");
            let n: u32 = args.get(1).and_then(|a| a.parse().ok()).unwrap_or(0);
            let w = app.ui.borrow().as_ref().and_then(|u| u.activators.get(list).cloned());
            if let Some(cv) = w.as_ref().and_then(|w| w.downcast_ref::<gtk::ColumnView>()) {
                cv.emit_by_name::<()>("activate", &[&n]);
            } else if let Some(gv) = w.as_ref().and_then(|w| w.downcast_ref::<gtk::GridView>()) {
                gv.emit_by_name::<()>("activate", &[&n]);
            }
            delay = 250;
        }
        "scroll" => {
            let list = args.first().copied().unwrap_or("");
            let n: u32 = args.get(1).and_then(|a| a.parse().ok()).unwrap_or(0);
            let w = app.ui.borrow().as_ref().and_then(|u| u.activators.get(list).cloned());
            if let Some(cv) = w.as_ref().and_then(|w| w.downcast_ref::<gtk::ColumnView>()) {
                cv.scroll_to(n, None::<&gtk::ColumnViewColumn>, gtk::ListScrollFlags::NONE, None);
            } else if let Some(gv) = w.as_ref().and_then(|w| w.downcast_ref::<gtk::GridView>()) {
                gv.scroll_to(n, gtk::ListScrollFlags::NONE, None);
            }
            delay = 200;
        }
        "back" => {
            app::back(&app);
            delay = 250;
        }
        "fling" => {
            // fling <list id> <ms>: scrolls the list from top to bottom over <ms> on the frame
            // clock and prints the frame intervals (indicative only under Xvfb).
            let list = args.first().copied().unwrap_or("");
            let ms: i64 = args.get(1).and_then(|a| a.parse().ok()).unwrap_or(3000);
            let w = app.ui.borrow().as_ref().and_then(|u| u.activators.get(list).cloned());
            if let Some(sw) = w.and_then(|w| w.ancestor(gtk::ScrolledWindow::static_type())).and_downcast::<gtk::ScrolledWindow>() {
                let adj = sw.vadjustment();
                let start = std::cell::Cell::new(0i64);
                let times = std::rc::Rc::new(std::cell::RefCell::new(Vec::<i64>::new()));
                let t2 = times.clone();
                sw.add_tick_callback(move |_, clock| {
                    let now = clock.frame_time();
                    if start.get() == 0 {
                        start.set(now);
                    }
                    t2.borrow_mut().push(now);
                    let f = ((now - start.get()) as f64 / (ms as f64 * 1000.0)).min(1.0);
                    adj.set_value(f * (adj.upper() - adj.page_size()));
                    if f >= 1.0 {
                        let t = t2.borrow();
                        let mut d: Vec<i64> = t.windows(2).map(|x| x[1] - x[0]).collect();
                        d.sort_unstable();
                        if !d.is_empty() {
                            let pct = |p: f64| d[((d.len() - 1) as f64 * p) as usize] as f64 / 1000.0;
                            println!("fling: {} frames, median {:.1} ms, p95 {:.1} ms, max {:.1} ms, scrolled to {:.0} px", d.len() + 1, pct(0.5), pct(0.95), pct(1.0), adj.value());
                        }
                        return glib::ControlFlow::Break;
                    }
                    glib::ControlFlow::Continue
                });
            }
            delay = (ms + 500) as u64;
        }
        "type" => {
            // type <node id> <text>: sets an entry's text as if typed (its change signals run).
            let w = args.first().and_then(|id| app.ui.borrow().as_ref().and_then(|u| u.activators.get(*id).cloned()));
            let text = args.get(1..).map(|a| a.join(" ")).unwrap_or_default();
            if let Some(e) = w.as_ref().and_then(|w| w.dynamic_cast_ref::<gtk::Editable>()) {
                e.set_text(&text);
            } else {
                eprintln!("script: no entry {:?}", args.first());
            }
            delay = 200;
        }
        "join" => {
            let addr = args.first().and_then(|a| godnotic_core::link::parse_join(a));
            if let Some(addr) = addr {
                let password = args.get(1).map(|p| p.to_string());
                crate::session_ui::start(&app, godnotic_core::session::Action::Connect { address: addr, password });
            }
            delay = 300;
        }
        "play" => {
            crate::session_ui::start(&app, godnotic_core::session::Action::Play);
            delay = 300;
        }
        "waitvisible" => {
            // Polls until the window is shown again (the game ended), up to N ms.
            let left: i64 = args.first().and_then(|a| a.parse().ok()).unwrap_or(30000);
            let shown = win.as_ref().is_some_and(|w| w.is_visible());
            if !shown && left > 0 {
                lines.push_front(format!("waitvisible {}", left - 200));
                delay = 200;
            }
        }
        "size" => {
            if let (Some(win), Some(wd), Some(h)) = (&win, args.first().and_then(|a| a.parse().ok()), args.get(1).and_then(|a| a.parse().ok())) {
                win.set_default_size(wd, h);
                win.unmaximize();
            }
            delay = 400;
        }
        "dark" => adw::StyleManager::default().set_color_scheme(adw::ColorScheme::ForceDark),
        "light" => adw::StyleManager::default().set_color_scheme(adw::ColorScheme::ForceLight),
        "snapshot" => {
            if let (Some(win), Some(p)) = (&win, args.first()) {
                snapshot(win, p);
            }
        }
        "labels" => {
            // what a person sees: the text of the labels that are mapped and have room
            // (inside the window: a label scrolled out of view does not count)
            fn walk(w: &gtk::Widget, top: &gtk::Widget, out: &mut Vec<String>) {
                if !w.is_mapped() || !w.is_child_visible() {
                    return;
                }
                if let Some(l) = w.downcast_ref::<gtk::Label>() {
                    let inside = l.compute_bounds(top).is_some_and(|b| {
                        b.width() > 0.0 && b.height() > 0.0 && b.x() >= -1.0 && b.y() >= -1.0
                            && b.x() + b.width() <= top.width() as f32 + 1.0 && b.y() + b.height() <= top.height() as f32 + 1.0
                    });
                    if inside && !l.text().is_empty() {
                        out.push(l.text().to_string());
                    }
                }
                let mut c = w.first_child();
                while let Some(k) = c {
                    walk(&k, top, out);
                    c = k.next_sibling();
                }
            }
            let mut out = Vec::new();
            for t in gtk::Window::list_toplevels() {
                if t.is_mapped() {
                    walk(&t, &t, &mut out);
                }
            }
            if let Some(p) = args.first() {
                let _ = std::fs::write(p, out.join("\n") + "\n");
            }
        }
        "count" => {
            let n = gtk::Window::list_toplevels().iter().filter(|w| w.is_mapped()).count();
            if let Some(p) = args.first() {
                use std::io::Write;
                if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(p) {
                    let _ = writeln!(f, "{n}");
                }
            }
            println!("toplevels {n}");
        }
        "measure" => {
            // layout checks: "window W H min M" (M > W: the content does not fit and is cut off),
            // then "<node> X Y W H min M" for each node asked for (or "<node> none")
            let mut out = String::new();
            if let Some(win) = &win {
                let content = win.content().map(|c| c.measure(gtk::Orientation::Horizontal, -1).0).unwrap_or(0);
                out.push_str(&format!("window {} {} min {}\n", win.width(), win.height(), content));
                out.push_str(&format!("map-pictures {}\n", app.images.borrow().arrived_count("pk3:maps/")));
                for id in args.iter().skip(1) {
                    let w = app.ui.borrow().as_ref().and_then(|u| u.activators.get(*id).cloned());
                    match w.and_then(|w| w.compute_bounds(win).map(|b| (b, w.measure(gtk::Orientation::Horizontal, -1).0))) {
                        Some((b, min)) => out.push_str(&format!("{id} {:.0} {:.0} {:.0} {:.0} min {min}\n", b.x(), b.y(), b.width(), b.height())),
                        None => out.push_str(&format!("{id} none\n")),
                    }
                }
            }
            if let Some(p) = args.first() {
                use std::io::Write;
                if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(p) {
                    let _ = f.write_all(out.as_bytes());
                }
            }
            print!("{out}");
        }
        "quit" => {
            if let Some(a) = win.and_then(|w| w.application()) {
                a.quit();
            }
            return;
        }
        other => eprintln!("script: unknown command {other}"),
    }
    let _ = gdk::Display::default();
    glib::timeout_add_local_once(std::time::Duration::from_millis(delay), move || step(w, lines));
}
