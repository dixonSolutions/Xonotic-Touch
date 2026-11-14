// SPDX-License-Identifier: GPL-3.0-or-later
//! Node types to libadwaita widgets (docs/UI_BRIDGE.md section 4).
//!
//! A screen is an `AdwToolbarView`: header bar (title, back, the screen's
//! actions) and the `toolbar` child as sticky top bars, the `actionbar` child
//! (or the screen's `primary`) as the sticky bottom bar, and one scrolling
//! region between them. A `list` is its own virtualised scrolling region
//! (`GtkColumnView` for tables, `GtkGridView` for cards). Unknown types
//! become a generic row, or their children, or nothing.

use crate::app::{self, invoke, value, want_image, want_rows, App, ListView};
use crate::lists::WindowModel;
use adw::prelude::*;
use godnotic_core::bridge::{adwaita_icon, Node, Props, Store};
use gtk::glib;
use serde_json::{json, Map, Value};
use std::cell::{Cell, RefCell};
use std::rc::Rc;

fn icon(name: &str) -> Option<&'static str> {
    adwaita_icon(name)
}

fn label(text: &str, classes: &[&str]) -> gtk::Label {
    let l = gtk::Label::new(Some(text));
    l.set_wrap(true);
    l.set_max_width_chars(24);
    l.set_xalign(0.0);
    for c in classes {
        l.add_css_class(c);
    }
    l
}

fn register_updater(app: &App, id: &str, f: Rc<dyn Fn(&Node)>) {
    if let Some(u) = app.ui.borrow_mut().as_mut() {
        u.updaters.insert(id.to_string(), f);
    }
}

fn register_activator(app: &App, id: &str, w: &impl IsA<gtk::Widget>) {
    if let Some(u) = app.ui.borrow_mut().as_mut() {
        u.activators.insert(id.to_string(), w.clone().upcast());
    }
}

/// A picture for an image ref, asked for at about `w`×`h` pixels.
pub fn picture(app: &App, r: &str, w: u32, h: u32) -> gtk::Picture {
    let p = gtk::Picture::new();
    p.set_content_fit(gtk::ContentFit::Cover);
    p.set_can_shrink(true);
    if app.images.borrow_mut().attach(r, &p) {
        want_image(app, r, w, h);
    }
    p
}

fn aspect_height(aspect: Option<String>, width: i32) -> i32 {
    let (a, b) = aspect.as_deref().and_then(|s| s.split_once(':')).and_then(|(a, b)| Some((a.parse::<f64>().ok()?, b.parse::<f64>().ok()?))).unwrap_or((16.0, 9.0));
    (width as f64 * b / a) as i32
}

fn action_button(app: &App, node: &str, a: &Map<String, Value>, primary: bool) -> gtk::Button {
    let b = gtk::Button::new();
    let text = a.str_of("label").unwrap_or_default();
    match a.str_of("icon").and_then(|i| icon(&i)) {
        Some(i) if !text.is_empty() => {
            let c = adw::ButtonContent::new();
            c.set_label(&text);
            c.set_icon_name(i);
            b.set_child(Some(&c));
        }
        Some(i) => {
            b.set_icon_name(i);
            b.set_tooltip_text(a.str_of("a11y").as_deref());
        }
        None => b.set_label(&text),
    }
    match a.str_of("style").as_deref() {
        Some("primary") => b.add_css_class(if primary { "gn-play" } else { "suggested-action" }),
        Some("destructive") => b.add_css_class("destructive-action"),
        Some("link") => b.add_css_class("flat"),
        _ if primary => b.add_css_class("gn-play"),
        _ => {}
    }
    b.set_sensitive(a.bool_of("enabled").unwrap_or(true));
    let w = Rc::downgrade(app);
    let owner = node.to_string();
    let a2 = a.clone();
    b.connect_clicked(move |_| {
        if let Some(app) = w.upgrade() {
            invoke(&app, &owner, &a2);
        }
    });
    if let Some(id) = a.str_of("id") {
        register_activator(app, &format!("{node}:{id}"), &b);
    }
    b
}

/// The Home banner (`image` with `role: "banner"`): large, about a quarter of a normal window.
const BANNER_HEIGHT: i32 = 240;
/// ... and the least it shrinks to on Home, where it is sized from the window's width.
const BANNER_MIN_HEIGHT: i32 = 80;

/// The native skeleton shown before the engine's first snapshot: the banner and placeholder cards.
pub fn skeleton() -> adw::NavigationPage {
    let tv = adw::ToolbarView::new();
    tv.add_top_bar(&adw::HeaderBar::new());
    let b = gtk::Box::new(gtk::Orientation::Vertical, 18);
    b.set_margin_top(12);
    b.set_margin_bottom(24);
    b.set_margin_start(16);
    b.set_margin_end(16);
    let banner = gtk::Picture::for_paintable(&gtk::gdk::Texture::from_bytes(&glib::Bytes::from_static(include_bytes!("../data/banner-800.png"))).expect("banner"));
    banner.set_content_fit(gtk::ContentFit::Contain);
    banner.set_height_request(BANNER_HEIGHT);
    banner.add_css_class("gn-banner");
    b.append(&banner);
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
    row.set_homogeneous(true);
    for _ in 0..3 {
        let c = gtk::Box::new(gtk::Orientation::Vertical, 0);
        c.set_height_request(300);
        c.add_css_class("card");
        c.add_css_class("gn-placeholder");
        row.append(&c);
    }
    b.append(&row);
    let clamp = adw::Clamp::builder().maximum_size(1100).child(&b).build();
    let sw = gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).child(&clamp).vexpand(true).build();
    tv.set_content(Some(&sw));
    adw::NavigationPage::builder().tag("skeleton").title("Godnotic").child(&tv).can_pop(false).build()
}

/// One screen: sticky header, toolbar and action bar around one scrolling region.
pub fn screen(app: &App, store: &Store, s: &Node) -> gtk::Widget {
    let tv = adw::ToolbarView::new();
    tv.set_top_bar_style(adw::ToolbarStyle::Raised);
    let header = adw::HeaderBar::new();
    header.set_show_back_button(false);
    let title = adw::WindowTitle::new(&s.title(), &s.str("subtitle").unwrap_or_default());
    header.set_title_widget(Some(&title));
    let in_stack = store.stack().iter().position(|x| *x == s.id);
    let depth = in_stack.unwrap_or(0);
    // Dialogs and sheets close with their own button; pages get Back.
    if in_stack.is_some_and(|d| d > 0 || store.app().and_then(|a| a.bool("canBack")).unwrap_or(false)) {
        let back = gtk::Button::from_icon_name("go-previous-symbolic");
        back.set_tooltip_text(Some("Back"));
        let w = Rc::downgrade(app);
        back.connect_clicked(move |_| {
            if let Some(a) = w.upgrade() {
                app::back(&a);
            }
        });
        register_activator(app, &format!("{}:back", s.id), &back);
        header.pack_start(&back);
    }
    for a in s.arr("actions").iter().rev() {
        if let Some(a) = a.as_object() {
            let b = action_button(app, &s.id, a, false);
            if let Some(i) = a.str_of("icon").and_then(|i| icon(&i)) {
                b.add_css_class("flat");
                if app.narrow.get() {
                    // a phone-width header has room for icons, not for three labelled buttons
                    b.set_child(Some(&gtk::Image::from_icon_name(i)));
                    b.set_tooltip_text(a.str_of("label").as_deref());
                }
            }
            header.pack_end(&b);
        }
    }
    if depth == 0 {
        header.pack_end(&crate::data_ui::button());
    }
    tv.add_top_bar(&header);
    if let Some(from) = s.from_mod() {
        let l = label(&format!("From the mod {from}"), &["gn-from-mod"]);
        l.set_margin_start(12);
        tv.add_top_bar(&l);
    }

    let kids = store.children(&s.id);
    let mut body: Vec<&Node> = Vec::new();
    let mut tabs: Option<&Node> = None;
    for k in &kids {
        if k.hidden() {
            continue;
        }
        match k.kind.as_str() {
            "toolbar" => {
                let t = container(app, store, k, gtk::Orientation::Vertical);
                t.add_css_class("gn-toolbar");
                tv.add_top_bar(&t);
            }
            "actionbar" => tv.add_bottom_bar(&actionbar(app, store, k)),
            "tabbar" => tabs = Some(k),
            _ => body.push(k),
        }
    }
    if !kids.iter().any(|k| k.kind == "actionbar") {
        if let Some(p) = s.obj("primary") {
            let bar = gtk::CenterBox::new();
            bar.add_css_class("gn-actionbar");
            bar.set_center_widget(Some(&action_button(app, &s.id, p, true)));
            tv.add_bottom_bar(&bar);
        }
    }
    // Home's bar of tabs: the last bottom bar, under the action bar.
    if let Some(t) = tabs {
        tv.add_bottom_bar(&tabbar(app, store, t));
    }

    // One scrolling region: a list that fills the screen scrolls by itself; everything else in one scrolled column.
    let big_list = body.iter().position(|n| (n.kind == "list" && body.len() <= 2) || (n.kind == "split" && body.len() == 1));
    let content: gtk::Widget = match big_list {
        Some(i) => {
            let v = gtk::Box::new(gtk::Orientation::Vertical, 0);
            for (j, n) in body.iter().enumerate() {
                let w = node(app, store, n);
                if j == i {
                    w.set_vexpand(true);
                } else {
                    w.set_margin_start(12);
                    w.set_margin_end(12);
                    w.set_margin_top(8);
                }
                v.append(&w);
            }
            v.upcast()
        }
        None => {
            let col = gtk::Box::new(gtk::Orientation::Vertical, 18);
            col.set_margin_top(12);
            col.set_margin_bottom(24);
            col.set_margin_start(16);
            col.set_margin_end(16);
            for n in &body {
                col.append(&node(app, store, n));
            }
            if body.is_empty() {
                col.append(&adw::StatusPage::builder().title(s.title()).icon_name("dialog-information-symbolic").build());
            }
            let clamp = adw::Clamp::builder().maximum_size(1100).tightening_threshold(900).child(&col).build();
            gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).kinetic_scrolling(true).child(&clamp).vexpand(true).build().upcast()
        }
    };
    tv.set_content(Some(&content));
    tv.upcast()
}

