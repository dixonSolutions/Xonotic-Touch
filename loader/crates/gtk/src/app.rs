// SPDX-License-Identifier: GPL-3.0-or-later
//! The loader window and the GTK renderer of the engine's UI bridge.
//!
//! State is split so GTK callbacks never re-enter a borrowed cell: the core
//! (store, client) is borrowed mutably only while a message is applied and
//! immutably while widgets are built or rows bound; callbacks only queue
//! outgoing messages, which a main-loop pump sends.

use crate::backend::Backend;
use crate::images::Images;
use crate::lists::{index_of, WindowModel};
use crate::widgets;
use adw::prelude::*;
use godnotic_core::bridge::{Change, Client, Node, Props, Splice, Store};
use gtk::{gdk, gio, glib};
use serde_json::{json, Map, Value};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet, VecDeque};
use std::rc::{Rc, Weak};

pub struct Core {
    pub store: Store,
    pub client: Client,
}

/// A list node on screen and its model.
pub struct ListView {
    pub source: String,
    pub model: WindowModel,
    pub sort: Option<String>,
    pub desc: bool,
    pub filter: String,
    /// First and last positions bound since the last check (for window requests).
    pub seen: Cell<(usize, usize)>,
}

/// Updates a control's widget in place from its node.
pub type Updater = Rc<dyn Fn(&Node)>;

pub struct Ui {
    pub win: adw::ApplicationWindow,
    pub nav: adw::NavigationView,
    pub toasts: adw::ToastOverlay,
    pub banner: adw::Banner,
    /// Screen id -> its page.
    pub pages: HashMap<String, adw::NavigationPage>,
    pub shown_stack: Vec<String>,
    pub dialogs: HashMap<String, adw::Dialog>,
    /// Node id -> in-place updater for controls.
    pub updaters: HashMap<String, Updater>,
    /// Node id -> the widget a script "click" activates.
    pub activators: HashMap<String, gtk::Widget>,
    pub lists: HashMap<String, Rc<RefCell<ListView>>>,
    pub shown_toasts: HashSet<String>,
    pub skeleton: bool,
}

pub struct Shared {
    pub core: RefCell<Core>,
    pub out: RefCell<VecDeque<String>>,
    /// Set while widgets are being filled in, so their change signals send nothing.
    pub quiet: Cell<bool>,
    pub ui: RefCell<Option<Ui>>,
    pub images: RefCell<Images>,
    pub backend: RefCell<Backend>,
    pub narrow: Cell<bool>,
    /// Clicks on lists that tell choosing from playing (one click chooses, two play).
    pub clicks: RefCell<godnotic_core::bridge::Clicks>,
    pub reduced_motion: Cell<bool>,
    pub pump_queued: Cell<bool>,
    pub first_snapshot_ms: Cell<Option<f64>>,
    pub t0: std::time::Instant,
    /// Requests made while the core was borrowed (widget building, row binding): sent by the pump.
    pub deferred: RefCell<Vec<Deferred>>,
}

pub enum Deferred {
    Image(String, u32, u32),
    Rows(Rc<RefCell<ListView>>),
}

/// Asks for an image later (safe from any callback).
pub fn want_image(app: &App, r: &str, w: u32, h: u32) {
    app.deferred.borrow_mut().push(Deferred::Image(r.to_string(), w, h));
    schedule(app);
}

/// Asks for a list's visible rows later (safe from any callback).
pub fn want_rows(app: &App, l: &Rc<RefCell<ListView>>) {
    app.deferred.borrow_mut().push(Deferred::Rows(l.clone()));
    schedule(app);
}

pub type App = Rc<Shared>;

impl Shared {
    pub fn new(backend: Backend) -> App {
        Rc::new(Shared {
            core: RefCell::new(Core { store: Store::new(), client: Client::new() }),
            out: RefCell::new(VecDeque::new()),
            quiet: Cell::new(false),
            ui: RefCell::new(None),
            images: RefCell::new(Images::default()),
            backend: RefCell::new(backend),
            narrow: Cell::new(false),
            clicks: RefCell::new(Default::default()),
            reduced_motion: Cell::new(false),
            pump_queued: Cell::new(false),
            first_snapshot_ms: Cell::new(None),
            t0: std::time::Instant::now(),
            deferred: RefCell::new(Vec::new()),
        })
    }
}

