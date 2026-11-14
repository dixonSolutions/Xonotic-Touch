// SPDX-License-Identifier: GPL-3.0-or-later
//! Which archive the pk3s come from (the Kotlin launcher's DataSets.kt).

use super::manifest::{self, manifest_url, Manifest, Phase, Session, Status};
use super::{installed, plan, CancelToken, Config, Error, FreeSpace, Http, Meta, PackFetch, Plan, Progress, Result};
use std::sync::Mutex;
use std::time::SystemTime;
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

/// The split set's manifest for a choice (docs/DATA_FETCH.md): the ASTC one for the slim set,
/// the DXT one where the GPU has no ASTC and nobody chose the stock packs. `Ok(None)`: there is
/// none, take the single archive as before. A stored manifest stays in charge when the network fails.
pub fn split_manifest(http: &dyn Http, choice: &Choice, astc: Option<bool>, dir: &Path, token: &CancelToken) -> Result<Option<Manifest>> {
    let profile = match (choice.kind, choice.automatic) {
        (Kind::Slim, _) => "astc",
        (Kind::Full, true) => "dxt",
        (Kind::Custom, _) => if astc == Some(true) { "astc" } else { "dxt" },
        (Kind::Full, false) => return Ok(None),
    };
    let base = if choice.kind == Kind::Custom { choice.url.as_str() } else { SLIM_URL };
    match Manifest::fetch(http, &manifest_url(base, profile), token) {
        Ok(Some(m)) => {
            m.store(dir);
            Ok(Some(m))
        }
        Ok(None) => Ok(Manifest::stored(dir)),
        Err(Error::Cancelled) => Err(Error::Cancelled),
        Err(e) => Manifest::stored(dir).map(Some).ok_or(e),
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

/// A split install that is not whole yet: a front end carries it on at its start without asking.
pub fn split_unfinished(dir: &Path) -> bool {
    Manifest::stored(dir).is_some_and(|m| {
        let i = manifest::Install::read(dir);
        i.set.as_deref() == Some(m.set.as_str()) && !m.packs.iter().all(|p| i.has(dir, p))
    })
}

/// The whole download for a front end, in one call.
///
/// With a split-set manifest: `first_play`, then `first_play_ready` (the game may start; the
/// shell scripts' marker is written), then everything else by priority while the status file in
/// `engine_write_dir` (`<engine userdir>/data/data`) tells a running game what landed and its
/// request file says which map the player waits for. Without one: the single archive, as before.
#[allow(clippy::too_many_arguments)]
pub fn fetch_all(http: &dyn Http, choice: &Choice, astc: Option<bool>, dir: &Path, engine_write_dir: Option<&Path>, cfg: Config, free_space: &FreeSpace, token: &CancelToken,
    progress: &(dyn Fn(Progress) + Send + Sync), first_play_ready: &(dyn Fn() + Send + Sync), full_url: &str) -> Result<()> {
    let Some(m) = split_manifest(http, choice, astc, dir, token)? else {
        let (_, plan) = plan_choice(http, choice, token, full_url)?;
        PackFetch::new(http, dir, &plan, cfg, free_space, progress).run(token, true)?;
        mark_installed(dir, &plan);
        first_play_ready();
        return Ok(());
    };
    let started = SystemTime::now();
    let last = Mutex::new(String::new());
    let phase = Mutex::new(Phase::FirstPlay);
    let listen = |s: &Status| {
        // the boot phase shows first_play alone; afterwards the whole set
        let total = if *phase.lock().unwrap_or_else(|e| e.into_inner()) == Phase::FirstPlay { m.first_play_size() } else { s.total };
        progress(Progress { done: s.done.min(total), total, file: None, verifying: false });
        if let Some(e) = engine_write_dir {
            let text = s.text();
            let mut l = last.lock().unwrap_or_else(|e| e.into_inner());
            if *l != text {
                s.write(e);
                *l = text;
            }
        }
    };
    let want = || engine_write_dir.and_then(|e| manifest::read_request(e, started)).map(|(map, _)| map);
    let session = Session { http, dir: dir.into(), manifest: &m, cfg, free_space, listener: &listen, want: &want };
    if !Session::playable(dir, &m) {
        session.run(Phase::FirstPlay, token)?;
    }
    super::write_atomic(&dir.join(READY_MARKER), b"");
    first_play_ready();
    *phase.lock().unwrap_or_else(|e| e.into_inner()) = Phase::Everything;
    session.run(Phase::Everything, token)?;
    if m.packs.iter().any(|p| p.kind == "music") {
        let _ = std::fs::create_dir_all(dir.join("touch"));
        super::write_atomic(&dir.join(MUSIC_MARKER), b"");
    }
    Ok(())
}
