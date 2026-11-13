// SPDX-License-Identifier: GPL-3.0-or-later
//! Which archive the pk3s come from (the Kotlin launcher's DataSets.kt).

use super::{installed, plan, CancelToken, Error, Http, Meta, Plan, Result};
use std::path::Path;

pub const SLIM_URL: &str = "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/Godnotic-data.zip";
pub const FULL_URL: &str = "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107/Godnotic-data-20261107.zip";
pub const ENV: &str = "XT_DATA_SET";
/// The marker packaging/start.sh and the fetch scripts treat as "the packs are installed".
pub const READY_MARKER: &str = ".assets-ready";
pub const MUSIC_MARKER: &str = "touch/music-ready.txt";
pub const FOLDER: &str = "Xonotic/data/";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind { Slim, Full, Custom }

#[derive(Clone, Debug, PartialEq)]
pub struct Choice {
    pub kind: Kind,
    pub url: String,
    pub reason: String,
    pub automatic: bool,
}

/// `setting`: launcher.conf's data_set ("", "slim", "full"); `env`: XT_DATA_SET, which wins;
/// `astc`: whether the GPU draws ASTC textures (None: not known).
pub fn choose(setting: Option<&str>, env: Option<&str>, astc: Option<bool>, gpu: Option<&str>) -> Choice {
    let c = |kind, url: &str, reason: String, automatic| Choice { kind, url: url.into(), reason, automatic };
    let e = env.unwrap_or("").trim();
    if e.eq_ignore_ascii_case("slim") { return c(Kind::Slim, SLIM_URL, format!("{ENV}=slim"), false); }
    if e.eq_ignore_ascii_case("full") { return c(Kind::Full, FULL_URL, format!("{ENV}=full"), false); }
    if e.starts_with("http://") || e.starts_with("https://") { return c(Kind::Custom, e, format!("{ENV} is set"), false); }
    match setting.map(|s| s.trim().to_ascii_lowercase()).as_deref() {
        Some("slim") => return c(Kind::Slim, SLIM_URL, "Chosen in Settings".into(), false),
        Some("full") => return c(Kind::Full, FULL_URL, "Chosen in Settings".into(), false),
        _ => {}
    }
    let name = gpu.map(|g| format!(" ({g})")).unwrap_or_default();
    match astc {
        Some(true) => c(Kind::Slim, SLIM_URL, format!("Your GPU{name} draws ASTC textures"), true),
        Some(false) => c(Kind::Full, FULL_URL, format!("Your GPU{name} has no ASTC texture support"), true),
        None => c(Kind::Full, FULL_URL, "GPU not checked; the full set works everywhere".into(), true),
    }
}

/// Plans a choice; an automatic slim choice falls back to the full set when the slim archive is missing (404).
pub fn plan_choice(http: &dyn Http, choice: &Choice, token: &CancelToken, full_url: &str) -> Result<(Choice, Plan)> {
    match plan(http, &choice.url, token, FOLDER) {
        Err(Error::Http { code: 404, .. }) if choice.automatic && choice.kind == Kind::Slim => {
            let full = Choice { kind: Kind::Full, url: full_url.into(), reason: "The slim set is not on the server yet; using the full set".into(), automatic: true };
            let p = plan(http, full_url, token, FOLDER)?;
            Ok((full, p))
        }
        r => r.map(|p| (choice.clone(), p)),
    }
}

/// After a run: the marker the shell scripts look for, and the menu's music flag.
pub fn mark_installed(dir: &Path, plan: &Plan) {
    if plan.entries.iter().any(|e| !installed(dir, &e.role())) {
        return;
    }
    super::write_atomic(&dir.join(READY_MARKER), b"");
    if plan.entries.iter().any(|e| e.is_music()) {
        let _ = std::fs::create_dir_all(dir.join("touch"));
        super::write_atomic(&dir.join(MUSIC_MARKER), b"");
    }
}

/// A setup that stopped part-way: what it wants that is not installed yet.
pub fn unfinished(dir: &Path) -> Vec<String> {
    let m = Meta::read(dir);
    if m.identity.is_none() { vec![] } else { m.missing(dir) }
}