fn actionbar(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let b = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    b.add_css_class("gn-actionbar");
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        let w = match k.kind.as_str() {
            "field" => {
                let e = entry(app, k);
                e.set_hexpand(true);
                e.upcast()
            }
            _ => node(app, store, k),
        };
        b.append(&w);
    }
    let wrap = adw::Clamp::builder().maximum_size(1100).child(&b).build();
    wrap.upcast()
}

/// A plain text entry (toolbar search, the address field in the action bar).
fn entry(app: &App, n: &Node) -> gtk::Widget {
    let private = n.bool("private").unwrap_or(false) || n.str("kind").as_deref() == Some("password");
    let e: gtk::Widget = if n.str("kind").as_deref() == Some("search") {
        let s = gtk::SearchEntry::new();
        s.set_placeholder_text(n.str("placeholder").as_deref());
        s.set_text(&n.str("value").unwrap_or_default());
        let w = Rc::downgrade(app);
        let id = n.id.clone();
        s.connect_search_changed(move |s| {
            if let Some(a) = w.upgrade() {
                value(&a, &id, json!(s.text().to_string()));
                filter_lists(&a, &s.text());
            }
        });
        s.upcast()
    } else if private {
        let p = gtk::PasswordEntry::new();
        p.set_show_peek_icon(true);
        p.set_placeholder_text(n.str("placeholder").as_deref());
        p.upcast()
    } else {
        let t = gtk::Entry::new();
        t.set_placeholder_text(n.str("placeholder").as_deref());
        t.set_text(&n.str("value").unwrap_or_default());
        let w = Rc::downgrade(app);
        let id = n.id.clone();
        let has_submit = n.obj("submit").is_some();
        t.connect_changed(move |t| {
            if !has_submit {
                if let Some(a) = w.upgrade() {
                    value(&a, &id, json!(t.text().to_string()));
                }
            }
        });
        t.upcast()
    };
    e.set_size_request(-1, 44);
    register_activator(app, &n.id, &e);
    if let Some(sub) = n.obj("submit").cloned() {
        let w = Rc::downgrade(app);
        let id = n.id.clone();
        if let Some(t) = e.downcast_ref::<gtk::Entry>() {
            t.connect_activate(move |t| {
                if let Some(a) = w.upgrade() {
                    let m = a.core.borrow_mut().client.submit(&id, json!(t.text().to_string()));
                    app::send(&a, m);
                    invoke(&a, &id, &sub);
                }
            });
        }
    }
    e
}

/// The search field also filters the lists on screen (sent with their `window` requests).
fn filter_lists(app: &App, text: &str) {
    let lists: Vec<_> = app.ui.borrow().as_ref().map(|u| u.lists.values().cloned().collect()).unwrap_or_default();
    for l in lists {
        l.borrow_mut().filter = text.to_string();
        let src = l.borrow().source.clone();
        if let Some(w) = app.core.borrow_mut().store.sources.get_mut(&src) {
            w.invalidate();
        }
        want_rows(app, &l);
    }
}

fn container(app: &App, store: &Store, n: &Node, o: gtk::Orientation) -> gtk::Box {
    let b = gtk::Box::new(o, 8);
    for k in store.children(&n.id) {
        if !k.hidden() {
            b.append(&node(app, store, k));
        }
    }
    b
}

/// Builds the widget for one node.
pub fn node(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let w: gtk::Widget = match n.kind.as_str() {
        "section" | "form" => section(app, store, n),
        "hero" => hero(app, n),
        "card" => card(app, n).upcast(),
        "grid" => grid(app, store, n),
        "row" => row(app, store, n),
        "toolbar" => container(app, store, n, gtk::Orientation::Vertical).upcast(),
        "list" => list(app, store, n),
        "preview" => preview(app, n),
        "text" => text(n),
        "image" => {
            let r = n.str("ref").unwrap_or_default();
            let p = picture(app, &r, 960, 400);
            if n.str("fit").as_deref() == Some("contain") {
                p.set_content_fit(gtk::ContentFit::Contain);
            }
            let banner = n.str("role").as_deref() == Some("banner") || r == "asset:banner";
            if banner {
                // Home's banner, at the top: it takes the height the window has above the row of
                // cards (never under the minimum), so it scales with the window.
                p.set_height_request(BANNER_MIN_HEIGHT);
                p.set_vexpand(true);
            } else {
                p.set_height_request(220);
            }
            p.set_alternative_text(n.str("alt").as_deref());
            p.add_css_class("gn-banner");
            p.upcast()
        }
        "divider" => gtk::Separator::new(gtk::Orientation::Horizontal).upcast(),
        "status" => status(app, n),
        "progress" => progress(app, n),
        "tabs" => tabs(app, store, n),
        "split" => split(app, store, n),
        "field" => entry(app, n),
        "button" => {
            let mut a = n.obj("action").cloned().unwrap_or_default();
            if a.str_of("label").is_none() {
                a.insert("label".into(), json!(n.title()));
            }
            if let Some(s) = n.str("style") {
                a.insert("style".into(), json!(s));
            }
            let b = action_button(app, &n.id, &a, false);
            register_activator(app, &n.id, &b);
            b.set_halign(gtk::Align::Start);
            b.set_size_request(-1, 48);
            b.set_sensitive(n.enabled());
            let bw = b.downgrade();
            register_updater(app, &n.id, Rc::new(move |n: &Node| {
                if let Some(b) = bw.upgrade() {
                    b.set_sensitive(n.enabled());
                }
            }));
            b.upcast()
        }
        "switch" if n.str("style").as_deref() == Some("checkbox") => checkbox(app, n),
        "switch" | "slider" | "stepper" | "choice" | "colour" => {
            let g = adw::PreferencesGroup::new();
            g.add(&control_row(app, n));
            g.upcast()
        }
        _ => generic(app, store, n),
    };
    if let Some(h) = n.str("help") {
        if w.tooltip_text().is_none() {
            w.set_tooltip_text(Some(&h));
        }
    }
    w
}

/// Unknown types: a row with title and actions when it has a title, else its children, else nothing.
fn generic(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    if !n.title().is_empty() {
        let r = adw::ActionRow::builder().title(glib::markup_escape_text(&n.title())).subtitle(glib::markup_escape_text(&n.str("subtitle").unwrap_or_default())).build();
        for a in n.arr("actions") {
            if let Some(a) = a.as_object() {
                let b = action_button(app, &n.id, a, false);
                b.set_valign(gtk::Align::Center);
                r.add_suffix(&b);
            }
        }
        let g = adw::PreferencesGroup::new();
        g.add(&r);
        return g.upcast();
    }
    container(app, store, n, gtk::Orientation::Vertical).upcast()
}

fn section(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let g = adw::PreferencesGroup::new();
    if !n.title().is_empty() {
        g.set_title(&glib::markup_escape_text(&n.title()));
    }
    if let Some(s) = n.str("subtitle") {
        g.set_description(Some(&glib::markup_escape_text(&s)));
    }
    if let Some(l) = n.obj("link") {
        let b = action_button(app, &n.id, l, false);
        b.add_css_class("flat");
        g.set_header_suffix(Some(&b));
    }
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        let w: gtk::Widget = match k.kind.as_str() {
            "switch" if k.str("style").as_deref() != Some("checkbox") => control_row(app, k),
            "slider" | "stepper" | "choice" | "colour" => control_row(app, k),
            "field" => field_row(app, k),
            "button" if k.str("style").as_deref() != Some("primary") => button_row(app, k),
            _ => {
                let w = node(app, store, k);
                w.set_margin_top(6);
                w
            }
        };
        g.add(&w);
    }
    if let Some(f) = n.str("footer") {
        let l = label(&f, &["dim-label", "caption"]);
        l.set_margin_top(6);
        g.add(&l);
    }
    g.upcast()
}

fn button_row(app: &App, n: &Node) -> gtk::Widget {
    let r = adw::ActionRow::builder().title(glib::markup_escape_text(&n.title())).activatable(true).build();
    if let Some(s) = n.str("subtitle").or_else(|| n.str("help")) {
        r.set_subtitle(&glib::markup_escape_text(&s));
    }
    if let Some(i) = n.obj("action").and_then(|a| a.str_of("icon")).or_else(|| n.str("icon")).and_then(|i| icon(&i)) {
        r.add_prefix(&gtk::Image::from_icon_name(i));
    }
    r.add_suffix(&gtk::Image::from_icon_name("go-next-symbolic"));
    let a = n.obj("action").cloned().unwrap_or_default();
    let w = Rc::downgrade(app);
    let id = n.id.clone();
    r.connect_activated(move |_| {
        if let Some(app) = w.upgrade() {
            invoke(&app, &id, &a);
        }
    });
    register_activator(app, &n.id, &r);
    r.upcast()
}

