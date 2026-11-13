// SPDX-License-Identifier: GPL-3.0-or-later
//! The UI-bridge client (docs/UI_BRIDGE.md, protocol 1, schema 1.0).
//!
//! The engine's menu VM describes every screen once as a flat map of keyed
//! nodes; the loader holds a [`Store`], applies snapshots and diffs exactly as
//! section 3.4 says, windows long lists per data source ([`ListWindow`]),
//! asks for images once, and sends events through a [`Client`] that numbers
//! its messages. [`fixture`] plays the fixtures of tests/uibridge/fixtures
//! in-process, the way scripts/uib-replay.py does over a stream.

pub mod fixture;
mod store;
mod window;

pub use store::{Change, Node, Store};
pub use window::{ListWindow, Splice};

use serde_json::{json, Map, Value};

/// Protocol major this client speaks, and the schema minor it was written against.
pub const PROTOCOL: u32 = 1;
pub const SCHEMA: &str = "1.0";

/// Node types the GTK renderer draws natively (sent in `hello.caps.nodes`).
pub const NODE_TYPES: &[&str] = &[
    "app", "screen", "toast", "section", "toolbar", "actionbar", "row", "grid", "tabs", "pane", "split", "form",
    "card", "hero", "list", "preview", "text", "image", "divider", "status", "progress",
    "switch", "slider", "stepper", "choice", "field", "button", "colour",
];
pub const HINTS: &[&str] = &["density", "emphasis", "accent", "featured", "aspect", "importance", "span", "tone", "live"];

/// The renderer's side of the conversation: every message carries `uib`, `t` and an increasing `id`.
#[derive(Default)]
pub struct Client {
    next: u64,
}

impl Client {
    pub fn new() -> Self {
        Self { next: 0 }
    }

    fn msg(&mut self, t: &str, body: Value) -> String {
        self.next += 1;
        let mut m = Map::new();
        m.insert("uib".into(), json!(PROTOCOL));
        m.insert("t".into(), json!(t));
        m.insert("id".into(), json!(self.next));
        if let Value::Object(b) = body {
            m.extend(b);
        }
        Value::Object(m).to_string()
    }

    /// `caps` is section 7's object (nodes, hints, size classes, input, locale...).
    pub fn hello(&mut self, name: &str, version: &str, caps: Value) -> String {
        self.msg("hello", json!({"schema": SCHEMA, "renderer": {"name": name, "version": version}, "caps": caps}))
    }
    pub fn caps(&mut self, caps: Value) -> String {
        self.msg("caps", json!({"caps": caps}))
    }
    pub fn sync(&mut self, have: i64) -> String {
        self.msg("sync", json!({"have": have}))
    }
    pub fn action(&mut self, node: &str, action: &str, payload: Option<Value>) -> String {
        let mut b = json!({"ev": "action", "node": node, "action": action});
        if let Some(p) = payload {
            b["payload"] = p;
        }
        self.msg("ev", b)
    }
    pub fn value(&mut self, node: &str, value: Value) -> String {
        self.msg("ev", json!({"ev": "value", "node": node, "value": value}))
    }
    pub fn submit(&mut self, node: &str, value: Value) -> String {
        self.msg("ev", json!({"ev": "submit", "node": node, "value": value}))
    }
    pub fn row(&mut self, list: &str, row: &str, kind: &str) -> String {
        self.msg("ev", json!({"ev": "row", "node": list, "row": row, "kind": kind}))
    }
    pub fn simple(&mut self, ev: &str, node: Option<&str>) -> String {
        let mut b = json!({"ev": ev});
        if let Some(n) = node {
            b["node"] = json!(n);
        }
        self.msg("ev", b)
    }
    pub fn visible(&mut self, list: &str, offset: usize, limit: usize) -> String {
        self.msg("ev", json!({"ev": "visible", "node": list, "offset": offset, "limit": limit}))
    }
    /// Rows `offset..offset+limit` (limit at most 200) of a data source.
    pub fn window(&mut self, source: &str, offset: usize, limit: usize, sort: Option<&str>, desc: bool, filter: Option<&str>) -> String {
        let mut b = json!({"source": source, "offset": offset, "limit": limit.min(200)});
        if let Some(s) = sort {
            b["sort"] = json!(s);
            b["desc"] = json!(desc);
        }
        if let Some(f) = filter.filter(|f| !f.is_empty()) {
            b["filter"] = json!(f);
        }
        self.msg("window", b)
    }
    /// An image ref at a pixel box.
    pub fn image(&mut self, r: &str, w: u32, h: u32) -> String {
        self.msg("image", json!({"ref": r, "w": w, "h": h, "as": "path"}))
    }
    pub fn bye(&mut self, reason: &str) -> String {
        self.msg("bye", json!({"reason": reason}))
    }
}

