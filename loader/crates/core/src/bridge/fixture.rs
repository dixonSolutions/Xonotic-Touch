// SPDX-License-Identifier: GPL-3.0-or-later
//! The fixtures of tests/uibridge/fixtures played in-process, standing in for
//! the engine the way scripts/uib-replay.py does over a stream: it answers
//! `hello` with `hello` + the snapshot, `window` from the fixture's rows
//! (substring filter, sort, window), `value` with a confirming diff, and
//! `image` from an image resolver (a placeholder when there is none). The
//! loader uses it for `--fixture` (development, screenshots); tests use it
//! to check the client end to end.

use super::Props;
use serde_json::{json, Map, Value};
use std::collections::HashMap;

pub struct Fixture {
    pub name: String,
    pub snapshot: Map<String, Value>,
    /// (delay in seconds, message) after the snapshot.
    pub steps: Vec<(f64, Map<String, Value>)>,
    pub rows: HashMap<String, Vec<Value>>,
}

impl Fixture {
    pub fn parse(text: &str) -> Result<Self, String> {
        let v: Value = serde_json::from_str(text).map_err(|e| e.to_string())?;
        let o = v.as_object().ok_or("not an object")?;
        let snapshot = o.get("snapshot").and_then(Value::as_object).cloned().ok_or("no snapshot")?;
        let steps = o.get("steps").and_then(Value::as_array).map(|a| a.iter().filter_map(|s| {
            Some((s.get("delay").and_then(Value::as_f64).unwrap_or(0.0), s.get("msg")?.as_object()?.clone()))
        }).collect()).unwrap_or_default();
        let rows = o.get("rows").and_then(Value::as_object).map(|r| r.iter().filter_map(|(k, v)| Some((k.clone(), v.as_array()?.clone()))).collect()).unwrap_or_default();
        Ok(Self { name: o.get("name").and_then(Value::as_str).unwrap_or("?").into(), snapshot, steps, rows })
    }
}

/// Resolves an image ref to bytes and a mime type (the fixture player has no engine filesystem).
pub type ImageResolver = dyn Fn(&str, u32, u32) -> Option<(Vec<u8>, String)>;

pub struct Player {
    pub fixture: Fixture,
    nodes: HashMap<String, Value>,
    order: Vec<String>,
    rev: i64,
    id: i64,
    step: usize,
    totals: HashMap<String, usize>,
    pub images: Option<Box<ImageResolver>>,
}

impl Player {
    pub fn new(fixture: Fixture) -> Self {
        let mut p = Self { fixture, nodes: HashMap::new(), order: vec![], rev: 0, id: 0, step: 0, totals: HashMap::new(), images: None };
        p.reset();
        p
    }

    fn reset(&mut self) {
        let snap = self.fixture.snapshot.clone();
        self.load_snapshot(&snap);
        self.step = 0;
    }

    fn load_snapshot(&mut self, snap: &Map<String, Value>) {
        self.nodes.clear();
        self.order.clear();
        for n in snap.get("nodes").and_then(Value::as_array).into_iter().flatten() {
            if let Some(id) = n.get("id").and_then(Value::as_str) {
                self.order.push(id.into());
                self.nodes.insert(id.into(), n.clone());
            }
        }
        self.rev = snap.i64_of("rev").unwrap_or(1);
    }

    fn mid(&mut self) -> i64 {
        self.id += 1;
        self.id
    }

    pub fn snapshot(&mut self) -> Map<String, Value> {
        // Parents before children, from app.
        let mut out = Vec::new();
        let mut stack = vec!["app".to_string()];
        let mut seen = std::collections::HashSet::new();
        while let Some(i) = stack.pop() {
            let Some(n) = self.nodes.get(&i) else { continue };
            if !seen.insert(i.clone()) {
                continue;
            }
            out.push(n.clone());
            if let Some(c) = n.get("children").and_then(Value::as_array) {
                for k in c.iter().rev() {
                    if let Some(s) = k.as_str() {
                        stack.push(s.into());
                    }
                }
            }
        }
        let id = self.mid();
        json!({"uib": 1, "t": "snapshot", "id": id, "rev": self.rev, "root": "app", "nodes": out}).as_object().cloned().unwrap_or_default()
    }