fn field_row(app: &App, n: &Node) -> gtk::Widget {
    let private = n.bool("private").unwrap_or(false) || n.str("kind").as_deref() == Some("password");
    let r: adw::EntryRow = if private { adw::PasswordEntryRow::new().upcast() } else { adw::EntryRow::new() };
    r.set_title(&glib::markup_escape_text(&n.title()));
    if !private {
        r.set_text(&n.str("value").unwrap_or_default());
    }
    r.set_sensitive(n.enabled());
    let w = Rc::downgrade(app);
    let id = n.id.clone();
    let submit = n.obj("submit").cloned();
    if submit.is_none() && !private {
        r.connect_changed(move |e| {
            if let Some(a) = w.upgrade() {
                value(&a, &id, json!(e.text().to_string()));
            }
        });
    } else {
        r.set_show_apply_button(!private);
        let send_it = move |e: &adw::EntryRow| {
            if let Some(a) = w.upgrade() {
                let m = a.core.borrow_mut().client.submit(&id, json!(e.text().to_string()));
                app::send(&a, m);
                // A private value is write-only: the field forgets it once sent.
                if private {
                    e.set_text("");
                }
            }
        };
        let s2 = send_it.clone();
        r.connect_entry_activated(move |e| s2(e));
        r.connect_apply(move |e| send_it(e));
    }
    if let Some(err) = n.str("error") {
        r.add_css_class("error");
        r.set_tooltip_text(Some(&err));
    }
    // A password row shows whether one is set: closed lock when it is, open lock when none applies.
    if private {
        let lock = gtk::Image::from_icon_name(if n.bool("hasValue").unwrap_or(false) { "changes-prevent-symbolic" } else { "changes-allow-symbolic" });
        r.add_prefix(&lock);
        let l2 = lock.clone();
        register_updater(app, &n.id, Rc::new(move |n: &Node| {
            l2.set_icon_name(Some(if n.bool("hasValue").unwrap_or(false) { "changes-prevent-symbolic" } else { "changes-allow-symbolic" }));
        }));
    }
    r.upcast()
}

fn checkbox(app: &App, n: &Node) -> gtk::Widget {
    let c = gtk::CheckButton::with_label(&n.title());
    c.set_active(n.bool("value").unwrap_or(false));
    c.set_sensitive(n.enabled());
    let w = Rc::downgrade(app);
    let id = n.id.clone();
    c.connect_toggled(move |c| {
        if let Some(a) = w.upgrade() {
            value(&a, &id, json!(c.is_active()));
        }
    });
    let c2 = c.clone();
    register_updater(app, &n.id, Rc::new(move |n: &Node| {
        c2.set_active(n.bool("value").unwrap_or(false));
        c2.set_sensitive(n.enabled());
    }));
    register_activator(app, &n.id, &c);
    c.upcast()
}

fn apply_note(n: &Node) -> Option<&'static str> {
    match n.str("apply").as_deref() {
        Some("video") => Some("Applies when you leave this screen"),
        Some("sound") => Some("Restarts the sound"),
        Some("reconnect") => Some("Applies from the next match"),
        Some("restart") => Some("Applies after a game restart"),
        _ => None,
    }
}

fn subtitle_of(n: &Node) -> String {
    let mut s = n.str("subtitle").or_else(|| n.str("help")).unwrap_or_default();
    if let Some(note) = apply_note(n) {
        s = if s.is_empty() { note.into() } else { format!("{s} · {note}") };
    }
    if let Some(e) = n.str("error") {
        s = e;
    }
    s
}

fn fmt_value(n: &Node, v: f64) -> String {
    let unit = n.str("unit").map(|u| format!(" {u}")).unwrap_or_default();
    match n.str("format").as_deref() {
        Some("percent") => format!("{:.0} %", v * 100.0),
        Some("float1") => format!("{v:.1}{unit}"),
        Some("float2") => format!("{v:.2}{unit}"),
        Some("ms") => format!("{v:.0} ms"),
        Some("deg") => format!("{v:.0}°"),
        Some("x") => format!("{v:.2}×"),
        _ => format!("{v:.0}{unit}"),
    }
}

/// Form controls as libadwaita rows: switch, slider, stepper, choice.
fn control_row(app: &App, n: &Node) -> gtk::Widget {
    let title = glib::markup_escape_text(&n.title()).to_string();
    let sub = glib::markup_escape_text(&subtitle_of(n)).to_string();
    let id = n.id.clone();
    let w = Rc::downgrade(app);
    let r: gtk::Widget = match n.kind.as_str() {
        "switch" => {
            let r = adw::SwitchRow::builder().title(&title).subtitle(&sub).active(n.bool("value").unwrap_or(false)).build();
            r.connect_active_notify(move |r| {
                if let Some(a) = w.upgrade() {
                    value(&a, &id, json!(r.is_active()));
                }
            });
            let r2 = r.clone();
            register_updater(app, &n.id, Rc::new(move |n: &Node| {
                r2.set_active(n.bool("value").unwrap_or(false));
                r2.set_sensitive(n.enabled());
                r2.set_subtitle(&glib::markup_escape_text(&subtitle_of(n)));
            }));
            register_activator(app, &n.id, &r);
            r.upcast()
        }
        "stepper" => {
            let (min, max, step) = (n.f64("min").unwrap_or(0.0), n.f64("max").unwrap_or(100.0), n.f64("step").unwrap_or(1.0));
            let r = adw::SpinRow::with_range(min, max, step.max(0.001));
            r.set_title(&title);
            r.set_subtitle(&sub);
            r.set_value(n.f64("value").unwrap_or(min));
            r.connect_value_notify(move |r| {
                if let Some(a) = w.upgrade() {
                    value(&a, &id, json!(r.value()));
                }
            });
            let r2 = r.clone();
            register_updater(app, &n.id, Rc::new(move |n: &Node| r2.set_value(n.f64("value").unwrap_or(0.0))));
            r.upcast()
        }
        "slider" => {
            let (min, max, step) = (n.f64("min").unwrap_or(0.0), n.f64("max").unwrap_or(1.0), n.f64("step").unwrap_or(0.01));
            let r = adw::ActionRow::builder().title(&title).subtitle(&sub).build();
            let scale = gtk::Scale::with_range(gtk::Orientation::Horizontal, min, max, step.max(0.0001));
            scale.set_value(n.f64("value").unwrap_or(min));
            scale.set_draw_value(false);
            scale.set_hexpand(true);
            scale.set_size_request(200, -1);
            for m in n.arr("marks") {
                if let Some(v) = m.get("value").and_then(Value::as_f64) {
                    scale.add_mark(v, gtk::PositionType::Bottom, m.get("label").and_then(Value::as_str));
                }
            }
            let shown = gtk::Label::new(Some(&fmt_value(n, scale.value())));
            shown.add_css_class("numeric");
            shown.set_width_chars(7);
            let n2 = n.clone();
            let s2 = shown.clone();
            // Throttled to 10 Hz while dragging; the last value always goes.
            let last = Rc::new(Cell::new(0i64));
            let pending: Rc<RefCell<Option<glib::SourceId>>> = Rc::new(RefCell::new(None));
            scale.connect_value_changed(move |sc| {
                s2.set_text(&fmt_value(&n2, sc.value()));
                let now = glib::monotonic_time();
                let (w, id) = (w.clone(), id.clone());
                if now - last.get() >= 100_000 {
                    last.set(now);
                    if let Some(a) = w.upgrade() {
                        value(&a, &id, json!(sc.value()));
                    }
                } else if pending.borrow().is_none() {
                    let p2 = pending.clone();
                    let sc2 = sc.clone();
                    *pending.borrow_mut() = Some(glib::timeout_add_local_once(std::time::Duration::from_millis(110), move || {
                        p2.borrow_mut().take();
                        if let Some(a) = w.upgrade() {
                            value(&a, &id, json!(sc2.value()));
                        }
                    }));
                }
            });
            r.add_suffix(&scale);
            r.add_suffix(&shown);
            let sc3 = scale.clone();
            register_updater(app, &n.id, Rc::new(move |n: &Node| sc3.set_value(n.f64("value").unwrap_or(0.0))));
            r.upcast()
        }
        "choice" => {
            let opts: Vec<(String, String)> = n.arr("options").iter().filter_map(|o| Some((o.get("value")?.as_str()?.to_string(), o.get("label").and_then(Value::as_str).unwrap_or("").to_string()))).collect();
            let cur = n.str("value").unwrap_or_default();
            let style = n.str("style").unwrap_or_default();
            // Segmented on wide windows; a drop-down when the segments would not fit (narrow, many options).
            let fits = !app.narrow.get() || opts.len() <= 3;
            if (style == "segmented" || (style.is_empty() && opts.len() <= 3)) && !opts.is_empty() && fits {
                let r = adw::ActionRow::builder().title(&title).subtitle(&sub).build();
                let g = adw::ToggleGroup::new();
                g.set_valign(gtk::Align::Center);
                for (v, l) in &opts {
                    g.add(adw::Toggle::builder().name(v.as_str()).label(l.as_str()).build());
                }
                g.set_active_name(Some(&cur));
                g.connect_active_name_notify(move |g| {
                    if let (Some(a), Some(v)) = (w.upgrade(), g.active_name()) {
                        value(&a, &id, json!(v.to_string()));
                    }
                });
                r.add_suffix(&g);
                let g2 = g.clone();
                register_updater(app, &n.id, Rc::new(move |n: &Node| g2.set_active_name(n.str("value").as_deref())));
                r.upcast()
            } else {
                let labels: Vec<&str> = opts.iter().map(|(_, l)| l.as_str()).collect();
                let r = adw::ComboRow::builder().title(&title).subtitle(&sub).model(&gtk::StringList::new(&labels)).build();
                if let Some(i) = opts.iter().position(|(v, _)| *v == cur) {
                    r.set_selected(i as u32);
                }
                let o2 = opts.clone();
                r.connect_selected_notify(move |r| {
                    if let (Some(a), Some((v, _))) = (w.upgrade(), o2.get(r.selected() as usize)) {
                        value(&a, &id, json!(v));
                    }
                });
                let r2 = r.clone();
                register_updater(app, &n.id, Rc::new(move |n: &Node| {
                    let v = n.str("value").unwrap_or_default();
                    if let Some(i) = opts.iter().position(|(x, _)| *x == v) {
                        r2.set_selected(i as u32);
                    }
                }));
                r.upcast()
            }
        }
        _ => adw::ActionRow::builder().title(&title).subtitle(&sub).build().upcast(),
    };
    r.set_sensitive(n.enabled());
    r
}