/// Queues a message for the engine and makes sure the pump runs.
pub fn send(app: &App, msg: String) {
    app.out.borrow_mut().push_back(msg);
    schedule(app);
}

pub fn schedule(app: &App) {
    if app.pump_queued.replace(true) {
        return;
    }
    let w = Rc::downgrade(app);
    glib::idle_add_local_once(move || {
        if let Some(a) = w.upgrade() {
            a.pump_queued.set(false);
            pump(&a);
        }
    });
}

/// Sends what is queued, takes what arrived, applies it and renders the changes.
pub fn pump(app: &App) {
    loop {
        let deferred: Vec<Deferred> = app.deferred.borrow_mut().drain(..).collect();
        let mut seen_lists = HashSet::new();
        for d in deferred {
            match d {
                Deferred::Image(r, w, h) => {
                    let ask = app.core.borrow_mut().store.want_image(&r);
                    if ask {
                        let m = app.core.borrow_mut().client.image(&r, w, h);
                        app.out.borrow_mut().push_back(m);
                    }
                }
                Deferred::Rows(l) => {
                    if seen_lists.insert(Rc::as_ptr(&l) as usize) {
                        request_rows(app, &l);
                    }
                }
            }
        }
        let outgoing: Vec<String> = app.out.borrow_mut().drain(..).collect();
        for m in &outgoing {
            app.backend.borrow_mut().send(m);
        }
        let incoming = app.backend.borrow_mut().take();
        if incoming.is_empty() && app.out.borrow().is_empty() {
            break;
        }
        for m in incoming {
            let changes = app.core.borrow_mut().store.apply(&m);
            handle_changes(app, &changes, &m);
        }
    }
}

fn handle_changes(app: &App, changes: &[Change], msg: &Map<String, Value>) {
    for c in changes {
        match c {
            Change::Snapshot => {
                if app.first_snapshot_ms.get().is_none() {
                    let ms = app.t0.elapsed().as_secs_f64() * 1000.0;
                    app.first_snapshot_ms.set(Some(ms));
                    if std::env::var_os("GODNOTIC_TIMING").is_some() {
                        eprintln!("godnotic-loader: first snapshot after {ms:.1} ms");
                    }
                }
                rebuild(app);
            }
            Change::Set(ids) => update(app, ids),
            Change::Deleted(_) => {}
            Change::NeedSync => {
                let have = app.core.borrow().store.rev;
                let m = app.core.borrow_mut().client.sync(have);
                send(app, m);
            }
            Change::Rows { source, splices } => rows(app, source, splices),
            Change::Source { source, .. } => {
                // Ask again for what is visible; the old rows stay until the reply.
                let lists: Vec<_> = ui_lists(app).into_iter().filter(|l| l.borrow().source == *source).collect();
                for l in lists {
                    request_rows(app, &l);
                }
            }
            Change::Image { reference, path, data } => app.images.borrow_mut().arrived(reference, path.as_deref(), data.as_deref()),
            Change::ImageMissing(r) => app.images.borrow_mut().missing(r),
            Change::Error { code, msg: m } => eprintln!("godnotic-loader: engine error {code}: {m}"),
            Change::Bye(reason) => crate::backend::engine_gone(app, reason),
            Change::Hello { .. } => {}
        }
    }
    let _ = msg;
}

fn ui_lists(app: &App) -> Vec<Rc<RefCell<ListView>>> {
    app.ui.borrow().as_ref().map(|u| u.lists.values().cloned().collect()).unwrap_or_default()
}

fn rows(app: &App, source: &str, splices: &[Splice]) {
    for l in ui_lists(app) {
        if l.borrow().source == source {
            let model = l.borrow().model.clone();
            for s in splices {
                model.splice(s);
            }
        }
    }
}

