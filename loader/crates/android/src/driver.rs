// SPDX-License-Identifier: GPL-3.0-or-later
//! The UI bridge read by the core, turned into the Android renderer's
//! operations (the Kotlin `UiRenderer` seam: create, update, remove, navigate,
//! dialog, toast, listRows, imageReady).
//!
//! The core's [`Store`] applies snapshots and diffs (docs/UI_BRIDGE.md 3.4);
//! this keeps which node views exist, which screens and dialogs are shown,
//! and emits the operations that bring the views in line, the same rules the
//! interim Kotlin reader (`UibClient`) follows: views are created when a
//! screen or dialog is shown, updated in place by a diff, and removed when
//! their node goes or nothing shows them. Everything crosses JNI as one JSON
//! document per call ([`Driver::feed`]), so the Kotlin side applies a batch
//! on the UI thread and nothing calls back into Java from Rust.

use godnotic_core::bridge::{Change, Client, Store};
use serde_json::{json, Map, Value};
use std::collections::HashSet;

/// Operations for the renderer and messages for the engine, from one call.
#[derive(Default)]
pub struct Batch {
    pub ops: Vec<Value>,
    pub send: Vec<String>,
}

impl Batch {
    pub fn to_json(&self, d: &Driver) -> String {
        json!({"ops": self.ops, "send": self.send, "state": d.state()}).to_string()
    }
}

#[derive(Default)]
pub struct Driver {
    pub store: Store,
    client: Client,
    built: HashSet<String>,
    shown_stack: Vec<String>,
    shown_modal: Vec<String>,
    /// Rows come from elsewhere (test builds generate them).
    pub ignore_rows: bool,
}

fn str_of(m: &Map<String, Value>, k: &str) -> String {
    match m.get(k) {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => n.to_string(),
        Some(Value::Bool(b)) => b.to_string(),
        _ => String::new(),
    }
}

fn int_of(m: &Map<String, Value>, k: &str) -> i64 {
    match m.get(k) {
        Some(Value::Number(n)) => n.as_f64().unwrap_or(0.0) as i64,
        Some(Value::String(s)) => s.parse().unwrap_or(0),
        _ => 0,
    }
}

impl Driver {
    pub fn new() -> Self {
        Self::default()
    }

    /// What the platform reads off the store after a batch: the top screen,
    /// its kind, whether back goes somewhere, the hero picture (touch_warm).
    pub fn state(&self) -> Value {
        let top = self.store.stack().last().cloned().unwrap_or_default();
        let kind = self.store.node(&top).and_then(|n| n.str("kind")).unwrap_or_default();
        let can_back = self.store.app().and_then(|a| a.bool("canBack")).unwrap_or(false);
        let hero = self
            .store
            .to_snapshot_nodes()
            .into_iter()
            .find(|n| n.kind == "hero")
            .and_then(|n| n.str("image"))
            .unwrap_or_default();
        json!({"top": top, "kind": kind, "canBack": can_back, "modal": self.store.modal(), "hero": hero, "rev": self.store.rev})
    }

    // ------------------------------------------------------------ to the engine

    pub fn hello(&mut self, caps: Value) -> String {
        self.client.hello("godnotic-android-rust", env!("CARGO_PKG_VERSION"), caps)
    }

    pub fn sync(&mut self) -> String {
        let have = self.store.rev;
        self.client.sync(have)
    }

    /// A UI event (section 6) as the Kotlin views report it: `ev` plus its fields.
    pub fn event(&mut self, ev: &str, body: &Map<String, Value>) -> String {
        let node = str_of(body, "node");
        match ev {
            "action" => self.client.action(&node, &str_of(body, "action"), body.get("payload").cloned()),
            "value" => self.client.value(&node, body.get("value").cloned().unwrap_or(Value::Null)),
            "submit" => self.client.submit(&node, body.get("value").cloned().unwrap_or(Value::Null)),
            "row" => {
                let kind = str_of(body, "kind");
                self.client.row(&node, &str_of(body, "row"), if kind.is_empty() { "activate" } else { &kind })
            }
            _ => self.client.simple(ev, if node.is_empty() { None } else { Some(&node) }),
        }
    }

