// SPDX-License-Identifier: GPL-3.0-or-later
//! The store (docs/UI_BRIDGE.md 3.4, 10a): a flat map of keyed nodes plus the
//! rev it holds, snapshots and diffs applied exactly as specified, list
//! windows per data source, image requests asked once. Every applied message
//! is reported as [`Change`]s for the renderer.

use super::window::{ListWindow, Splice};
use super::Props;
use serde::Deserialize;
use serde_json::{Map, Value};
use std::collections::{HashMap, HashSet};

/// A node: `{id, type, props, children, origin}`; `props` is decoded by type when used,
/// so an unknown type is kept, not an error (3.0).
#[derive(Clone, Debug, PartialEq, Deserialize, serde::Serialize)]
pub struct Node {
    pub id: String,
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default)]
    pub props: Map<String, Value>,
    #[serde(default)]
    pub children: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub origin: Option<String>,
}

impl Node {
    pub fn str(&self, k: &str) -> Option<String> {
        self.props.str_of(k)
    }
    pub fn f64(&self, k: &str) -> Option<f64> {
        self.props.f64_of(k)
    }
    pub fn bool(&self, k: &str) -> Option<bool> {
        self.props.bool_of(k)
    }
    pub fn obj(&self, k: &str) -> Option<&Map<String, Value>> {
        self.props.get(k)?.as_object()
    }
    pub fn arr(&self, k: &str) -> &[Value] {
        self.props.get(k).and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[])
    }
    /// title, else label.
    pub fn title(&self) -> String {
        self.str("title").or_else(|| self.str("label")).unwrap_or_default()
    }
    pub fn hidden(&self) -> bool {
        self.bool("hidden").unwrap_or(false)
    }
    pub fn enabled(&self) -> bool {
        self.bool("enabled").unwrap_or(true)
    }
    /// A mod described it (section 8).
    pub fn from_mod(&self) -> Option<&str> {
        self.origin.as_deref()?.strip_prefix("mod:")
    }
}

/// What a message changed.
#[derive(Clone, Debug, PartialEq)]
pub enum Change {
    /// The engine answered hello; `restarted` when its session differs from the last one.
    Hello { session: String, restarted: bool },
    /// A whole new tree: rebuild what is shown, keeping widgets whose ids survive.
    Snapshot,
    /// These nodes were inserted or replaced (parents before children).
    Set(Vec<String>),
    /// These ids are gone.
    Deleted(Vec<String>),
    /// A diff did not apply (base mismatch, unknown child): send `sync`.
    NeedSync,
    /// Rows of a data source changed: emit these splices on its list models.
    Rows { source: String, splices: Vec<Splice> },
    /// A data source has a new revision: re-request the visible window.
    Source { source: String, total: usize },
    /// An image the renderer asked for (`path`, or decoded `data`).
    Image { reference: String, path: Option<String>, data: Option<Vec<u8>> },
    /// An image could not be had: keep the placeholder.
    ImageMissing(String),
    Error { code: String, msg: String },
    Bye(String),
}

#[derive(Default)]
pub struct Store {
    nodes: HashMap<String, Node>,
    pub rev: i64,
    pub session: Option<String>,
    /// List windows by data source name.
    pub sources: HashMap<String, ListWindow>,
    asked_images: HashSet<String>,
    image_refs: HashMap<String, String>,
}

impl Store {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn node(&self, id: &str) -> Option<&Node> {
        self.nodes.get(id)
    }