    /// The next scripted step, if any: (delay, message), already rebased onto the player's rev.
    pub fn next_step(&mut self) -> Option<(f64, Map<String, Value>)> {
        let (delay, msg) = self.fixture.steps.get(self.step)?.clone();
        self.step += 1;
        Some((delay, self.apply_step(msg)))
    }

    fn apply_step(&mut self, mut m: Map<String, Value>) -> Map<String, Value> {
        match m.str_of("t").as_deref() {
            Some("diff") => {
                m.insert("base".into(), json!(self.rev));
                m.insert("rev".into(), json!(self.rev + 1));
                for d in m.get("del").and_then(Value::as_array).into_iter().flatten() {
                    if let Some(d) = d.as_str() {
                        self.nodes.remove(d);
                    }
                }
                for n in m.get("set").and_then(Value::as_array).into_iter().flatten() {
                    if let Some(id) = n.get("id").and_then(Value::as_str) {
                        self.nodes.insert(id.into(), n.clone());
                    }
                }
                self.rev += 1;
            }
            Some("snapshot") => {
                let mm = m.clone();
                self.load_snapshot(&mm);
            }
            Some("source") => {
                if let (Some(s), Some(t)) = (m.str_of("source"), m.i64_of("total")) {
                    self.totals.insert(s, t.max(0) as usize);
                }
            }
            _ => {}
        }
        let id = self.mid();
        m.insert("id".into(), json!(id));
        m
    }

    /// Answers one renderer message.
    pub fn handle(&mut self, m: &Map<String, Value>) -> Vec<Map<String, Value>> {
        let re = m.i64_of("id").unwrap_or(0);
        let obj = |v: Value| v.as_object().cloned().unwrap_or_default();
        match m.str_of("t").as_deref() {
            Some("hello") => {
                self.reset();
                let id = self.mid();
                let hello = obj(json!({"uib": 1, "t": "hello", "id": id, "re": re, "schema": super::SCHEMA,
                    "engine": format!("fixture player ({})", self.fixture.name), "session": "fixture",
                    "limits": {"window": 200}, "features": ["rows", "image.path", "image.bytes", "replay"]}));
                vec![hello, self.snapshot()]
            }
            Some("sync") => vec![self.snapshot()],
            Some("window") => vec![self.rows(m, re)],
            Some("image") => vec![self.image(m, re)],
            Some("ev") if m.str_of("ev").as_deref() == Some("value") => {
                let node = m.str_of("node").unwrap_or_default();
                let Some(mut n) = self.nodes.get(&node).cloned().filter(|n| n.pointer("/props/bind").is_some()) else {
                    let id = self.mid();
                    return vec![obj(json!({"uib": 1, "t": "error", "id": id, "re": re, "code": "denied", "msg": "no bound control with that id"}))];
                };
                n["props"]["value"] = m.get("value").cloned().unwrap_or(Value::Null);
                self.nodes.insert(node, n.clone());
                self.rev += 1;
                let id = self.mid();
                vec![obj(json!({"uib": 1, "t": "diff", "id": id, "base": self.rev - 1, "rev": self.rev, "set": [n], "del": []}))]
            }
            // A row was chosen in a list that tells choosing from activating: like the engine,
            // the list's `selected` becomes that row, and the buttons of that screen that were
            // waiting for a choice (`enabled: false`, the one Join) come on.
            Some("ev") if m.str_of("ev").as_deref() == Some("row") && m.str_of("kind").as_deref() == Some("select") => {
                let (node, row) = (m.str_of("node").unwrap_or_default(), m.str_of("row").unwrap_or_default());
                let Some(mut n) = self.nodes.get(&node).cloned().filter(|n| matches!(n.pointer("/props/selection").and_then(Value::as_str), Some("single") | Some("multi"))) else {
                    let id = self.mid();
                    return vec![obj(json!({"uib": 1, "t": "error", "id": id, "re": re, "code": "denied", "msg": "no list with a selection has that id"}))];
                };
                n["props"]["selected"] = json!([row]);
                let mut set = vec![n.clone()];
                self.nodes.insert(node.clone(), n);
                let screen = format!("{}/", node.split('/').next().unwrap_or_default());
                let waiting: Vec<String> = self.nodes.iter().filter(|(id, b)| id.starts_with(&screen) && b["type"] == "button" && b.pointer("/props/enabled") == Some(&json!(false))).map(|(id, _)| id.clone()).collect();
                for id in waiting {
                    if let Some(b) = self.nodes.get_mut(&id) {
                        b["props"]["enabled"] = json!(true);
                        set.push(b.clone());
                    }
                }
                self.rev += 1;
                let id = self.mid();
                vec![obj(json!({"uib": 1, "t": "diff", "id": id, "base": self.rev - 1, "rev": self.rev, "set": set, "del": []}))]
            }
            _ => vec![],
        }
    }