    pub fn window(&mut self, source: &str, offset: usize, limit: usize, sort: &str, desc: bool, filter: &str) -> String {
        self.client.window(source, offset, limit, if sort.is_empty() { None } else { Some(sort) }, desc, Some(filter))
    }

    pub fn image(&mut self, r: &str, w: u32, h: u32) -> String {
        self.client.image(r, w, h)
    }

    // ------------------------------------------------------------ from the engine

    /// One engine message (JSON text): the operations it causes.
    pub fn feed(&mut self, text: &str) -> Batch {
        let mut b = Batch::default();
        let Ok(Value::Object(m)) = serde_json::from_str::<Value>(text) else { return b };
        match str_of(&m, "t").as_str() {
            "rows" => {
                if !self.ignore_rows {
                    b.ops.push(json!({"op": "rows", "source": str_of(&m, "source"), "offset": int_of(&m, "offset"),
                        "total": int_of(&m, "total"), "rows": m.get("rows").cloned().unwrap_or(json!([]))}));
                }
                return b;
            }
            "source" => {
                if !self.ignore_rows {
                    b.ops.push(json!({"op": "rows", "source": str_of(&m, "source"), "offset": 0, "total": int_of(&m, "total"), "rows": []}));
                }
                return b;
            }
            _ => {}
        }
        let changes = self.store.apply(&m);
        let mut need_sync = false;
        let mut snapshot = false;
        let mut set: Vec<String> = Vec::new();
        let mut deleted: Vec<String> = Vec::new();
        for c in changes {
            match c {
                Change::Snapshot => snapshot = true,
                Change::Set(ids) => set.extend(ids),
                Change::Deleted(ids) => deleted.extend(ids),
                Change::NeedSync => need_sync = true,
                Change::Image { reference, path, .. } => b.ops.push(json!({"op": "image", "ref": reference, "path": path.unwrap_or_default()})),
                Change::ImageMissing(r) => b.ops.push(json!({"op": "image", "ref": r, "path": ""})),
                _ => {}
            }
        }
        if need_sync {
            b.send.push(self.sync());
            return b;
        }
        if snapshot {
            self.rebuild_all(&mut b);
        } else if !set.is_empty() || !deleted.is_empty() {
            self.apply_changes(&set, &deleted, &mut b);
        }
        b
    }

    fn op_create(&mut self, b: &mut Batch, id: &str, parent: Option<&str>, index: i64) {
        let Some(n) = self.store.node(id).cloned() else { return };
        let hints = n.props.get("hints").cloned().unwrap_or(json!({}));
        b.ops.push(json!({"op": "create", "id": n.id, "parent": parent, "index": index, "type": n.kind, "props": n.props, "hints": hints}));
        self.built.insert(n.id.clone());
        for c in &n.children {
            self.op_create(b, c, Some(&n.id), -1);
        }
    }

    fn rebuild_all(&mut self, b: &mut Batch) {
        b.ops.push(json!({"op": "begin"}));
        let roots: Vec<String> = self.shown_stack.iter().chain(self.shown_modal.iter()).cloned().collect();
        let mut old: Vec<String> = self.built.iter().cloned().collect();
        old.sort();
        for id in old {
            if self.store.node(&id).is_none() || roots.contains(&id) {
                b.ops.push(json!({"op": "remove", "id": id}));
            }
        }
        self.built.clear();
        self.shown_stack.clear();
        self.shown_modal.clear();
        self.show_app(true, b);
        b.ops.push(json!({"op": "end"}));
    }