    pub fn children(&self, id: &str) -> Vec<&Node> {
        self.nodes.get(id).map(|n| n.children.iter().filter_map(|c| self.nodes.get(c)).collect()).unwrap_or_default()
    }

    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }

    pub fn app(&self) -> Option<&Node> {
        self.nodes.get("app")
    }

    fn ids(&self, prop: &str) -> Vec<String> {
        self.app().map(|a| a.arr(prop).iter().filter_map(|v| v.as_str().map(str::to_string)).collect()).unwrap_or_default()
    }

    /// Screens, bottom to top.
    pub fn stack(&self) -> Vec<String> {
        self.ids("stack")
    }

    /// Sheets and dialogs over the stack, bottom to top.
    pub fn modal(&self) -> Vec<String> {
        self.ids("modal")
    }

    /// `menu`, `game` or `loading`.
    pub fn context(&self) -> String {
        self.app().and_then(|a| a.str("context")).unwrap_or_else(|| "menu".into())
    }

    /// Toasts currently in the tree (children of `app` of type toast).
    pub fn toasts(&self) -> Vec<&Node> {
        self.children("app").into_iter().filter(|n| n.kind == "toast").collect()
    }

    /// Every child id exists (3.4: true after a correct snapshot or diff).
    pub fn consistent(&self) -> bool {
        self.nodes.values().all(|n| n.children.iter().all(|c| self.nodes.contains_key(c)))
    }

    fn parse_nodes(v: Option<&Value>) -> Option<Vec<Node>> {
        let a = v?.as_array()?;
        Some(a.iter().filter_map(|n| serde_json::from_value::<Node>(n.clone()).ok()).collect())
    }

    /// Applies one engine message. Unknown types change nothing.
    pub fn apply(&mut self, msg: &Map<String, Value>) -> Vec<Change> {
        if msg.i64_of("uib").is_some_and(|v| v != 1) {
            return vec![Change::Error { code: "version".into(), msg: "protocol major mismatch".into() }];
        }
        match msg.str_of("t").unwrap_or_default().as_str() {
            "hello" => {
                let s = msg.str_of("session").unwrap_or_default();
                let restarted = self.session.as_ref().is_some_and(|old| *old != s);
                if restarted {
                    self.sources.clear();
                    self.asked_images.clear();
                    self.image_refs.clear();
                }
                self.session = Some(s.clone());
                vec![Change::Hello { session: s, restarted }]
            }
            "snapshot" => {
                let Some(nodes) = Self::parse_nodes(msg.get("nodes")) else { return vec![] };
                self.nodes = nodes.into_iter().map(|n| (n.id.clone(), n)).collect();
                self.rev = msg.i64_of("rev").unwrap_or(0);
                if self.consistent() { vec![Change::Snapshot] } else { vec![Change::Snapshot, Change::NeedSync] }
            }
            "diff" => {
                if msg.i64_of("base") != Some(self.rev) {
                    return vec![Change::NeedSync];
                }
                let del: Vec<String> = msg.get("del").and_then(Value::as_array).map(|a| a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect()).unwrap_or_default();
                let set = Self::parse_nodes(msg.get("set")).unwrap_or_default();
                for d in &del {
                    self.nodes.remove(d);
                }
                let ids: Vec<String> = set.iter().map(|n| n.id.clone()).collect();
                for n in set {
                    self.nodes.insert(n.id.clone(), n);
                }
                self.rev = msg.i64_of("rev").unwrap_or(self.rev);
                let mut out = Vec::new();
                if !del.is_empty() {
                    out.push(Change::Deleted(del));
                }
                if !ids.is_empty() {
                    out.push(Change::Set(ids));
                }
                if !self.consistent() {
                    out.push(Change::NeedSync);
                }
                out
            }
            "rows" => {
                let Some(source) = msg.str_of("source") else { return vec![] };
                let rows: Vec<Map<String, Value>> = msg.get("rows").and_then(Value::as_array).map(|a| a.iter().filter_map(|r| r.as_object().cloned()).collect()).unwrap_or_default();
                let total = msg.i64_of("total").map(|t| t.max(0) as usize);
                let offset = msg.i64_of("offset").unwrap_or(0).max(0) as usize;
                let splices = self.sources.entry(source.clone()).or_default().put(offset, rows, total);
                vec![Change::Rows { source, splices }]
            }
            "source" => {
                let Some(source) = msg.str_of("source") else { return vec![] };
                let total = msg.i64_of("total").unwrap_or(0).max(0) as usize;
                let w = self.sources.entry(source.clone()).or_default();
                let mut out = Vec::new();
                // Resize now (placeholders), mark everything stale so the visible window is asked again.
                let splices = w.put(0, vec![], Some(total));
                w.invalidate();
                if !splices.is_empty() {
                    out.push(Change::Rows { source: source.clone(), splices });
                }
                out.push(Change::Source { source, total });
                out
            }
            "image" => {
                let Some(r) = msg.str_of("ref") else { return vec![] };
                self.asked_images.remove(&r);
                if let Some(p) = msg.str_of("path") {
                    self.image_refs.insert(r.clone(), p);
                }
                let data = msg.str_of("data").and_then(|d| base64_decode(&d));
                vec![Change::Image { reference: r, path: msg.str_of("path"), data }]
            }
            "error" => {
                let code = msg.str_of("code").unwrap_or_default();
                let m = msg.str_of("msg").unwrap_or_default();
                // An image request that failed: the renderer keeps its placeholder.
                if code == "not_found" {
                    if let Some(r) = msg.str_of("ref") {
                        self.asked_images.remove(&r);
                        return vec![Change::ImageMissing(r)];
                    }
                }
                vec![Change::Error { code, msg: m }]
            }
            "bye" => vec![Change::Bye(msg.str_of("reason").unwrap_or_default())],
            _ => vec![],
        }
    }

    /// True the first time an image is wanted (send the request); false while pending.
    pub fn want_image(&mut self, r: &str) -> bool {
        self.asked_images.insert(r.to_string())
    }

    /// A path the engine already gave for this ref in this session.
    pub fn image_path(&self, r: &str) -> Option<&str> {
        self.image_refs.get(r).map(String::as_str)
    }

    /// The whole store as a snapshot message (tests: diffs applied in order equal the latest snapshot).
    pub fn to_snapshot_nodes(&self) -> Vec<Node> {
        let mut v: Vec<Node> = self.nodes.values().cloned().collect();
        v.sort_by(|a, b| a.id.cmp(&b.id));
        v
    }
}

/// Standard or URL-safe base64. None on bad input.
pub fn base64_decode(s: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0u32);
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            b'=' | b'\n' | b'\r' | b' ' => continue,
            _ => return None,
        } as u32;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    Some(out)
}