fn chips(n: &Node, key: &str, class: &str) -> Option<gtk::Box> {
    let items = n.arr(key);
    if items.is_empty() {
        return None;
    }
    let b = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    for c in items {
        let Some(c) = c.as_object() else { continue };
        let chip = gtk::Box::new(gtk::Orientation::Horizontal, 4);
        chip.add_css_class(class);
        if let Some(t) = c.str_of("tone") {
            chip.add_css_class(&t);
        }
        if let Some(i) = c.str_of("icon").and_then(|i| icon(&i)) {
            let im = gtk::Image::from_icon_name(i);
            im.set_pixel_size(12);
            chip.append(&im);
        }
        chip.append(&gtk::Label::new(c.str_of("text").as_deref()));
        b.append(&chip);
    }
    Some(b)
}

/// The big Play card: preview picture, title, meta, the primary action and the others.
fn hero(app: &App, n: &Node) -> gtk::Widget {
    let overlay = gtk::Overlay::new();
    overlay.add_css_class("gn-hero");
    let pic = match n.str("image") {
        Some(r) => picture(app, &r, 1280, 560),
        None => gtk::Picture::new(),
    };
    pic.set_height_request(260);
    pic.add_css_class("gn-hero");
    overlay.set_child(Some(&pic));
    let shade = gtk::Box::new(gtk::Orientation::Vertical, 6);
    shade.add_css_class("gn-hero-shade");
    shade.set_valign(gtk::Align::Fill);
    let text = gtk::Box::new(gtk::Orientation::Vertical, 6);
    text.set_valign(gtk::Align::End);
    text.set_vexpand(true);
    text.set_margin_start(24);
    text.set_margin_end(24);
    text.set_margin_bottom(20);
    text.append(&label(&n.title(), &["gn-hero-title"]));
    if let Some(s) = n.str("subtitle") {
        text.append(&label(&s, &["gn-hero-sub"]));
    }
    if let Some(m) = chips(n, "meta", "gn-chip") {
        text.append(&m);
    }
    let buttons = adw::WrapBox::new();
    buttons.set_child_spacing(10);
    buttons.set_line_spacing(8);
    buttons.set_margin_top(6);
    if let Some(p) = n.obj("primary") {
        let b = action_button(app, &n.id, p, true);
        register_activator(app, &n.id, &b);
        buttons.append(&b);
    }
    for a in n.arr("actions") {
        if let Some(a) = a.as_object() {
            let b = action_button(app, &n.id, a, false);
            b.add_css_class("pill");
            b.set_size_request(-1, 48);
            buttons.append(&b);
        }
    }
    text.append(&buttons);
    shade.append(&text);
    overlay.add_overlay(&shade);
    let frame = gtk::Frame::new(None);
    frame.add_css_class("gn-hero-frame");
    frame.set_child(Some(&overlay));
    frame.set_overflow(gtk::Overflow::Hidden);
    frame.upcast()
}

/// A card: optional preview picture, title, subtitle, badges; tapping it is its primary action.
fn card(app: &App, n: &Node) -> gtk::Button {
    let b = gtk::Button::new();
    b.add_css_class("card");
    b.add_css_class("gn-card");
    if n.bool("selected").unwrap_or(false) {
        b.add_css_class("gn-selected");
    }
    let v = gtk::Box::new(gtk::Orientation::Vertical, 0);
    // Map shots are pictures; other artwork (mode emblems) gives way to the card's symbolic icon,
    // so every card on a screen looks alike.
    let image = n.str("image").filter(|r| r.starts_with("pk3:maps/") || n.str("icon").is_none());
    match image {
        Some(r) => {
            let p = picture(app, &r, 480, 270);
            // Map shots fill the card; artwork (mode icons) is shown whole.
            if r.starts_with("pk3:maps/") {
                let featured = n.bool("featured").unwrap_or(false);
                p.set_height_request(if featured { if app.narrow.get() { 90 } else { 110 } } else { aspect_height(n.str("aspect"), 240).clamp(80, 200) });
                if featured {
                    // Home's cards share the window's height with the banner above them: both expand
                    // from a small minimum, so a short window (a phone on its side) needs no scrolling
                    p.set_vexpand(true);
                    b.add_css_class("gn-featured");
                }
            } else {
                p.set_content_fit(gtk::ContentFit::Contain);
                p.set_height_request(88);
                p.set_margin_top(12);
            }
            p.add_css_class("gn-card-pic");
            v.append(&p);
        }
        None => {
            b.add_css_class("gn-noimg");
            if let Some(i) = n.str("icon").and_then(|i| icon(&i)) {
                let im = gtk::Image::from_icon_name(i);
                im.set_pixel_size(40);
                im.set_halign(gtk::Align::Start);
                im.add_css_class("gn-card-icon");
                v.append(&im);
            }
        }
    }
    let t = gtk::Box::new(gtk::Orientation::Vertical, 4);
    t.add_css_class("gn-card-text");
    t.append(&label(&n.title(), &["gn-card-title"]));
    if let Some(s) = n.str("subtitle") {
        t.append(&label(&s, &["dim-label"]));
    }
    if let Some(c) = chips(n, "badges", "gn-badge") {
        t.append(&c);
    }
    if let Some(c) = chips(n, "meta", "gn-chip") {
        t.append(&c);
    }
    if let Some(p) = n.f64("progress") {
        let pb = gtk::ProgressBar::new();
        pb.set_fraction(p);
        t.append(&pb);
    }
    v.append(&t);
    if n.bool("featured").unwrap_or(false) {
        // Home's cards stay side by side at every width, never stacked: on a phone each has a
        // third of it. The picture shrinks and the subtitle and title end in an ellipsis
        // rather than hold the row wider than the window (a word set the minimum).
        let mut c = t.first_child();
        while let Some(w) = c {
            if let Some(l) = w.downcast_ref::<gtk::Label>() {
                if l.has_css_class("gn-card-title") {
                    l.set_wrap(false);
                } else {
                    l.set_lines(2);
                }
                l.set_ellipsize(gtk::pango::EllipsizeMode::End);
            }
            c = w.next_sibling();
        }
    }
    b.set_child(Some(&v));
    b.set_sensitive(n.enabled());
    if let Some(p) = n.obj("primary").cloned() {
        let w = Rc::downgrade(app);
        let id = n.id.clone();
        b.connect_clicked(move |_| {
            if let Some(a) = w.upgrade() {
                invoke(&a, &id, &p);
            }
        });
    }
    register_activator(app, &n.id, &b);
    b
}

/// Home's bar (`tabbar`: Settings, Credits, Quit): one strip of buttons joined edge to edge
/// along the bottom, like tabs. The `current` one (its screen is open) is a darker shade with
/// an ember line on top and does nothing when tapped; a pressed or focused one is darker too.
fn tabbar(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let bar = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    bar.set_homogeneous(true);
    bar.add_css_class("gn-tabbar");
    let current = n.str("current").unwrap_or_default();
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        let a = k.obj("action").cloned().unwrap_or_default();
        let b = gtk::Button::new();
        b.add_css_class("gn-tab");
        b.set_size_request(-1, if app.narrow.get() { 48 } else { 56 });
        let is_current = !current.is_empty() && k.id.ends_with(&format!("/{current}"));
        if is_current {
            b.add_css_class("gn-tab-current");
        }
        let head = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        head.set_halign(gtk::Align::Center);
        if let Some(i) = a.str_of("icon").and_then(|i| icon(&i)) {
            head.append(&gtk::Image::from_icon_name(i));
        }
        let title = label(&k.str("label").unwrap_or_else(|| k.title()), &["gn-tab-title"]);
        title.set_wrap(false);
        title.set_ellipsize(gtk::pango::EllipsizeMode::End);
        head.append(&title);
        b.set_child(Some(&head));
        let w = Rc::downgrade(app);
        let id = k.id.clone();
        b.connect_clicked(move |_| {
            if !is_current {
                if let Some(app) = w.upgrade() {
                    // Quit carries a confirm: invoke asks first
                    invoke(&app, &id, &a);
                }
            }
        });
        register_activator(app, &k.id, &b);
        bar.append(&b);
    }
    bar.upcast()
}