    fn rows(&mut self, req: &Map<String, Value>, re: i64) -> Map<String, Value> {
        let src = req.str_of("source").unwrap_or_default();
        let id = self.mid();
        let Some(all) = self.fixture.rows.get(&src) else {
            return json!({"uib": 1, "t": "error", "id": id, "re": re, "code": "not_found", "msg": format!("no rows for source {src}")}).as_object().cloned().unwrap_or_default();
        };
        let cap = self.totals.get(&src).copied().unwrap_or(all.len());
        let flt = req.str_of("filter").unwrap_or_default().to_lowercase();
        let off = req.i64_of("offset").unwrap_or(0).max(0) as usize;
        let lim = req.i64_of("limit").unwrap_or(50).clamp(0, 200) as usize;
        if flt.is_empty() && req.str_of("sort").is_none() {
            // The common case: a plain window, no copy of the whole source.
            let total = cap.min(all.len());
            let page: Vec<Value> = all.iter().take(total).skip(off).take(lim).cloned().collect();
            return json!({"uib": 1, "t": "rows", "id": id, "re": re, "source": src, "rev": 1, "total": total, "offset": off, "rows": page}).as_object().cloned().unwrap_or_default();
        }
        let mut rows: Vec<Value> = all.iter().take(cap).cloned().collect();
        if !flt.is_empty() {
            rows.retain(|r| r.get("cells").and_then(Value::as_object).is_some_and(|c| c.values().any(|v| cell_text(v).to_lowercase().contains(&flt))));
        }
        if let Some(sort) = req.str_of("sort") {
            let desc = req.bool_of("desc").unwrap_or(false);
            rows.sort_by(|a, b| {
                let (x, y) = (a.pointer(&format!("/cells/{sort}")), b.pointer(&format!("/cells/{sort}")));
                let o = match (x.and_then(Value::as_f64), y.and_then(Value::as_f64)) {
                    (Some(p), Some(q)) => p.partial_cmp(&q).unwrap_or(std::cmp::Ordering::Equal),
                    _ => cell_text(x.unwrap_or(&Value::Null)).to_lowercase().cmp(&cell_text(y.unwrap_or(&Value::Null)).to_lowercase()),
                };
                if desc { o.reverse() } else { o }
            });
        }
        let total = rows.len();
        let page: Vec<Value> = rows.into_iter().skip(off).take(lim).collect();
        json!({"uib": 1, "t": "rows", "id": id, "re": re, "source": src, "rev": 1, "total": total, "offset": off, "rows": page}).as_object().cloned().unwrap_or_default()
    }