/// Icon names (section 5.1) to Adwaita symbolic names; None: draw no icon.
pub fn adwaita_icon(name: &str) -> Option<&'static str> {
    Some(match name {
        "play" | "resume" => "media-playback-start-symbolic",
        "pause" => "media-playback-pause-symbolic",
        "singleplayer" | "player" => "avatar-default-symbolic",
        "multiplayer" | "players" => "system-users-symbolic",
        "settings" => "emblem-system-symbolic",
        "credits" => "help-about-symbolic",
        "quit" => "system-shutdown-symbolic",
        "back" => "go-previous-symbolic",
        "close" => "window-close-symbolic",
        "search" => "system-search-symbolic",
        "filter" => "funnel-symbolic",
        "sort" => "view-sort-descending-symbolic",
        "refresh" => "view-refresh-symbolic",
        "lock" => "changes-prevent-symbolic",
        "unlock" => "changes-allow-symbolic",
        "star" => "non-starred-symbolic",
        "star_filled" => "starred-symbolic",
        "wifi" => "network-wireless-symbolic",
        "vpn" => "network-vpn-symbolic",
        "lan" => "network-wired-symbolic",
        "internet" => "web-browser-symbolic",
        "host" | "server" => "network-server-symbolic",
        "map" => "mark-location-symbolic",
        "mode" | "game" | "gamepad" => "input-gaming-symbolic",
        "bot" => "computer-symbolic",
        "ping" => "network-cellular-signal-good-symbolic",
        "add" => "list-add-symbolic",
        "remove" => "list-remove-symbolic",
        "edit" => "document-edit-symbolic",
        "delete" => "user-trash-symbolic",
        "download" => "folder-download-symbolic",
        "upload" | "share" => "send-to-symbolic",
        "update" => "software-update-available-symbolic",
        "info" => "dialog-information-symbolic",
        "warning" => "dialog-warning-symbolic",
        "error" => "dialog-error-symbolic",
        "check" => "object-select-symbolic",
        "link" => "insert-link-symbolic",
        "copy" => "edit-copy-symbolic",
        "mods" => "application-x-addon-symbolic",
        "video" => "video-display-symbolic",
        "audio" => "audio-volume-high-symbolic",
        "input" => "input-keyboard-symbolic",
        "touch" => "input-touchscreen-symbolic",
        "esp" => "view-reveal-symbolic",
        "privacy" => "security-high-symbolic",
        "data" => "drive-harddisk-symbolic",
        "menu" => "open-menu-symbolic",
        "mouse" => "input-mouse-symbolic",
        "folder" => "folder-symbolic",
        "file" => "text-x-generic-symbolic",
        "chevron" => "go-next-symbolic",
        _ => return None,
    })
}

/// Splits a byte stream into newline-delimited JSON objects (the Linux transport's framing).
#[derive(Default)]
pub struct LineFramer {
    buf: Vec<u8>,
    pub dropped: usize,
    pub max_line: usize,
}

impl LineFramer {
    pub fn new() -> Self {
        Self { buf: Vec::new(), dropped: 0, max_line: 8 << 20 }
    }