/// Asks for the rows around what the list showed since the last check.
pub fn request_rows(app: &App, l: &Rc<RefCell<ListView>>) {
    let (source, sort, desc, filter, (first, last)) = {
        let b = l.borrow();
        (b.source.clone(), b.sort.clone(), b.desc, b.filter.clone(), b.seen.get())
    };
    let first = if first == usize::MAX { 0 } else { first };
    let last = last.max(first).max(30);
    let reqs = {
        let mut core = app.core.borrow_mut();
        core.store.sources.entry(source.clone()).or_default().need(first, last)
    };
    for (off, lim) in reqs {
        let m = app.core.borrow_mut().client.window(&source, off, lim, sort.as_deref(), desc, Some(&filter));
        send(app, m);
    }
}

/// The described tree was replaced: show `app.stack`, `app.modal`, busy state and toasts.
pub fn rebuild(app: &App) {
    {
        let mut ui = app.ui.borrow_mut();
        let Some(ui) = ui.as_mut() else { return };
        if ui.skeleton {
            ui.skeleton = false;
            ui.pages.clear();
            ui.shown_stack.clear();
        }
    }
    sync_stack(app, true);
    sync_app_extras(app);
}

/// Nodes were inserted or replaced: update controls in place, rebuild the screens that hold anything else.
pub fn update(app: &App, ids: &[String]) {
    let mut screens = HashSet::new();
    let mut app_changed = false;
    {
        let core = app.core.borrow();
        let parents = parent_map(&core.store);
        let ui = app.ui.borrow();
        let Some(ui) = ui.as_ref() else { return };
        for id in ids {
            if id == "app" {
                app_changed = true;
                continue;
            }
            let Some(n) = core.store.node(id) else { continue };
            if let Some(f) = ui.updaters.get(id) {
                app.quiet.set(true);
                f(n);
                app.quiet.set(false);
                continue;
            }
            // A screen itself, or the screen above this node.
            let mut cur = id.clone();
            loop {
                match core.store.node(&cur) {
                    Some(x) if x.kind == "screen" => {
                        screens.insert(cur.clone());
                        break;
                    }
                    Some(x) if x.kind == "toast" => break,
                    _ => {}
                }
                match parents.get(&cur) {
                    Some(p) if p != "app" => cur = p.clone(),
                    _ => break,
                }
            }
        }
    }
    if app_changed {
        sync_stack(app, false);
        sync_app_extras(app);
    }
    for s in screens {
        refresh_screen(app, &s);
    }
}

fn parent_map(store: &Store) -> HashMap<String, String> {
    let mut m = HashMap::new();
    let mut todo = vec!["app".to_string()];
    while let Some(id) = todo.pop() {
        for c in store.node(&id).map(|n| n.children.clone()).unwrap_or_default() {
            m.insert(c.clone(), id.clone());
            todo.push(c);
        }
    }
    m
}

/// Makes the navigation view show exactly `app.stack`, with push/pop animations when it grew or shrank by one.
fn sync_stack(app: &App, rebuild_all: bool) {
    let stack = app.core.borrow().store.stack();
    let (nav, old) = {
        let ui = app.ui.borrow();
        let Some(ui) = ui.as_ref() else { return };
        (ui.nav.clone(), ui.shown_stack.clone())
    };
    // Build pages that are new (or every page on a fresh snapshot).
    let mut pages = Vec::new();
    for id in &stack {
        let existing = app.ui.borrow().as_ref().and_then(|u| u.pages.get(id).cloned());
        let page = match existing {
            Some(p) if !rebuild_all => p,
            Some(p) => {
                fill_page(app, &p, id);
                p
            }
            None => {
                let p = new_page(app, id);
                app.ui.borrow_mut().as_mut().map(|u| u.pages.insert(id.clone(), p.clone()));
                p
            }
        };
        pages.push(page);
    }
    if stack == old && !rebuild_all {
        return;
    }
    if stack.len() == old.len() + 1 && stack[..old.len()] == old[..] && !old.is_empty() {
        nav.push(pages.last().unwrap());
    } else if stack.len() + 1 == old.len() && old[..stack.len()] == stack[..] && !stack.is_empty() {
        nav.pop();
    } else {
        nav.replace(&pages);
    }
    // shown / hidden events
    let top_new = stack.last().cloned();
    let top_old = old.last().cloned();
    if top_new != top_old {
        if let Some(t) = &top_old {
            let m = app.core.borrow_mut().client.simple("hidden", Some(t));
            send(app, m);
        }
        if let Some(t) = &top_new {
            let m = app.core.borrow_mut().client.simple("shown", Some(t));
            send(app, m);
        }
    }
    let mut ui = app.ui.borrow_mut();
    if let Some(ui) = ui.as_mut() {
        ui.pages.retain(|k, _| stack.contains(k));
        ui.shown_stack = stack;
    }
}