    fn image(&mut self, req: &Map<String, Value>, re: i64) -> Map<String, Value> {
        let r = req.str_of("ref").unwrap_or_default();
        let (w, h) = (req.i64_of("w").unwrap_or(320) as u32, req.i64_of("h").unwrap_or(180) as u32);
        let id = self.mid();
        match self.images.as_ref().and_then(|f| f(&r, w, h)) {
            Some((bytes, mime)) => json!({"uib": 1, "t": "image", "id": id, "re": re, "ref": r, "mime": mime, "w": w, "h": h, "data": base64_encode(&bytes)}),
            None => json!({"uib": 1, "t": "error", "id": id, "re": re, "ref": r, "code": "not_found", "msg": "no such image"}),
        }.as_object().cloned().unwrap_or_default()
    }
}

fn cell_text(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Null => String::new(),
        x => x.to_string(),
    }
}

pub fn base64_encode(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(b.len().div_ceil(3) * 4);
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        s.push(T[(n >> 18) as usize & 63] as char);
        s.push(T[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        s.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    s
}

/// Reads single files out of pk3s (stored or deflated entries), so the fixture
/// player can answer `pk3:` image refs with the real map previews when game
/// data is installed. The engine does this itself in a real session.
pub mod pk3 {
    use std::fs::File;
    use std::io::{Read, Seek, SeekFrom};
    use std::path::Path;

    pub struct Entry {
        pub name: String,
        method: u16,
        csize: u64,
        size: u64,
        local: u64,
    }

    fn le16(b: &[u8], o: usize) -> u64 {
        u16::from_le_bytes([b[o], b[o + 1]]) as u64
    }
    fn le32(b: &[u8], o: usize) -> u64 {
        u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) as u64
    }

    pub fn entries(path: &Path) -> Vec<Entry> {
        let Ok(mut f) = File::open(path) else { return vec![] };
        let Ok(size) = f.seek(SeekFrom::End(0)) else { return vec![] };
        let tail_len = size.min(65557);
        let mut tail = vec![0u8; tail_len as usize];
        if f.seek(SeekFrom::Start(size - tail_len)).is_err() || f.read_exact(&mut tail).is_err() || tail.len() < 22 {
            return vec![];
        }
        let Some(e) = (0..=tail.len() - 22).rev().find(|&i| le32(&tail, i) == 0x0605_4b50) else { return vec![] };
        let (cd_size, cd_off) = (le32(&tail, e + 12), le32(&tail, e + 16));
        if cd_off + cd_size > size || cd_size > 64 << 20 {
            return vec![];
        }
        let mut cd = vec![0u8; cd_size as usize];
        if f.seek(SeekFrom::Start(cd_off)).is_err() || f.read_exact(&mut cd).is_err() {
            return vec![];
        }
        let mut out = Vec::new();
        let mut p = 0;
        while p + 46 <= cd.len() && le32(&cd, p) == 0x0201_4b50 {
            let (n, x, k) = (le16(&cd, p + 28) as usize, le16(&cd, p + 30) as usize, le16(&cd, p + 32) as usize);
            if p + 46 + n > cd.len() {
                break;
            }
            out.push(Entry { name: String::from_utf8_lossy(&cd[p + 46..p + 46 + n]).into(), method: le16(&cd, p + 10) as u16, csize: le32(&cd, p + 20), size: le32(&cd, p + 24), local: le32(&cd, p + 42) });
            p += 46 + n + x + k;
        }
        out
    }

    pub fn read(path: &Path, e: &Entry) -> Option<Vec<u8>> {
        if e.size > 32 << 20 {
            return None;
        }
        let mut f = File::open(path).ok()?;
        let mut lh = [0u8; 30];
        f.seek(SeekFrom::Start(e.local)).ok()?;
        f.read_exact(&mut lh).ok()?;
        if le32(&lh, 0) != 0x0403_4b50 {
            return None;
        }
        f.seek(SeekFrom::Current((le16(&lh, 26) + le16(&lh, 28)) as i64)).ok()?;
        let mut data = vec![0u8; e.csize as usize];
        f.read_exact(&mut data).ok()?;
        match e.method {
            0 => Some(data),
            8 => miniz_oxide::inflate::decompress_to_vec_with_limit(&data, e.size as usize).ok(),
            _ => None,
        }
    }
}