    fn drop_tree(&mut self, root: &str, b: &mut Batch) {
        if self.built.contains(root) {
            b.ops.push(json!({"op": "remove", "id": root}));
        }
        let gone: Vec<String> = self.built.iter().filter(|id| self.store.node(id).is_none() || id.as_str() == root).cloned().collect();
        for g in gone {
            self.built.remove(&g);
        }
    }

    fn show_app(&mut self, force: bool, b: &mut Batch) {
        if self.store.app().is_none() {
            return;
        }
        let stack: Vec<String> = self.store.stack().into_iter().filter(|s| self.store.node(s).is_some()).collect();
        let modal: Vec<String> = self.store.modal().into_iter().filter(|s| self.store.node(s).is_some()).collect();
        if force || stack != self.shown_stack {
            if let Some(top) = stack.last().cloned() {
                if !self.built.contains(&top) {
                    self.op_create(b, &top, None, -1);
                }
                let transition = if self.shown_stack.is_empty() {
                    "replace"
                } else if stack.len() > self.shown_stack.len() {
                    "push"
                } else if stack.len() < self.shown_stack.len() {
                    "pop"
                } else {
                    "replace"
                };
                b.ops.push(json!({"op": "navigate", "screen": top, "transition": transition}));
            }
            let old = std::mem::take(&mut self.shown_stack);
            for o in &old {
                if !stack.contains(o) {
                    self.drop_tree(o, b);
                }
            }
            self.shown_stack = stack;
        }
        if force || modal != self.shown_modal {
            let old = std::mem::take(&mut self.shown_modal);
            for o in &old {
                if !modal.contains(o) {
                    self.drop_tree(o, b);
                }
            }
            let top = modal.last().cloned();
            if let Some(t) = &top {
                if !self.built.contains(t) {
                    self.op_create(b, t, None, -1);
                }
            }
            b.ops.push(json!({"op": "dialog", "id": top}));
            self.shown_modal = modal;
        }
        let toasts: Vec<(String, String, String)> = self
            .store
            .toasts()
            .iter()
            .map(|t| (t.id.clone(), t.str("text").unwrap_or_default(), t.str("kind").unwrap_or_else(|| "info".into())))
            .collect();
        for (id, text, kind) in toasts {
            if self.built.insert(id) {
                b.ops.push(json!({"op": "toast", "text": text, "kind": kind}));
            }
        }
    }