/// A short grid of cards (home tiles, modes). Long ones are `list` with `display: cards`.
fn grid(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let f = gtk::FlowBox::new();
    f.set_selection_mode(gtk::SelectionMode::None);
    f.set_homogeneous(true);
    f.set_row_spacing(12);
    let min = n.f64("minItem").unwrap_or(160.0);
    let kids = store.children(&n.id);
    let featured = kids.first().is_some_and(|k| k.bool("featured").unwrap_or(false));
    // Home's row on a phone: a little less between the cards, more for each
    f.set_column_spacing(if featured && app.narrow.get() { 8 } else { 12 });
    let compact_strip = n.str("scroll").as_deref() == Some("horizontal") && !featured && window_height(app) < SHORT_WINDOW;
    // Home's cards: always one row, side by side at every width, never stacked
    f.set_min_children_per_line(if featured { kids.len().clamp(1, 4) as u32 } else if app.narrow.get() { 1 } else { 2 });
    f.set_max_children_per_line(n.f64("maxColumns").map(|m| m as u32).unwrap_or(((1100.0 / (min + 12.0)) as u32).clamp(1, 6)));
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        let w = node(app, store, k);
        w.set_size_request(if app.narrow.get() || featured { 0 } else { min as i32 }, -1);
        if compact_strip {
            compact_card(&w);
        }
        f.append(&w);
    }
    if n.str("scroll").as_deref() == Some("horizontal") {
        f.set_min_children_per_line(store.children(&n.id).len() as u32);
        f.set_max_children_per_line(store.children(&n.id).len() as u32);
        let s = gtk::ScrolledWindow::builder().vscrollbar_policy(gtk::PolicyType::Never).child(&f).build();
        return s.upcast();
    }
    f.upcast()
}

/// A card in a strip on a short window (the game modes over the map picker): a chip with its name
/// only, no artwork and no subtitle (that stays as the tooltip), as the QuakeC renderer's strip on
/// a short page.
fn compact_card(card: &gtk::Widget) {
    let Some(v) = card.first_child() else { return };
    let mut c = v.first_child();
    while let Some(w) = c {
        if w.is::<gtk::Picture>() {
            w.set_visible(false);
        } else if w.has_css_class("gn-card-text") {
            let mut l = w.first_child();
            while let Some(x) = l {
                if !x.has_css_class("gn-card-title") {
                    if let Some(t) = x.downcast_ref::<gtk::Label>() {
                        card.set_tooltip_text(Some(&t.text()));
                    }
                    x.set_visible(false);
                }
                l = x.next_sibling();
            }
        }
        c = w.next_sibling();
    }
}

/// Small items side by side; `wrap` rows (the filter checkboxes) wrap on narrow widths.
fn row(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    if n.str("label").is_some() {
        return labelled_row(app, store, n);
    }
    let b = adw::WrapBox::new();
    b.set_child_spacing(12);
    b.set_line_spacing(6);
    if !n.bool("wrap").unwrap_or(false) {
        b.set_wrap_policy(adw::WrapPolicy::Minimum);
    }
    b.add_css_class("gn-filters");
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        let w = if k.kind == "button" {
            let mut a = k.obj("action").cloned().unwrap_or_default();
            a.entry("label").or_insert(json!(k.title()));
            if let Some(i) = k.str("icon") {
                a.entry("icon").or_insert(json!(i));
            }
            let bt = action_button(app, &k.id, &a, false);
            register_activator(app, &k.id, &bt);
            bt.add_css_class("flat");
            bt.upcast()
        } else {
            node(app, store, k)
        };
        b.append(&w);
    }
    b.upcast()
}

/// A `row` with a `label` (an address in the LAN popup): label, the content, its buttons at the end.
fn labelled_row(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let r = adw::ActionRow::builder().title(glib::markup_escape_text(&n.title())).build();
    r.add_css_class("property");
    if let Some(i) = n.str("icon").and_then(|i| icon(&i)) {
        r.add_prefix(&gtk::Image::from_icon_name(i));
    }
    for k in store.children(&n.id) {
        if k.hidden() {
            continue;
        }
        match k.kind.as_str() {
            "text" => {
                r.set_subtitle(&glib::markup_escape_text(&k.str("body").unwrap_or_default()));
                r.set_subtitle_selectable(k.bool("selectable").unwrap_or(false));
            }
            "button" => {
                let mut a = k.obj("action").cloned().unwrap_or_default();
                a.entry("label").or_insert(json!(k.title()));
                let bt = action_button(app, &k.id, &a, false);
                register_activator(app, &k.id, &bt);
                bt.add_css_class("flat");
                bt.set_valign(gtk::Align::Center);
                r.add_suffix(&bt);
            }
            _ => r.add_suffix(&node(app, store, k)),
        }
    }
    let g = adw::PreferencesGroup::new();
    g.add(&r);
    g.upcast()
}

fn text(n: &Node) -> gtk::Widget {
    let body = n.str("body").unwrap_or_else(|| n.title());
    if let Some(i) = n.str("icon").and_then(|i| icon(&i)) {
        let b = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        b.append(&gtk::Image::from_icon_name(i));
        let l = label(&body, &[]);
        l.set_max_width_chars(48);
        match n.str("style").as_deref() {
            Some("caption") | Some("note") => l.add_css_class("dim-label"),
            Some("warning") => l.add_css_class("warning"),
            _ => {}
        }
        b.append(&l);
        return b.upcast();
    }
    let l = label(&body, &[]);
    match n.str("style").as_deref() {
        Some("title") => l.add_css_class("title-2"),
        Some("caption") | Some("note") => l.add_css_class("dim-label"),
        Some("mono") => l.add_css_class("monospace"),
        Some("warning") => l.add_css_class("warning"),
        _ => {}
    }
    l.set_selectable(n.bool("selectable").unwrap_or(false));
    l.upcast()
}

fn status(app: &App, n: &Node) -> gtk::Widget {
    let s = adw::StatusPage::builder().title(glib::markup_escape_text(&n.title())).description(glib::markup_escape_text(&n.str("body").unwrap_or_default())).build();
    if let Some(i) = n.str("icon").and_then(|i| icon(&i)) {
        s.set_icon_name(Some(i));
    }
    let b = gtk::Box::new(gtk::Orientation::Vertical, 12);
    b.set_halign(gtk::Align::Center);
    if n.bool("indeterminate").unwrap_or(false) {
        let sp = adw::Spinner::new();
        sp.set_size_request(48, 48);
        b.append(&sp);
    } else if let Some(p) = n.f64("progress") {
        let pb = gtk::ProgressBar::new();
        pb.set_fraction(p);
        pb.set_size_request(320, -1);
        b.append(&pb);
    }
    if let Some(p) = n.obj("primary") {
        b.append(&action_button(app, &n.id, p, true));
    }
    for a in n.arr("actions") {
        if let Some(a) = a.as_object() {
            b.append(&action_button(app, &n.id, a, false));
        }
    }
    s.set_child(Some(&b));
    s.upcast()
}

fn progress(app: &App, n: &Node) -> gtk::Widget {
    let v = gtk::Box::new(gtk::Orientation::Vertical, 6);
    let l = label(&n.title(), &["heading"]);
    let pb = gtk::ProgressBar::new();
    let d = label(&n.str("detail").unwrap_or_default(), &["dim-label", "numeric"]);
    let set = {
        let (l, pb, d) = (l.clone(), pb.clone(), d.clone());
        move |n: &Node| {
            l.set_text(&n.title());
            d.set_text(&n.str("detail").unwrap_or_default());
            if n.bool("indeterminate").unwrap_or(false) {
                pb.pulse();
            } else {
                pb.set_fraction(n.f64("value").unwrap_or(0.0).clamp(0.0, 1.0));
            }
        }
    };
    set(n);
    v.append(&l);
    v.append(&pb);
    v.append(&d);
    if let Some(c) = n.obj("cancel") {
        let b = action_button(app, &n.id, c, false);
        b.set_halign(gtk::Align::Start);
        v.append(&b);
    }
    register_updater(app, &n.id, Rc::new(set));
    v.upcast()
}

/// Horizontal tabs: an inline switcher fixed at the top, each pane scrolling under it.
fn tabs(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let stack = adw::ViewStack::new();
    for p in store.children(&n.id) {
        let col = container(app, store, p, gtk::Orientation::Vertical);
        col.set_spacing(18);
        col.set_margin_top(12);
        col.set_margin_bottom(24);
        col.set_margin_start(16);
        col.set_margin_end(16);
        let clamp = adw::Clamp::builder().maximum_size(900).child(&col).build();
        let sw = gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).child(&clamp).vexpand(true).build();
        let key = p.str("key").unwrap_or_else(|| p.id.clone());
        let page = stack.add_titled(&sw, Some(&key), &p.title());
        if let Some(i) = p.str("icon").and_then(|i| icon(&i)) {
            page.set_icon_name(Some(i));
        }
    }
    if let Some(v) = n.str("value") {
        stack.set_visible_child_name(&v);
    }
    let sw = adw::InlineViewSwitcher::builder().stack(&stack).display_mode(adw::InlineViewSwitcherDisplayMode::Labels).build();
    sw.set_halign(gtk::Align::Center);
    sw.set_margin_top(6);
    sw.set_margin_bottom(6);
    let w = Rc::downgrade(app);
    let id = n.id.clone();
    stack.connect_visible_child_name_notify(move |s| {
        if let (Some(a), Some(v)) = (w.upgrade(), s.visible_child_name()) {
            value(&a, &id, json!(v.to_string()));
        }
    });
    let tv = adw::ToolbarView::new();
    tv.add_top_bar(&sw);
    tv.set_content(Some(&stack));
    tv.set_vexpand(true);
    tv.upcast()
}