/// Modal sheets and dialogs, the busy banner, toasts, and the game/menu context.
fn sync_app_extras(app: &App) {
    let (modal, busy, toasts, context) = {
        let core = app.core.borrow();
        let s = &core.store;
        let busy = s.app().and_then(|a| a.obj("busy").cloned());
        let toasts: Vec<Node> = s.toasts().into_iter().cloned().collect();
        (s.modal(), busy, toasts, s.context())
    };
    let Some((win, overlay, banner)) = app.ui.borrow().as_ref().map(|u| (u.win.clone(), u.toasts.clone(), u.banner.clone())) else { return };
    // Dialogs: close those gone, open the new ones.
    let open: Vec<String> = app.ui.borrow().as_ref().map(|u| u.dialogs.keys().cloned().collect()).unwrap_or_default();
    for id in open.iter().filter(|i| !modal.contains(i)) {
        if let Some(d) = app.ui.borrow_mut().as_mut().and_then(|u| u.dialogs.remove(id)) {
            d.force_close();
        }
    }
    for id in &modal {
        if open.contains(id) {
            continue;
        }
        let d = widgets::dialog(app, id);
        d.present(Some(&win));
        app.ui.borrow_mut().as_mut().map(|u| u.dialogs.insert(id.clone(), d));
    }
    match busy {
        Some(b) => {
            let label = b.str_of("label").unwrap_or_else(|| "Working…".into());
            let detail = b.str_of("detail").map(|d| format!(" · {d}")).unwrap_or_default();
            banner.set_title(&format!("{label}{detail}"));
            banner.set_revealed(true);
        }
        None => banner.set_revealed(false),
    }
    for t in toasts {
        let new = app.ui.borrow_mut().as_mut().map(|u| u.shown_toasts.insert(t.id.clone())).unwrap_or(false);
        if !new {
            continue;
        }
        let toast = adw::Toast::new(&t.str("text").unwrap_or_default());
        toast.set_timeout(t.f64("timeout").unwrap_or(3.0).clamp(0.0, 60.0) as u32);
        let w = Rc::downgrade(app);
        let id = t.id.clone();
        toast.connect_dismissed(move |_| {
            if let Some(a) = w.upgrade() {
                let m = a.core.borrow_mut().client.simple("dismiss", Some(&id));
                send(&a, m);
            }
        });
        overlay.add_toast(toast);
    }
    // In a match the engine shows its own window: the loader steps aside.
    crate::backend::context_changed(app, &context);
}

fn new_page(app: &App, id: &str) -> adw::NavigationPage {
    let page = adw::NavigationPage::builder().tag(id).title("").can_pop(false).build();
    fill_page(app, &page, id);
    page
}

/// (Re)builds a page's content from its screen node.
pub fn fill_page(app: &App, page: &adw::NavigationPage, id: &str) {
    let core = app.core.borrow();
    let Some(screen) = core.store.node(id) else { return };
    page.set_title(&screen.title());
    app.quiet.set(true);
    let content = widgets::screen(app, &core.store, screen);
    app.quiet.set(false);
    page.set_child(Some(&content));
}

