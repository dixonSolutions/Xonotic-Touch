// SPDX-License-Identifier: GPL-3.0-or-later
//! The loader's data download from the command line, for tests and timings
//! (scripts/test-datafetch.sh):
//!
//!   datafetch --url <.../Godnotic-data.zip> --dir <data dir> [--engine-dir <userdir>/data/data]
//!             [--astc|--dxt] [--connections N] [--first-play-only]
//!
//! Prints `first_play <seconds> <bytes on disk>` when the game may start and
//! `done <seconds>` at the end.

use godnotic_core::fetch::datasets::{self, Choice, Kind};
use godnotic_core::fetch::http_ureq::UreqHttp;
use godnotic_core::fetch::{CancelToken, Config, Progress};
use std::path::{Path, PathBuf};
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let val = |k: &str| a.iter().position(|x| x == k).and_then(|i| a.get(i + 1)).cloned();
    let (Some(url), Some(dir)) = (val("--url"), val("--dir")) else {
        eprintln!("usage: datafetch --url URL --dir DIR [--engine-dir DIR] [--astc|--dxt] [--connections N] [--first-play-only]");
        std::process::exit(2);
    };
    let dir = PathBuf::from(dir);
    let engine = val("--engine-dir").map(PathBuf::from);
    let astc = if a.iter().any(|x| x == "--dxt") { Some(false) } else { Some(true) };
    let mut cfg = Config::default();
    if let Some(n) = val("--connections").and_then(|n| n.parse().ok()) {
        cfg.connections = n;
    }
    let first_only = a.iter().any(|x| x == "--first-play-only");
    let http = UreqHttp::new();
    let choice = Choice { kind: Kind::Custom, url, reason: String::new(), automatic: false };
    let token = CancelToken::new();
    let start = Instant::now();
    let free = |_: &Path| u64::MAX;
    let progress = |_: Progress| {};
    let t2 = token.clone();
    let d2 = dir.clone();
    let ready = move || {
        let bytes: u64 = std::fs::read_dir(&d2).map(|it| it.filter_map(|e| e.ok()?.metadata().ok()).map(|m| m.len()).sum()).unwrap_or(0);
        println!("first_play {:.2} {bytes}", start.elapsed().as_secs_f64());
        if first_only {
            t2.cancel();
        }
    };
    match datasets::fetch_all(&http, &choice, astc, &dir, engine.as_deref(), cfg, &free, &token, &progress, &ready, datasets::FULL_URL) {
        Ok(()) => println!("done {:.2}", start.elapsed().as_secs_f64()),
        Err(godnotic_core::fetch::Error::Cancelled) if first_only => {}
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    }
}
