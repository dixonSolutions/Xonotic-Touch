// SPDX-License-Identifier: GPL-3.0-or-later
//! Every fixture of tests/uibridge/fixtures through the Android driver: the
//! snapshot shows the top screen, every step applies without a sync, and the
//! operations stay consistent (a node is created before it is updated or
//! removed, never removed twice, never created twice while it exists).

use godnotic_android::driver::Driver;
use serde_json::Value;
use std::collections::HashSet;
use std::path::PathBuf;

fn fixtures() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../tests/uibridge/fixtures")
}

fn check(name: &str, ops: &[Value], live: &mut HashSet<String>) {
    for o in ops {
        let id = o
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        match o["op"].as_str().unwrap_or("") {
            "create" => assert!(live.insert(id.clone()), "{name}: {id} created twice"),
            "update" => assert!(
                live.contains(&id),
                "{name}: {id} updated before it was created"
            ),
            "remove" => {
                assert!(live.remove(&id), "{name}: {id} removed while not shown");
            }
            "navigate" => {
                let s = o["screen"].as_str().unwrap_or("");
                assert!(live.contains(s), "{name}: navigate to {s} that has no view");
            }
            _ => {}
        }
    }
}

#[test]
fn every_fixture_plays_through_the_driver() {
    let mut n = 0;
    let mut dir: Vec<_> = std::fs::read_dir(fixtures())
        .expect("fixtures")
        .flatten()
        .map(|e| e.path())
        .collect();
    dir.sort();
    for path in dir {
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        let doc: Value = serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap();
        let Some(snap) = doc.get("snapshot") else {
            continue;
        };
        let mut d = Driver::new();
        let mut live = HashSet::new();
        let b = d.feed(&snap.to_string());
        assert!(b.send.is_empty(), "{name}: the snapshot asked for a sync");
        check(&name, &b.ops, &mut live);
        let top = d.state()["top"].as_str().unwrap_or("").to_string();
        assert!(
            top.is_empty() || live.contains(&top),
            "{name}: top screen {top} not built"
        );
        for (i, step) in doc
            .get("steps")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
            .iter()
            .enumerate()
        {
            let Some(msg) = step.get("msg") else { continue };
            let b = d.feed(&msg.to_string());
            assert!(
                b.send.is_empty(),
                "{name}: step {i} did not apply (sync asked)"
            );
            check(&format!("{name} step {i}"), &b.ops, &mut live);
        }
        n += 1;
    }
    assert!(n >= 20, "only {n} fixtures found");
}