/// Rebuilds a screen whose structure changed, keeping its scroll position.
pub fn refresh_screen(app: &App, id: &str) {
    let page = app.ui.borrow().as_ref().and_then(|u| u.pages.get(id).cloned());
    let Some(page) = page else { return };
    let scroll = page.child().and_then(|c| find_scrolled(&c)).map(|s| s.vadjustment().value());
    fill_page(app, &page, id);
    if let (Some(v), Some(s)) = (scroll, page.child().and_then(|c| find_scrolled(&c))) {
        glib::idle_add_local_once(move || s.vadjustment().set_value(v));
    }
}

fn find_scrolled(w: &gtk::Widget) -> Option<gtk::ScrolledWindow> {
    if let Some(s) = w.downcast_ref::<gtk::ScrolledWindow>() {
        return Some(s.clone());
    }
    let mut c = w.first_child();
    while let Some(x) = c {
        if let Some(s) = find_scrolled(&x) {
            return Some(s);
        }
        c = x.next_sibling();
    }
    None
}

/// The window, with the native skeleton shown at once (banner and placeholders) until the first snapshot.
pub fn build_window(app: &App, gapp: &adw::Application, size: Option<(i32, i32)>) -> adw::ApplicationWindow {
    let win = adw::ApplicationWindow::builder().application(gapp).title("Godnotic").icon_name("io.github.dixonSolutions.XonoticTouch").build();
    let (w, h) = size.unwrap_or((1100, 760));
    win.set_default_size(w, h);
    win.set_size_request(360, 480);
    let nav = adw::NavigationView::new();
    nav.set_pop_on_escape(false);
    let banner = adw::Banner::new("");
    let toasts = adw::ToastOverlay::new();
    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    vbox.append(&banner);
    nav.set_vexpand(true);
    vbox.append(&nav);
    toasts.set_child(Some(&vbox));
    win.set_content(Some(&toasts));

    // Escape / system back: the engine decides what back means.
    let keys = gtk::EventControllerKey::new();
    let wk = Rc::downgrade(app);
    keys.connect_key_pressed(move |_, key, _, _| {
        if key == gdk::Key::Escape {
            if let Some(a) = wk.upgrade() {
                back(&a);
                return glib::Propagation::Stop;
            }
        }
        glib::Propagation::Proceed
    });
    win.add_controller(keys);

    let bp = adw::Breakpoint::new(adw::BreakpointCondition::parse("max-width: 600sp").expect("condition"));
    let (wa, wu) = (Rc::downgrade(app), Rc::downgrade(app));
    let (win_a, win_u) = (win.clone(), win.clone());
    bp.connect_apply(move |_| {
        win_a.add_css_class("gn-narrow");
        if let Some(a) = wa.upgrade() {
            set_narrow(&a, true);
        }
    });
    bp.connect_unapply(move |_| {
        win_u.remove_css_class("gn-narrow");
        if let Some(a) = wu.upgrade() {
            set_narrow(&a, false);
        }
    });
    win.add_breakpoint(bp);

    let skeleton = widgets::skeleton();
    nav.add(&skeleton);
    *app.ui.borrow_mut() = Some(Ui {
        win: win.clone(), nav, toasts, banner, pages: HashMap::new(), shown_stack: vec![], dialogs: HashMap::new(),
        updaters: HashMap::new(), activators: HashMap::new(), lists: HashMap::new(), shown_toasts: HashSet::new(), skeleton: true,
    });
    win
}

fn set_narrow(app: &App, narrow: bool) {
    if app.narrow.replace(narrow) == narrow {
        return;
    }
    // Columns of lower priority come and go; rows become two-line.
    let ids: Vec<String> = app.ui.borrow().as_ref().map(|u| u.shown_stack.clone()).unwrap_or_default();
    for id in ids {
        refresh_screen(app, &id);
    }
}

pub fn back(app: &App) {
    let m = app.core.borrow_mut().client.simple("back", None);
    send(app, m);
    crate::backend::fixture_back(app);
}