    fn apply_changes(&mut self, set: &[String], deleted: &[String], b: &mut Batch) {
        b.ops.push(json!({"op": "begin"}));
        for id in deleted {
            if self.built.remove(id) {
                b.ops.push(json!({"op": "remove", "id": id}));
            }
        }
        for id in set {
            if id == "app" || !self.built.contains(id) {
                continue; // built when its parent shows it
            }
            let Some(n) = self.store.node(id).cloned() else { continue };
            b.ops.push(json!({"op": "update", "id": id, "props": n.props}));
            for (i, c) in n.children.iter().enumerate() {
                if !self.built.contains(c) {
                    self.op_create(b, c, Some(id), i as i64);
                }
            }
        }
        // views of nodes nothing refers to any more
        let all = self.store.to_snapshot_nodes();
        let referenced: HashSet<&str> = all.iter().flat_map(|n| n.children.iter().map(String::as_str)).collect();
        let shown: HashSet<String> = self.store.stack().into_iter().chain(self.store.modal()).collect();
        let mut orphans: Vec<String> = self
            .built
            .iter()
            .filter(|id| {
                id.as_str() != "app"
                    && !referenced.contains(id.as_str())
                    && !shown.contains(*id)
                    && !self.shown_stack.contains(id)
                    && !self.shown_modal.contains(id)
                    && self.store.node(id).is_some_and(|n| n.kind != "toast")
            })
            .cloned()
            .collect();
        orphans.sort();
        for o in orphans {
            self.built.remove(&o);
            b.ops.push(json!({"op": "remove", "id": o}));
        }
        self.show_app(false, b);
        b.ops.push(json!({"op": "end"}));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ops(b: &Batch) -> Vec<String> {
        b.ops.iter().map(|o| format!("{} {}", o["op"].as_str().unwrap(), o.get("id").or(o.get("screen")).and_then(Value::as_str).unwrap_or(""))).collect()
    }

    const SNAP: &str = r#"{"uib":1,"t":"snapshot","rev":1,"nodes":[
        {"id":"app","type":"app","props":{"stack":["home"],"modal":[]},"children":["home"]},
        {"id":"home","type":"screen","props":{"title":"Home"},"children":["home/hero","home/play"]},
        {"id":"home/hero","type":"hero","props":{"image":"pk3:maps/stormkeep"},"children":[]},
        {"id":"home/play","type":"button","props":{"label":"Play"},"children":[]}]}"#;

    #[test]
    fn snapshot_creates_the_top_screen_and_navigates() {
        let mut d = Driver::new();
        let b = d.feed(SNAP);
        assert_eq!(ops(&b), ["begin ", "create home", "create home/hero", "create home/play", "navigate home", "dialog ", "end "]);
        assert_eq!(d.state()["hero"], "pk3:maps/stormkeep");
        assert_eq!(d.state()["top"], "home");
    }

    #[test]
    fn diff_updates_in_place_and_removes_what_went() {
        let mut d = Driver::new();
        d.feed(SNAP);
        let b = d.feed(r#"{"uib":1,"t":"diff","base":1,"rev":2,"del":["home/hero"],
            "set":[{"id":"home","type":"screen","props":{"title":"Home"},"children":["home/play"]},
                   {"id":"home/play","type":"button","props":{"label":"Play now"},"children":[]}]}"#);
        assert_eq!(ops(&b), ["begin ", "remove home/hero", "update home", "update home/play", "end "]);
    }

    #[test]
    fn a_diff_on_the_wrong_base_asks_for_sync() {
        let mut d = Driver::new();
        d.feed(SNAP);
        let b = d.feed(r#"{"uib":1,"t":"diff","base":7,"rev":8,"del":[],"set":[]}"#);
        assert!(b.ops.is_empty());
        assert_eq!(b.send.len(), 1);
        assert!(b.send[0].contains("\"t\":\"sync\"") && b.send[0].contains("\"have\":1"));
    }

    #[test]
    fn push_then_pop_drops_the_screen() {
        let mut d = Driver::new();
        d.feed(SNAP);
        let b = d.feed(r#"{"uib":1,"t":"diff","base":1,"rev":2,"del":[],
            "set":[{"id":"app","type":"app","props":{"stack":["home","mp"],"modal":[],"canBack":true},"children":["home","mp"]},
                   {"id":"mp","type":"screen","props":{"title":"Servers"},"children":[]}]}"#);
        assert_eq!(ops(&b), ["begin ", "create mp", "navigate mp", "end "]);
        assert_eq!(d.state()["canBack"], true);
        let b = d.feed(r#"{"uib":1,"t":"diff","base":2,"rev":3,"del":["mp"],
            "set":[{"id":"app","type":"app","props":{"stack":["home"],"modal":[]},"children":["home"]}]}"#);
        assert_eq!(ops(&b), ["begin ", "remove mp", "navigate home", "end "]);
    }

    #[test]
    fn rows_and_events_pass_through() {
        let mut d = Driver::new();
        let b = d.feed(r#"{"uib":1,"t":"rows","source":"servers","offset":0,"total":2,"rows":[{"key":"a"},{"key":"b"}]}"#);
        assert_eq!(b.ops[0]["op"], "rows");
        assert_eq!(b.ops[0]["total"], 2);
        let mut body = Map::new();
        body.insert("node".into(), json!("mp/list"));
        body.insert("row".into(), json!("a"));
        body.insert("kind".into(), json!("select"));
        let m = d.event("row", &body);
        assert!(m.contains("\"ev\":\"row\"") && m.contains("\"kind\":\"select\""));
    }
}
