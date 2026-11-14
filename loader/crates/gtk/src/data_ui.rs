// SPDX-License-Identifier: GPL-3.0-or-later
//! Storage and game data as a small popover from the header bar (native: it
//! must work before any engine can run). Shows what is installed, the data
//! set, and Download / Check again with progress; music is always part of
//! the set. The downloader is the core's (ranged, resumable, CRC-checked).

use godnotic_core::fetch::{self, datasets, http_ureq::UreqHttp, CancelToken, Progress};
use godnotic_core::paths::GamePaths;
use gtk::glib;
use gtk::prelude::*;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc;

/// Where the engine's QuakeC reads and writes (`<userdir>/data/data`): the download's status and request files.
fn engine_write_dir() -> PathBuf {
    let env: HashMap<String, String> = std::env::vars().collect();
    let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_string_lossy().to_string()));
    PathBuf::from(GamePaths::resolve(&env, exe.as_deref(), &|f| Path::new(f).exists(), "").engine_user_dir).join("data/data")
}

fn data_dir() -> PathBuf {
    let env: HashMap<String, String> = std::env::vars().collect();
    let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_string_lossy().to_string()));
    PathBuf::from(GamePaths::resolve(&env, exe.as_deref(), &|f| Path::new(f).exists(), "").user_data())
}

fn installed(dir: &Path) -> (usize, u64, bool) {
    let mut n = 0;
    let mut bytes = 0;
    for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if name.ends_with(".pk3") {
            n += 1;
            bytes += e.metadata().map(|m| m.len()).unwrap_or(0);
        }
    }
    (n, bytes, dir.join(datasets::READY_MARKER).exists())
}

fn free_space(p: &Path) -> u64 {
    let Ok(c) = std::ffi::CString::new(p.to_string_lossy().as_bytes()) else { return u64::MAX };
    // SAFETY: statvfs fills the struct we pass; the path is a valid C string.
    unsafe {
        let mut s: libc::statvfs = std::mem::zeroed();
        if libc::statvfs(c.as_ptr(), &mut s) == 0 { s.f_bavail as u64 * s.f_frsize as u64 } else { u64::MAX }
    }
}

enum Msg {
    Progress(Progress),
    Done(Result<(), String>),
}

/// The header-bar button with the storage popover.
pub fn button() -> gtk::MenuButton {
    let mb = gtk::MenuButton::builder().icon_name("drive-harddisk-symbolic").tooltip_text("Storage and game data").build();
    let pop = gtk::Popover::new();
    let v = gtk::Box::new(gtk::Orientation::Vertical, 10);
    v.set_margin_top(12);
    v.set_margin_bottom(12);
    v.set_margin_start(12);
    v.set_margin_end(12);
    v.set_size_request(300, -1);
    let title = gtk::Label::new(Some("Game data"));
    title.add_css_class("heading");
    title.set_xalign(0.0);
    let status = gtk::Label::new(None);
    status.set_xalign(0.0);
    status.set_wrap(true);
    let detail = gtk::Label::new(None);
    detail.set_xalign(0.0);
    detail.set_wrap(true);
    detail.add_css_class("dim-label");
    detail.add_css_class("caption");
    let bar = gtk::ProgressBar::new();
    bar.set_visible(false);
    let go = gtk::Button::with_label("Download");
    go.add_css_class("suggested-action");
    go.add_css_class("pill");
    let stop = gtk::Button::with_label("Stop");
    stop.add_css_class("pill");
    stop.set_visible(false);
    for w in [title.upcast_ref::<gtk::Widget>(), status.upcast_ref(), detail.upcast_ref(), bar.upcast_ref(), go.upcast_ref(), stop.upcast_ref()] {
        v.append(w);
    }
    pop.set_child(Some(&v));
    mb.set_popover(Some(&pop));

    let refresh = {
        let (status, detail, go) = (status.clone(), detail.clone(), go.clone());
        move || {
            let dir = data_dir();
            let (n, bytes, ready) = installed(&dir);
            status.set_text(&if ready { format!("Installed · {n} packs, {} MB", bytes >> 20) } else if n > 0 { format!("Incomplete · {n} packs, {} MB", bytes >> 20) } else { "Not downloaded yet".into() });
            let choice = datasets::choose(None, std::env::var(datasets::ENV).ok().as_deref(), None, None);
            detail.set_text(&format!("{} · {} MB free · {}", choice.reason, free_space(&dir) >> 20, dir.display()));
            go.set_label(if ready { "Check again" } else { "Download" });
        }
    };
    refresh();
    let r2 = refresh.clone();
    pop.connect_show(move |_| r2());

    let token: std::rc::Rc<std::cell::RefCell<Option<CancelToken>>> = Default::default();
    let t2 = token.clone();
    stop.connect_clicked(move |_| {
        if let Some(t) = t2.borrow().as_ref() {
            t.cancel();
        }
    });
    let (bar2, stop2, status2) = (bar.clone(), stop.clone(), status.clone());
    go.connect_clicked(move |go| {
        let (tx, rx) = mpsc::channel::<Msg>();
        let t = CancelToken::new();
        *token.borrow_mut() = Some(t.clone());
        go.set_sensitive(false);
        stop2.set_visible(true);
        bar2.set_visible(true);
        bar2.set_fraction(0.0);
        status2.set_text("Looking at the archive…");
        std::thread::spawn(move || {
            let dir = data_dir();
            let http = UreqHttp::new();
            let choice = datasets::choose(None, std::env::var(datasets::ENV).ok().as_deref(), None, None);
            let r: Result<(), fetch::Error> = {
                let tx2 = tx.clone();
                let listen = move |p: Progress| {
                    let _ = tx2.send(Msg::Progress(p));
                };
                let free = |p: &Path| free_space(p);
                // The split set when the server has its manifest (first the packs a match needs, then the
                // rest while the game may already run: the status file tells it what landed), else one archive.
                let engine_dir = engine_write_dir();
                datasets::fetch_all(&http, &choice, None, &dir, Some(&engine_dir), fetch::Config::default(), &free, &t, &listen, &|| {}, datasets::FULL_URL)
            };
            let _ = tx.send(Msg::Done(r.map_err(|e| e.to_string())));
        });
        let (go, bar, stop, status, refresh) = (go.clone(), bar2.clone(), stop2.clone(), status2.clone(), refresh.clone());
        glib::timeout_add_local(std::time::Duration::from_millis(100), move || {
            for m in rx.try_iter() {
                match m {
                    Msg::Progress(p) => {
                        if p.total > 0 {
                            bar.set_fraction(p.done as f64 / p.total as f64);
                            status.set_text(&format!("{} of {} MB{}", p.done >> 20, p.total >> 20, if p.verifying { " · checking" } else { "" }));
                        }
                    }
                    Msg::Done(r) => {
                        go.set_sensitive(true);
                        stop.set_visible(false);
                        bar.set_visible(false);
                        refresh();
                        if let Err(e) = r {
                            status.set_text(&e);
                        }
                        return glib::ControlFlow::Break;
                    }
                }
            }
            glib::ControlFlow::Continue
        });
    });
    // a split set that is not whole yet carries on by itself (docs/DATA_FETCH.md)
    if datasets::split_unfinished(&data_dir()) {
        go.emit_clicked();
    }
    mb
}
