// SPDX-License-Identifier: GPL-3.0-or-later
//! The client against every fixture in tests/uibridge/fixtures: each one
//! deserializes into typed nodes and round-trips, its snapshot is consistent,
//! and for the seq-* fixtures, applying the diffs in order through the client
//! gives exactly the player's latest snapshot (docs/UI_BRIDGE.md 3.4).

use godnotic_core::bridge::fixture::{Fixture, Player};
use godnotic_core::bridge::{Change, Client, LineFramer, Node, Store};
use serde_json::{json, Map, Value};
use std::path::PathBuf;

fn dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../tests/uibridge/fixtures")
}

fn all() -> Vec<(String, Fixture)> {
    let mut v: Vec<_> = std::fs::read_dir(dir()).expect("fixtures dir").filter_map(|e| {
        let p = e.ok()?.path();
        (p.extension()? == "json").then(|| (p.file_name().unwrap().to_string_lossy().to_string(), Fixture::parse(&std::fs::read_to_string(&p).unwrap()).unwrap()))
    }).collect();
    v.sort_by(|a, b| a.0.cmp(&b.0));
    v
}

fn obj(s: &str) -> Map<String, Value> {
    serde_json::from_str::<Value>(s).unwrap().as_object().unwrap().clone()
}

#[test]
fn every_fixture_deserializes_and_round_trips() {
    let fx = all();
    assert!(fx.len() >= 20, "found {}", fx.len());
    for (name, f) in &fx {
        let nodes = f.snapshot["nodes"].as_array().unwrap();
        for n in nodes {
            let node: Node = serde_json::from_value(n.clone()).unwrap_or_else(|e| panic!("{name}: {e}: {n}"));
            // Round trip: what we hold serializes back to the same content (children/props default to empty).
            let back = serde_json::to_value(&node).unwrap();
            let mut want = n.clone();
            let w = want.as_object_mut().unwrap();
            w.entry("props").or_insert(json!({}));
            w.entry("children").or_insert(json!([]));
            assert_eq!(back, want, "{name}: {}", node.id);
        }
        let mut s = Store::new();
        let ch = s.apply(&f.snapshot);
        assert_eq!(ch, vec![Change::Snapshot], "{name}: snapshot not consistent");
        assert!(s.app().is_some(), "{name}: no app node");
        for id in s.stack().iter().chain(s.modal().iter()) {
            assert!(s.node(id).is_some(), "{name}: stack/modal id {id} missing");
        }
    }
}

#[test]
fn sequences_through_the_client_end_where_the_player_ends() {
    for (name, f) in all().into_iter().filter(|(n, _)| n.starts_with("seq-")) {
        let mut p = Player::new(f);
        let mut c = Client::new();
        let mut s = Store::new();
        let mut framer = LineFramer::new();
        // Over the wire, as NDJSON, to exercise the framing too.
        let mut wire = String::new();
        for m in p.handle(&obj(&c.hello("test", "0", json!({"nodes": godnotic_core::bridge::NODE_TYPES})))) {
            wire.push_str(&Value::Object(m).to_string());
            wire.push('\n');
        }
        while let Some((_, m)) = p.next_step() {
            wire.push_str(&Value::Object(m).to_string());
            wire.push('\n');
        }
        let mut need_sync = false;
        for chunk in wire.as_bytes().chunks(997) {
            for m in framer.feed(chunk) {
                need_sync |= s.apply(&m).contains(&Change::NeedSync);
            }
        }
        assert_eq!(framer.dropped, 0);
        assert!(!need_sync, "{name}: a diff did not apply");
        let latest = p.snapshot();
        let mut fresh = Store::new();
        fresh.apply(&latest);
        assert_eq!(s.to_snapshot_nodes(), fresh.to_snapshot_nodes(), "{name}: diffs in order != latest snapshot");
    }
}

#[test]
fn a_diff_on_the_wrong_base_asks_for_sync() {
    let (_, f) = all().into_iter().find(|(n, _)| n == "home.json").unwrap();
    let mut s = Store::new();
    s.apply(&f.snapshot);
    let rev = s.rev;
    let bad = obj(&format!(r#"{{"uib":1,"t":"diff","id":9,"base":{},"rev":{},"set":[],"del":[]}}"#, rev + 5, rev + 6));
    assert_eq!(s.apply(&bad), vec![Change::NeedSync]);
    assert_eq!(s.rev, rev);
    let dangling = obj(&format!(r#"{{"uib":1,"t":"diff","id":9,"base":{rev},"rev":{},"set":[{{"id":"app","type":"app","props":{{}},"children":["nowhere"]}}],"del":[]}}"#, rev + 1));
    assert!(s.apply(&dangling).contains(&Change::NeedSync));
    let other = obj(r#"{"uib":2,"t":"snapshot","nodes":[]}"#);
    assert!(matches!(s.apply(&other)[0], Change::Error { .. }));
}

#[test]
fn server_rows_window_sort_and_filter_through_the_player() {
    let (_, f) = all().into_iter().find(|(n, _)| n == "servers.json").unwrap();
    let total = f.rows["servers"].len();
    let mut p = Player::new(f);
    let mut c = Client::new();
    let mut s = Store::new();
    for m in p.handle(&obj(&c.hello("t", "0", json!({})))) {
        s.apply(&m);
    }
    let w = s.sources.entry("servers".into()).or_default();
    let reqs = w.need(0, 20);
    assert_eq!(reqs, vec![(0, 50)]);
    for m in p.handle(&obj(&c.window("servers", 0, 50, Some("ping"), false, None))) {
        let ch = s.apply(&m);
        assert!(matches!(&ch[0], Change::Rows { source, .. } if source == "servers"));
    }
    let w = &s.sources["servers"];
    assert_eq!(w.total(), total);
    let pings: Vec<f64> = (0..total).filter_map(|i| w.row(i)?["cells"]["ping"].as_f64()).collect();
    assert!(pings.windows(2).all(|x| x[0] <= x[1]), "sorted by ping: {pings:?}");
    for m in p.handle(&obj(&c.window("servers", 0, 50, None, false, Some("lan")))) {
        s.apply(&m);
    }
    assert!(s.sources["servers"].total() < total);
    // A value event comes back as a confirming diff.
    let ev = obj(&c.value("mp/bar/filters/lan", json!(true)));
    let ch: Vec<Change> = p.handle(&ev).iter().flat_map(|m| s.apply(m)).collect();
    assert_eq!(ch, vec![Change::Set(vec!["mp/bar/filters/lan".into()])]);
    assert_eq!(s.node("mp/bar/filters/lan").unwrap().bool("value"), Some(true));
}