/// Vertical tabs (Settings): a sidebar of sections, the selected one beside it;
/// one column with the section pushed on narrow windows (AdwNavigationSplitView).
fn split(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let kids = store.children(&n.id);
    let (Some(list), detail) = (kids.first(), kids.get(1)) else { return container(app, store, n, gtk::Orientation::Vertical).upcast() };
    let lb = gtk::ListBox::new();
    lb.add_css_class("navigation-sidebar");
    lb.set_selection_mode(gtk::SelectionMode::Single);
    let items = store.children(&list.id);
    let mut selected = None;
    for (i, it) in items.iter().enumerate() {
        if it.hidden() {
            continue;
        }
        let r = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        r.set_margin_top(10);
        r.set_margin_bottom(10);
        if let Some(ic) = it.str("icon").and_then(|i| icon(&i)) {
            r.append(&gtk::Image::from_icon_name(ic));
        }
        r.append(&label(&it.title(), &[]));
        lb.append(&r);
        if it.bool("selected").unwrap_or(false) {
            selected = Some(i);
        }
    }
    if let Some(i) = selected {
        lb.select_row(lb.row_at_index(i as i32).as_ref());
    }
    let actions: Vec<(String, Map<String, Value>)> = items.iter().filter(|i| !i.hidden()).map(|i| (i.id.clone(), i.obj("primary").or_else(|| i.obj("action")).cloned().unwrap_or_default())).collect();
    let w = Rc::downgrade(app);
    lb.connect_row_activated(move |_, row| {
        if let (Some(a), Some((id, act))) = (w.upgrade(), actions.get(row.index() as usize)) {
            if act.str_of("id").is_some() {
                invoke(&a, id, act);
            } else {
                value(&a, id, json!(true));
            }
        }
    });
    let side_sw = gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).child(&lb).vexpand(true).build();
    let sidebar = adw::NavigationPage::builder().title(list.title()).child(&side_sw).tag("sidebar").build();
    let content_w: gtk::Widget = match detail {
        Some(d) if d.kind == "tabs" => tabs(app, store, d),
        Some(d) => {
            let col = gtk::Box::new(gtk::Orientation::Vertical, 18);
            col.set_margin_top(12);
            col.set_margin_bottom(24);
            col.set_margin_start(16);
            col.set_margin_end(16);
            for k in store.children(&d.id) {
                if !k.hidden() {
                    col.append(&node(app, store, k));
                }
            }
            let clamp = adw::Clamp::builder().maximum_size(900).child(&col).build();
            gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).child(&clamp).vexpand(true).build().upcast()
        }
        None => adw::StatusPage::builder().title("Choose a section").build().upcast(),
    };
    let content = adw::NavigationPage::builder().title(detail.map(|d| d.title()).unwrap_or_default()).child(&content_w).tag("detail").build();
    let sv = adw::NavigationSplitView::builder().sidebar(&sidebar).content(&content).min_sidebar_width(200.0).max_sidebar_width(280.0).build();
    sv.set_collapsed(app.narrow.get());
    sv.set_show_content(n.bool("detailOpen").unwrap_or(true));
    sv.set_vexpand(true);
    sv.upcast()
}

/// A detail header: big picture, title, attributes, description, actions.
/// A preview that follows its node (the chosen map changes while the picker stays put).
fn preview(app: &App, n: &Node) -> gtk::Widget {
    let holder = gtk::Box::new(gtk::Orientation::Vertical, 0);
    holder.append(&preview_content(app, n));
    let (w, h) = (Rc::downgrade(app), holder.downgrade());
    register_updater(app, &n.id, Rc::new(move |n: &Node| {
        let (Some(app), Some(h)) = (w.upgrade(), h.upgrade()) else { return };
        while let Some(c) = h.first_child() {
            h.remove(&c);
        }
        h.append(&preview_content(&app, n));
    }));
    holder.upcast()
}

fn preview_content(app: &App, n: &Node) -> gtk::Widget {
    let v = gtk::Box::new(gtk::Orientation::Vertical, 12);
    // `density: compact` (the chosen map above the picker): the picture beside the text.
    let compact = n.str("density").as_deref() == Some("compact") && !app.narrow.get();
    let outer = gtk::Box::new(gtk::Orientation::Horizontal, 18);
    if let Some(r) = n.str("image") {
        let p = picture(app, &r, 1280, 720);
        if compact {
            p.set_size_request(320, 180);
        } else {
            p.set_height_request(aspect_height(n.str("aspect"), 640).clamp(160, 380));
        }
        p.add_css_class("gn-preview-pic");
        let f = gtk::Frame::new(None);
        f.set_child(Some(&p));
        f.set_overflow(gtk::Overflow::Hidden);
        f.add_css_class("gn-preview-pic");
        if compact {
            f.set_valign(gtk::Align::Start);
            outer.append(&f);
        } else {
            v.append(&f);
        }
    }
    v.append(&label(&n.title(), &["gn-preview-title"]));
    if let Some(s) = n.str("subtitle") {
        v.append(&label(&s, &["dim-label"]));
    }
    if let Some(c) = chips(n, "badges", "gn-badge") {
        v.append(&c);
    }
    let attrs = n.arr("attrs");
    if !attrs.is_empty() {
        let g = adw::PreferencesGroup::new();
        for a in attrs {
            let Some(a) = a.as_object() else { continue };
            let r = adw::ActionRow::builder().title(glib::markup_escape_text(&a.str_of("label").unwrap_or_default())).build();
            r.add_css_class("property");
            r.set_subtitle(&glib::markup_escape_text(&a.str_of("value").unwrap_or_default()));
            if let Some(i) = a.str_of("icon").and_then(|i| icon(&i)) {
                r.add_prefix(&gtk::Image::from_icon_name(i));
            }
            g.add(&r);
        }
        v.append(&g);
    }
    if let Some(b) = n.str("body") {
        v.append(&label(&b, &[]));
    }
    let buttons = gtk::Box::new(gtk::Orientation::Horizontal, 10);
    if let Some(p) = n.obj("primary") {
        let b = action_button(app, &n.id, p, true);
        register_activator(app, &n.id, &b);
        buttons.append(&b);
    }
    for a in n.arr("actions") {
        if let Some(a) = a.as_object() {
            let b = action_button(app, &n.id, a, false);
            b.add_css_class("pill");
            b.set_size_request(-1, 48);
            buttons.append(&b);
        }
    }
    v.append(&buttons);
    if compact {
        v.set_hexpand(true);
        outer.append(&v);
        return outer.upcast();
    }
    v.upcast()
}

fn cell_text(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Bool(b)) => if *b { "yes".into() } else { String::new() },
        Some(Value::Number(n)) => n.to_string(),
        _ => String::new(),
    }
}

/// A `list`: a GtkColumnView (table) or GtkGridView (cards) over a windowed data source.
fn list(app: &App, store: &Store, n: &Node) -> gtk::Widget {
    let source = n.str("source").unwrap_or_default();
    let model = WindowModel::default();
    // Start from what the core already holds for this source.
    if let Some(w) = store.sources.get(&source) {
        model.splice(&godnotic_core::bridge::Splice { position: 0, removed: 0, added: w.total() });
    }
    let lv = Rc::new(RefCell::new(ListView {
        source: source.clone(),
        model: model.clone(),
        sort: n.str("sort"),
        desc: n.bool("desc").unwrap_or(false),
        filter: n.str("filter").unwrap_or_default(),
        seen: Cell::new((usize::MAX, 0)),
    }));
    if let Some(u) = app.ui.borrow_mut().as_mut() {
        u.lists.insert(n.id.clone(), lv.clone());
    }
    want_rows(app, &lv);
    let display = n.str("display").unwrap_or_else(|| "table".into());
    let view: gtk::Widget = if display == "cards" { card_grid(app, n, &lv) } else { table(app, n, &lv) };
    let sideways = (display == "cards").then(|| sideways_rows(n)).flatten();
    let sw = match sideways {
        // N rows, scrolling sideways, the scroll bar inside the list
        Some(rows) => {
            let sw = gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Automatic).vscrollbar_policy(gtk::PolicyType::Never)
                .kinetic_scrolling(true).overlay_scrolling(false).child(&view).build();
            let (rows, tile) = sideways_fit(app, rows);
            let h = rows as i32 * (tile + 26) + 26;
            sw.set_min_content_height(h);
            sw.set_height_request(h);
            sw.set_propagate_natural_height(true);
            sw.add_css_class("gn-sideways");
            sw
        }
        None => {
            let sw = gtk::ScrolledWindow::builder().hscrollbar_policy(gtk::PolicyType::Never).kinetic_scrolling(true).child(&view).vexpand(true).build();
            sw.set_min_content_height(if display == "cards" { 420 } else { 260 });
            sw
        }
    };
    // Empty state, once the engine has answered with no rows.
    let answered_empty = store.sources.get(&source).is_some_and(|w| w.total() == 0 && w.version > 0);
    if answered_empty && n.str("state").as_deref() == Some("ready") {
        if let Some(e) = n.obj("empty") {
            let s = adw::StatusPage::builder().title(e.str_of("title").unwrap_or_default()).description(e.str_of("text").unwrap_or_default()).build();
            if let Some(i) = e.str_of("icon").and_then(|i| icon(&i)) {
                s.set_icon_name(Some(i));
            }
            return s.upcast();
        }
    }
    sw.upcast()
}

/// Notes which positions are being bound; requests run when the main loop is idle.
fn saw(app: &App, lv: &Rc<RefCell<ListView>>, pos: usize) {
    let l = lv.borrow();
    let (a, b) = l.seen.get();
    let missing = {
        let core = app.core.borrow();
        core.store.sources.get(&l.source).is_none_or(|w| w.row(pos).is_none())
    };
    l.seen.set((a.min(pos), b.max(pos)));
    drop(l);
    if missing {
        want_rows(app, lv);
        // The next requests start from what is bound after these.
        let l = lv.borrow();
        l.seen.set((pos, pos));
    }
}

fn row_cells(app: &App, source: &str, pos: usize) -> Option<Map<String, Value>> {
    let core = app.core.borrow();
    core.store.sources.get(source)?.row(pos).cloned()
}