    /// Feeds bytes; returns the complete messages they ended. Lines that are not a JSON object are counted and skipped.
    pub fn feed(&mut self, bytes: &[u8]) -> Vec<Map<String, Value>> {
        let mut out = Vec::new();
        for &b in bytes {
            if b == b'\n' {
                let line = std::mem::take(&mut self.buf);
                let text = String::from_utf8_lossy(&line);
                let t = text.trim();
                if t.is_empty() {
                    continue;
                }
                match serde_json::from_str::<Value>(t) {
                    Ok(Value::Object(m)) => out.push(m),
                    _ => self.dropped += 1,
                }
            } else if self.buf.len() < self.max_line.max(1) {
                self.buf.push(b);
            }
        }
        out
    }
}

/// Typed reads from a JSON object that tolerate what the other side sends.
pub trait Props {
    fn str_of(&self, k: &str) -> Option<String>;
    fn f64_of(&self, k: &str) -> Option<f64>;
    fn bool_of(&self, k: &str) -> Option<bool>;
    fn i64_of(&self, k: &str) -> Option<i64> {
        self.f64_of(k).map(|v| v as i64)
    }
}

impl Props for Map<String, Value> {
    fn str_of(&self, k: &str) -> Option<String> {
        match self.get(k)? {
            Value::String(s) => Some(s.clone()),
            Value::Number(n) => Some(n.to_string()),
            Value::Bool(b) => Some(b.to_string()),
            _ => None,
        }
    }
    fn f64_of(&self, k: &str) -> Option<f64> {
        match self.get(k)? {
            Value::Number(n) => n.as_f64(),
            Value::String(s) => s.trim().parse().ok(),
            Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
            _ => None,
        }
    }
    fn bool_of(&self, k: &str) -> Option<bool> {
        match self.get(k)? {
            Value::Bool(b) => Some(*b),
            Value::Number(n) => n.as_f64().map(|v| v != 0.0),
            Value::String(s) => Some(s == "1" || s.eq_ignore_ascii_case("true")),
            _ => None,
        }
    }
}

/// Quake text: colour codes out (`^1`, `^xRGB`), the game font's private glyphs dropped.
pub fn plain_text(s: &str) -> String {
    let c: Vec<char> = s.chars().collect();
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    while i < c.len() {
        if c[i] == '^' && i + 1 < c.len() {
            if c[i + 1].is_ascii_digit() {
                i += 2;
                continue;
            }
            if c[i + 1] == 'x' && i + 4 < c.len() && c[i + 2..i + 5].iter().all(|x| x.is_ascii_hexdigit()) {
                i += 5;
                continue;
            }
            if c[i + 1] == '^' {
                out.push('^');
                i += 2;
                continue;
            }
        }
        let ch = c[i];
        if ch.is_control() {
            out.push(' ');
        } else if !('\u{E000}'..='\u{F8FF}').contains(&ch) && ch != '\u{FFFD}' {
            out.push(ch);
        }
        i += 1;
    }
    out.trim().to_string()
}

/// Two clicks or taps on the same row this close together are a double click.
pub const DOUBLE_CLICK: std::time::Duration = std::time::Duration::from_millis(400);

/// Choosing versus playing in a list whose `selection` is `single` or `multi` (docs/UI_BRIDGE.md 6.1): the
/// first click or tap on a row chooses it (`row`, kind `select`); a second one on the same
/// row within [`DOUBLE_CLICK`] plays it (kind `activate`). A third click starts over, and
/// a click on another row, or a slow second click, only chooses. Scrolling never clicks.
#[derive(Default)]
pub struct Clicks {
    last: Option<(String, String, std::time::Instant)>,
}

impl Clicks {
    /// The kind of `row` event a click on `row` of `list` at `now` sends.
    pub fn click(&mut self, list: &str, row: &str, now: std::time::Instant) -> &'static str {
        let double = self.last.as_ref().is_some_and(|(l, r, t)| l == list && r == row && now.duration_since(*t) <= DOUBLE_CLICK);
        self.last = if double { None } else { Some((list.to_string(), row.to_string(), now)) };
        if double { "activate" } else { "select" }
    }
}