/// Invokes an action of a node: confirm, link, copy, file and back kinds are handled natively first.
pub fn invoke(app: &App, node: &str, action: &Map<String, Value>) {
    let id = action.str_of("id").unwrap_or_default();
    let kind = action.str_of("kind").unwrap_or_else(|| "normal".into());
    let Some(win) = app.ui.borrow().as_ref().map(|u| u.win.clone()) else { return };
    let w: Weak<Shared> = Rc::downgrade(app);
    let node = node.to_string();
    let go = move |payload: Option<Value>| {
        if let Some(a) = w.upgrade() {
            let m = a.core.borrow_mut().client.action(&node, &id, payload);
            send(&a, m);
            crate::backend::fixture_route(&a, &id);
        }
    };
    match kind.as_str() {
        "back" => back(app),
        "copy" | "share" => {
            if let Some(t) = action.str_of("text") {
                win.clipboard().set_text(&t);
                if let Some(u) = app.ui.borrow().as_ref() {
                    u.toasts.add_toast(adw::Toast::new("Copied"));
                }
            }
            go(None);
        }
        "link" => {
            let Some(url) = action.str_of("url").filter(|u| u.starts_with("https://") || u.starts_with("http://")) else { return };
            let host = url.split('/').nth(2).unwrap_or("").to_string();
            let d = adw::AlertDialog::new(Some("Open in the browser?"), Some(&host));
            d.add_response("cancel", "Cancel");
            d.add_response("open", "Open");
            d.set_response_appearance("open", adw::ResponseAppearance::Suggested);
            let win2 = win.clone();
            d.choose(Some(&win), gio::Cancellable::NONE, move |r| {
                if r == "open" {
                    gtk::UriLauncher::new(&url).launch(Some(&win2), gio::Cancellable::NONE, |_| {});
                    go(None);
                }
            });
        }
        "file" => {
            let dlg = gtk::FileDialog::builder().title(action.str_of("label").unwrap_or_else(|| "Choose a file".into())).build();
            dlg.open(Some(&win), gio::Cancellable::NONE, move |r| {
                if let Ok(f) = r {
                    if let Some(p) = f.path() {
                        go(Some(json!({"path": p.to_string_lossy()})));
                    }
                }
            });
        }
        _ => match action.get("confirm").and_then(Value::as_object) {
            Some(c) => {
                let d = adw::AlertDialog::new(c.str_of("title").as_deref(), c.str_of("body").as_deref());
                d.add_response("cancel", "Cancel");
                d.add_response("ok", &c.str_of("ok").unwrap_or_else(|| "OK".into()));
                d.set_response_appearance("ok", if c.bool_of("destructive").unwrap_or(false) { adw::ResponseAppearance::Destructive } else { adw::ResponseAppearance::Suggested });
                d.choose(Some(&win), gio::Cancellable::NONE, move |r| {
                    if r == "ok" {
                        go(None);
                    }
                });
            }
            None => go(None),
        },
    }
}

/// A list row was tapped.
pub fn row_activated(app: &App, list_node: &str, source: &str, pos: usize) {
    let key = app.core.borrow().store.sources.get(source).and_then(|w| w.row(pos)).and_then(|r| r.str_of("key"));
    if let Some(k) = key {
        // A list with `rowSelect`: the first click chooses the row (row/select); a second
        // click on the same row within the double-click time plays it (row/activate).
        let selects = app.core.borrow().store.node(list_node).is_some_and(|n| n.obj("rowSelect").is_some());
        if selects && app.clicks.borrow_mut().click(list_node, &k, std::time::Instant::now()) == "select" {
            let m = app.core.borrow_mut().client.row(list_node, &k, "select");
            send(app, m);
            return;
        }
        let m = app.core.borrow_mut().client.row(list_node, &k, "activate");
        send(app, m);
        let primary = app.core.borrow().store.node(list_node).and_then(|n| n.obj("rowPrimary").and_then(|p| p.str_of("id")));
        if let Some(p) = primary {
            crate::backend::fixture_route(app, &p);
        }
    }
}

/// A value event from a control (optimistic: the widget already shows it).
pub fn value(app: &App, node: &str, v: Value) {
    if app.quiet.get() {
        return;
    }
    let m = app.core.borrow_mut().client.value(node, v);
    send(app, m);
}

pub fn index(item: &glib::Object) -> usize {
    index_of(item).unwrap_or(0)
}