fn table(app: &App, n: &Node, lv: &Rc<RefCell<ListView>>) -> gtk::Widget {
    let model = lv.borrow().model.clone();
    // A list with `selection: single` (the server list): a click chooses the row and shows it chosen,
    // a second click in time joins (app::row_activated tells the two apart). Others activate.
    let selects = selects_rows(n);
    let cv = if selects {
        let sel = gtk::SingleSelection::new(Some(model));
        sel.set_autoselect(false);
        sel.set_can_unselect(true);
        sel.set_selected(gtk::INVALID_LIST_POSITION);
        gtk::ColumnView::new(Some(sel))
    } else {
        gtk::ColumnView::new(Some(gtk::NoSelection::new(Some(model))))
    };
    cv.set_single_click_activate(true);
    if selects {
        // The engine's answer to a choice changes only `selected`: the view already shows it,
        // so the screen (and the list's scroll position) stays as it is. Anything else that
        // changes on the list rebuilds the screen as before.
        let rest = |n: &Node| {
            let mut p = n.props.clone();
            p.remove("selected");
            Value::Object(p).to_string()
        };
        let before = RefCell::new(rest(n));
        let (w, screen_id) = (Rc::downgrade(app), n.id.split('/').next().unwrap_or_default().to_string());
        let (cvw, src) = (cv.downgrade(), lv.borrow().source.clone());
        register_updater(app, &n.id, Rc::new(move |n: &Node| {
            let now = rest(n);
            if *before.borrow() == now {
                // only the choice changed: show the engine's chosen row as chosen
                let (Some(a), Some(cv)) = (w.upgrade(), cvw.upgrade()) else { return };
                let Some(sel) = cv.model().and_downcast::<gtk::SingleSelection>() else { return };
                let key = n.arr("selected").first().and_then(|v| v.as_str()).map(str::to_string);
                let pos = key.and_then(|k| {
                    let core = a.core.borrow();
                    let win = core.store.sources.get(&src)?;
                    (0..win.total().min(5000)).find(|i| win.row(*i).and_then(|r| r.str_of("key")).as_deref() == Some(&k))
                });
                let want = pos.map(|p| p as u32).unwrap_or(gtk::INVALID_LIST_POSITION);
                if sel.selected() != want {
                    sel.set_selected(want);
                }
                return;
            }
            *before.borrow_mut() = now;
            let (w, id) = (w.clone(), screen_id.clone());
            glib::idle_add_local_once(move || {
                if let Some(a) = w.upgrade() {
                    app::refresh_screen(&a, &id);
                }
            });
        }));
    }
    cv.set_show_row_separators(false);
    cv.set_reorderable(false);
    if n.str("density").as_deref() == Some("compact") || n.str("rowHeight").as_deref() == Some("compact") || n.str("display").as_deref() != Some("rows") {
        cv.add_css_class("gn-dense");
    }
    cv.add_css_class("data-table");
    let narrow = app.narrow.get();
    let cols: Vec<Map<String, Value>> = n.arr("columns").iter().filter_map(|c| c.as_object().cloned()).collect();
    let source = lv.borrow().source.clone();
    for c in &cols {
        let key = c.str_of("key").unwrap_or_default();
        let kind = c.str_of("type").unwrap_or_else(|| "text".into());
        let prio = c.i64_of("priority").unwrap_or(1);
        // Narrow windows keep priority 1 only; the name cell then carries a second line.
        if narrow && (prio > 1 || kind != "text") {
            continue;
        }
        let f = gtk::SignalListItemFactory::new();
        let two_line = narrow && prio == 1 && kind == "text";
        f.connect_setup(move |_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
            let b = gtk::Box::new(gtk::Orientation::Vertical, 0);
            match kind.as_str() {
                "lock" | "icon" | "bool" => b.append(&gtk::Image::new()),
                _ => {
                    let l = gtk::Label::new(None);
                    l.set_xalign(0.0);
                    l.set_ellipsize(gtk::pango::EllipsizeMode::End);
                    b.append(&l);
                    if two_line {
                        let s = gtk::Label::new(None);
                        s.set_xalign(0.0);
                        s.add_css_class("dim-label");
                        s.add_css_class("caption");
                        s.set_ellipsize(gtk::pango::EllipsizeMode::End);
                        b.append(&s);
                    }
                }
            }
            item.set_child(Some(&b));
        });
        let (w, src, lv2, k2, t2) = (Rc::downgrade(app), source.clone(), lv.clone(), key.clone(), c.str_of("type").unwrap_or_else(|| "text".into()));
        f.connect_bind(move |_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
            let (Some(app), Some(obj), Some(b)) = (w.upgrade(), item.item(), item.child()) else { return };
            let pos = app::index(&obj);
            saw(&app, &lv2, pos);
            // Read the row in place (no copy): binding happens many times a second while flinging.
            let core = app.core.borrow();
            let row = core.store.sources.get(&src).and_then(|w| w.row(pos));
            let cells = row.and_then(|r| r.get("cells")).and_then(Value::as_object);
            let first = b.first_child();
            if row.is_none() {
                b.add_css_class("gn-placeholder");
            } else {
                b.remove_css_class("gn-placeholder");
            }
            let v = cells.and_then(|c| c.get(&k2));
            match t2.as_str() {
                "lock" | "bool" => {
                    if let Some(im) = first.and_downcast::<gtk::Image>() {
                        let on = v.and_then(Value::as_bool).unwrap_or(false);
                        im.set_icon_name(on.then_some(if t2 == "lock" { "changes-prevent-symbolic" } else { "object-select-symbolic" }));
                    }
                }
                "icon" => {
                    if let Some(im) = first.and_downcast::<gtk::Image>() {
                        im.set_icon_name(v.and_then(Value::as_str).and_then(adwaita_icon));
                    }
                }
                _ => {
                    let Some(l) = first.and_downcast::<gtk::Label>() else { return };
                    let text = match t2.as_str() {
                        "players" => cells.map(|c| {
                            let p = c.get("players").and_then(Value::as_i64).unwrap_or(0);
                            let m = c.get("max").and_then(Value::as_i64).unwrap_or(0);
                            format!("{p}/{m}")
                        }).unwrap_or_default(),
                        "ping" => v.and_then(Value::as_i64).map(|p| format!("{p} ms")).unwrap_or_default(),
                        "mode" => cells.and_then(|c| c.get("modeName").and_then(Value::as_str).map(str::to_string)).unwrap_or_else(|| cell_text(v).to_uppercase()),
                        _ => cell_text(v),
                    };
                    l.set_text(&text);
                    for c in ["gn-ping-good", "gn-ping-ok", "gn-ping-bad"] {
                        l.remove_css_class(c);
                    }
                    if t2 == "ping" {
                        let p = v.and_then(Value::as_i64).unwrap_or(0);
                        l.add_css_class(if p < 60 { "gn-ping-good" } else if p < 150 { "gn-ping-ok" } else { "gn-ping-bad" });
                    }
                    if let Some(s) = l.next_sibling().and_downcast::<gtk::Label>() {
                        let sub = cells.map(|c| {
                            let mode = c.get("modeName").and_then(Value::as_str).map(str::to_string).unwrap_or_else(|| cell_text(c.get("mode")).to_uppercase());
                            let p = format!("{}/{}", c.get("players").and_then(Value::as_i64).unwrap_or(0), c.get("max").and_then(Value::as_i64).unwrap_or(0));
                            let ping = c.get("ping").and_then(Value::as_i64).map(|p| format!(" · {p} ms")).unwrap_or_default();
                            let lock = if c.get("lock").and_then(Value::as_bool).unwrap_or(false) { " · 🔒" } else { "" };
                            format!("{} · {mode} · {p}{ping}{lock}", cell_text(c.get("map")))
                        }).unwrap_or_default();
                        s.set_text(&sub);
                    }
                }
            }
        });
        let title = c.str_of("label").unwrap_or_default();
        let col = gtk::ColumnViewColumn::new(Some(&title), Some(f));
        match c.str_of("width").as_deref() {
            Some("fill") => col.set_expand(true),
            Some("xs") => col.set_fixed_width(36),
            Some("s") => col.set_fixed_width(80),
            Some("m") => col.set_fixed_width(150),
            Some("l") => col.set_fixed_width(220),
            _ => {}
        }
        if two_line {
            col.set_expand(true);
        }
        if c.bool_of("sortable").unwrap_or(false) {
            col.set_sorter(Some(&gtk::CustomSorter::new(|_, _| gtk::Ordering::Equal)));
            col.set_id(Some(&key));
        }
        cv.append_column(&col);
    }
    // Header clicks re-window with the engine's sort (the model itself is never sorted here).
    if let Some(sorter) = cv.sorter().and_downcast::<gtk::ColumnViewSorter>() {
        let (w, lv2) = (Rc::downgrade(app), lv.clone());
        sorter.connect_changed(move |s, _| {
            let Some(app) = w.upgrade() else { return };
            let col = s.primary_sort_column().and_then(|c| c.id()).map(|g| g.to_string());
            {
                let mut l = lv2.borrow_mut();
                l.sort = col;
                l.desc = s.primary_sort_order() == gtk::SortType::Descending;
            }
            let src = lv2.borrow().source.clone();
            if let Some(win) = app.core.borrow_mut().store.sources.get_mut(&src) {
                win.invalidate();
            }
            want_rows(&app, &lv2);
        });
    }
    let (w, list_id, src) = (Rc::downgrade(app), n.id.clone(), source.clone());
    cv.connect_activate(move |_, pos| {
        if let Some(a) = w.upgrade() {
            app::row_activated(&a, &list_id, &src, pos as usize);
        }
    });
    register_activator(app, &n.id, &cv);
    cv.upcast()
}

/// `display: cards` (the map picker): a GtkGridView of preview cards; pictures load only for bound cards.
/// A list whose `selection` is `single` or `multi` tells choosing from activating (6.1).
pub fn selects_rows(n: &Node) -> bool {
    matches!(n.str("selection").as_deref(), Some("single") | Some("multi"))
}

/// `rows: N` with `scroll: horizontal` on a card list (the map picker): N full rows of
/// tiles in view, more by scrolling sideways.
fn sideways_rows(n: &Node) -> Option<u32> {
    (n.str("scroll").as_deref() == Some("horizontal")).then(|| n.f64("rows")).flatten().map(|r| (r as u32).clamp(1, 4))
}

/// The window's height, before the first allocation its default one.
fn window_height(app: &App) -> i32 {
    app.ui.borrow().as_ref().map(|u| if u.win.height() > 0 { u.win.height() } else { u.win.default_height() }).filter(|h| *h > 0).unwrap_or(760)
}

/// A short window (a phone on its side, 480 high): the strip of game modes is chips, as in the
/// QuakeC renderer on a short page (touch_uib_render.qc), so the maps under it keep their room.
const SHORT_WINDOW: i32 = 540;

/// The rows and the height of one tile of a sideways list: up to N rows fill the room the window
/// has under the header, the mode strip and the section title, and above the action bar. When N
/// rows would be under the smallest legible tile there, fewer and taller ones: a row reached
/// only by scrolling the page leaves the visible one a sliver.
fn sideways_fit(app: &App, rows: u32) -> (u32, i32) {
    let win_h = window_height(app);
    let above = if app.narrow.get() { 190 } else if win_h < SHORT_WINDOW { 310 } else { 440 };
    let room = win_h - above;
    let mut r = rows.max(1);
    while r > 1 && room / r as i32 - 26 < 96 {
        r -= 1;
    }
    (r, (room / r as i32 - 26).clamp(96, 170))
}

fn card_grid(app: &App, n: &Node, lv: &Rc<RefCell<ListView>>) -> gtk::Widget {
    let model = lv.borrow().model.clone();
    let f = gtk::SignalListItemFactory::new();
    let fit = sideways_rows(n).map(|r| sideways_fit(app, r));
    let tile = fit.map(|(_, h)| h);
    f.connect_setup(move |_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
        let v = gtk::Box::new(gtk::Orientation::Vertical, 0);
        v.add_css_class("card");
        v.add_css_class("gn-card");
        let p = gtk::Picture::new();
        p.set_content_fit(gtk::ContentFit::Cover);
        p.set_height_request(tile.map(|t| t - 34).unwrap_or(120));
        p.set_can_shrink(true);
        p.add_css_class("gn-card-pic");
        v.append(&p);
        let t = gtk::Box::new(gtk::Orientation::Vertical, 2);
        t.add_css_class("gn-card-text");
        let title = gtk::Label::new(None);
        title.set_xalign(0.0);
        title.add_css_class("gn-card-title");
        title.set_ellipsize(gtk::pango::EllipsizeMode::End);
        let sub = gtk::Label::new(None);
        sub.set_xalign(0.0);
        sub.add_css_class("dim-label");
        sub.add_css_class("caption");
        sub.set_ellipsize(gtk::pango::EllipsizeMode::End);
        t.append(&title);
        t.append(&sub);
        v.append(&t);
        match tile {
            // a tile: the picture at 16:9 and one line for the name
            Some(th) => {
                sub.set_visible(false);
                t.add_css_class("gn-tile-text");
                v.set_size_request((th - 34) * 16 / 9, th);
            }
            None => v.set_size_request(180, -1),
        }
        item.set_child(Some(&v));
    });
    let (w, src, lv2, list_id_b) = (Rc::downgrade(app), lv.borrow().source.clone(), lv.clone(), n.id.clone());
    f.connect_bind(move |_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
        let (Some(app), Some(obj), Some(v)) = (w.upgrade(), item.item(), item.child()) else { return };
        let pos = app::index(&obj);
        saw(&app, &lv2, pos);
        let row = row_cells(&app, &src, pos);
        let Some(p) = v.first_child().and_downcast::<gtk::Picture>() else { return };
        let Some(t) = p.next_sibling() else { return };
        let title = t.first_child().and_downcast::<gtk::Label>();
        let sub = title.as_ref().and_then(|l| l.next_sibling()).and_downcast::<gtk::Label>();
        let cells = row.as_ref().and_then(|r| r.get("cells")).and_then(Value::as_object);
        let key = row.as_ref().and_then(|r| r.str_of("key")).unwrap_or_default();
        let chosen = !key.is_empty() && app.core.borrow().store.node(&list_id_b).is_some_and(|n| n.arr("selected").iter().any(|v| v.as_str() == Some(&key)));
        v.set_widget_name(&key);
        if chosen { v.add_css_class("gn-selected") } else { v.remove_css_class("gn-selected") }
        if cells.and_then(|c| c.get("locked")).and_then(Value::as_bool).unwrap_or(false) { v.add_css_class("gn-dim") } else { v.remove_css_class("gn-dim") }
        if let Some(l) = title {
            l.set_text(&cells.map(|c| cell_text(c.get("title").or(c.get("name")))).unwrap_or_default());
        }
        if let Some(l) = sub {
            l.set_text(&cells.map(|c| {
                let modes = cell_text(c.get("modes")).split_whitespace().map(|m| m.to_uppercase()).collect::<Vec<_>>().join(" ");
                let players = cell_text(c.get("players"));
                if let Some(mode) = c.get("modeName") {
                    // a campaign level: its mode and whether it is won, next or locked
                    let state = match cell_text(c.get("state")).as_str() { "done" => "✓ ", "locked" => "Locked · ", _ => "" };
                    return format!("{state}{}", cell_text(Some(mode)));
                }
                if players.is_empty() { modes } else { format!("{modes} · {players} players") }
            }).unwrap_or_default());
        }
        match row.as_ref().and_then(|r| r.str_of("image")) {
            Some(r) => {
                if app.images.borrow_mut().attach(&r, &p) {
                    want_image(&app, &r, 360, 200);
                }
            }
            None => {
                p.set_paintable(gtk::gdk::Paintable::NONE);
                p.add_css_class("gn-placeholder");
            }
        }
    });
    // Unbound cards drop their picture (the texture stays in the bounded cache).
    f.connect_unbind(|_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
        if let Some(p) = item.child().and_then(|v| v.first_child()).and_downcast::<gtk::Picture>() {
            p.set_paintable(gtk::gdk::Paintable::NONE);
        }
    });
    // A list with `selection: single` tells choosing from playing: one click or tap chooses the card
    // (event row/select), a second one on it within the double-click time plays it
    // (row/activate); app::row_activated keeps the time, so it survives the screen updating
    // between the two. Scrolling does neither. Without it a click activates.
    let gv = gtk::GridView::new(Some(gtk::NoSelection::new(Some(model))), Some(f));
    gv.set_single_click_activate(true);
    gv.set_min_columns(if app.narrow.get() { 1 } else { 2 });
    gv.set_max_columns(n.f64("maxColumns").map(|m| m as u32).unwrap_or(6));
    if let Some((rows, _)) = fit {
        // `rows: N`, scrolling sideways: the grid runs left to right in N rows (fewer on a short window).
        gv.set_orientation(gtk::Orientation::Horizontal);
        gv.set_min_columns(rows);
        gv.set_max_columns(rows);
    }
    let (w, list_id, src) = (Rc::downgrade(app), n.id.clone(), lv.borrow().source.clone());
    gv.connect_activate(move |_, pos| {
        if let Some(a) = w.upgrade() {
            app::row_activated(&a, &list_id, &src, pos as usize);
        }
    });
    register_activator(app, &n.id, &gv);
    // A new choice (`selected` changed) moves the ring; the grid and its scroll stay.
    let gvw = gv.downgrade();
    register_updater(app, &n.id, Rc::new(move |n: &Node| {
        let Some(gv) = gvw.upgrade() else { return };
        let chosen: Vec<String> = n.arr("selected").iter().filter_map(|v| v.as_str().map(str::to_string)).collect();
        let mut c = gv.first_child();
        while let Some(cell) = c {
            if let Some(card) = cell.first_child() {
                if chosen.iter().any(|k| *k == card.widget_name()) { card.add_css_class("gn-selected") } else { card.remove_css_class("gn-selected") }
            }
            c = cell.next_sibling();
        }
    }));
    gv.upcast()
}

/// A modal screen (`app.modal`) as an AdwDialog: a sheet at the bottom on narrow windows.
pub fn dialog(app: &App, id: &str) -> adw::Dialog {
    let d = adw::Dialog::new();
    {
        let core = app.core.borrow();
        if let Some(s) = core.store.node(id) {
            d.set_title(&s.title());
            app.quiet.set(true);
            let content = screen(app, &core.store, s);
            app.quiet.set(false);
            d.set_child(Some(&content));
            d.set_content_width(520);
            // as tall as its content asks: about a row per child, within the window
            let rows = core.store.children(id).iter().map(|k| 1 + core.store.children(&k.id).len()).sum::<usize>() as i32;
            d.set_content_height(if s.str("presentation").as_deref() == Some("dialog") { (190 + rows * 58).clamp(300, 640) } else { 480 });
            d.set_can_close(s.bool("dismissable").unwrap_or(true));
        }
    }
    let w = Rc::downgrade(app);
    let id2 = id.to_string();
    d.connect_closed(move |_| {
        if let Some(a) = w.upgrade() {
            let still = a.core.borrow().store.modal().contains(&id2);
            if still {
                let m = a.core.borrow_mut().client.simple("dismiss", Some(&id2));
                app::send(&a, m);
                crate::backend::fixture_dismiss(&a, &id2);
            }
        }
    });
    d
}
