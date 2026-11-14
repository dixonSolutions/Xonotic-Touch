// SPDX-License-Identifier: GPL-3.0-or-later
//! The split data set (docs/DATA_FETCH.md): a manifest of packs inside one
//! stored archive, fetched by byte range in priority order, so the game can
//! start after the `first_play` packs and the rest follows while it runs.
//!
//! Same files as the Android app (`<install_as>.part`, `.part.state`,
//! `.godnotic-set`, `.godnotic-data.json`, the status and request files), so
//! either can carry on what the other started.

use super::sha256::Sha256;
use super::{crc32, list, range_headers, role, write_at, write_atomic, CancelToken, ChunkState, Config, Error, FreeSpace, Http, Result};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

/// The accepted manifest, kept beside the packs.
pub const STORED: &str = ".godnotic-data.json";
/// What was verified: `set`, `legacy`, `pack <file> <size> <crc32>` lines.
pub const STATE: &str = ".godnotic-set";
pub const STATUS_FILE: &str = "touch/data-status.txt";
pub const REQUEST_FILE: &str = "touch/data-request.txt";
/// A pack that fails its check this many times is given up for the run.
pub const CHECKS: u32 = 3;

#[derive(Clone, Debug, PartialEq)]
pub struct Pack {
    pub name: String,
    pub install_as: String,
    pub kind: String,
    pub size: u64,
    pub sha256: String,
    pub crc32: u32,
    pub priority: i64,
    pub offset: u64,
    pub depends: Vec<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct MapNeeds {
    pub pack: String,
    pub needs: Vec<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Manifest {
    pub set: String,
    pub default_map: String,
    pub first_play: Vec<String>,
    pub maps: BTreeMap<String, MapNeeds>,
    pub packs: Vec<Pack>,
    pub archive_url: String,
    pub archive_size: u64,
    /// The text it was parsed from, stored as it came.
    pub text: String,
}

fn word(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

fn hex(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// `.../data-slim/Godnotic-data.zip` + `astc` -> `.../data-slim/godnotic-data-astc.json`.
pub fn manifest_url(archive_url: &str, profile: &str) -> String {
    let dir = archive_url.rsplit_once('/').map(|(d, _)| d).unwrap_or(archive_url);
    format!("{dir}/godnotic-data-{profile}.json")
}

impl Manifest {
    /// Parses and checks a manifest; `Err` says why it cannot be used (the
    /// caller then takes the single archive, as before the split).
    pub fn parse(text: &str) -> std::result::Result<Self, String> {
        let v: serde_json::Value = serde_json::from_str(text).map_err(|e| format!("not JSON: {e}"))?;
        let o = v.as_object().ok_or("not an object")?;
        if o.get("format").and_then(|f| f.as_u64()) != Some(1) {
            return Err("unknown format".into());
        }
        let s = |v: Option<&serde_json::Value>| v.and_then(|x| x.as_str()).map(str::to_string);
        let strs = |v: Option<&serde_json::Value>, what: &str| -> std::result::Result<Vec<String>, String> {
            v.and_then(|x| x.as_array()).ok_or(format!("{what} is not a list"))?.iter().map(|x| x.as_str().map(str::to_string).ok_or(format!("{what} has a non-string"))).collect()
        };
        let set = s(o.get("set")).filter(|x| word(x)).ok_or("no set id")?;
        let archive = o.get("archive").and_then(|a| a.as_object()).ok_or("no archive")?;
        let archive_url = s(archive.get("url")).filter(|u| u.starts_with("https://") || u.starts_with("http://")).ok_or("archive.url is not an http(s) URL")?;
        let archive_size = archive.get("size").and_then(|x| x.as_u64()).filter(|n| *n > 0).ok_or("archive.size")?;
        let mut packs = Vec::new();
        let (mut names, mut files) = (BTreeSet::new(), BTreeSet::new());
        for p in o.get("packs").and_then(|p| p.as_array()).ok_or("no packs")? {
            let name = s(p.get("name")).filter(|x| word(x)).ok_or("a pack without a name")?;
            let install_as = s(p.get("install_as")).filter(|x| word(x) && x.starts_with("godnotic-") && x.ends_with(".pk3")).ok_or(format!("{name}: install_as"))?;
            let size = p.get("size").and_then(|x| x.as_u64()).filter(|n| *n > 0).ok_or(format!("{name}: size"))?;
            let offset = p.get("offset").and_then(|x| x.as_u64()).ok_or(format!("{name}: offset"))?;
            if offset.checked_add(size).is_none_or(|end| end > archive_size) {
                return Err(format!("{name}: outside the archive"));
            }
            let crc = s(p.get("crc32")).filter(|x| hex(x, 8)).ok_or(format!("{name}: crc32"))?;
            let sha256 = s(p.get("sha256")).filter(|x| hex(x, 64)).ok_or(format!("{name}: sha256"))?.to_ascii_lowercase();
            let priority = p.get("priority").and_then(|x| x.as_i64()).ok_or(format!("{name}: priority"))?;
            let depends = match p.get("depends") {
                None => vec![],
                d => strs(d, "depends")?,
            };
            if !names.insert(name.clone()) || !files.insert(install_as.clone()) {
                return Err(format!("{name}: listed twice"));
            }
            packs.push(Pack { name, install_as, kind: s(p.get("kind")).unwrap_or_default(), size, sha256, crc32: u32::from_str_radix(&crc, 16).map_err(|_| "crc32")?, priority, offset, depends });
        }
        let first_play = strs(o.get("first_play"), "first_play")?;
        if first_play.is_empty() {
            return Err("first_play is empty".into());
        }
        let mut maps = BTreeMap::new();
        if let Some(m) = o.get("maps") {
            for (map, d) in m.as_object().ok_or("maps is not an object")? {
                if !word(map) {
                    return Err(format!("map name {map:?}"));
                }
                let pack = s(d.get("pack")).ok_or(format!("map {map}: pack"))?;
                let needs = strs(d.get("needs"), "needs")?;
                maps.insert(map.clone(), MapNeeds { pack, needs });
            }
        }
        let known = |n: &String| names.contains(n);
        if !first_play.iter().all(known) || !maps.values().all(|m| known(&m.pack) && m.needs.iter().all(known)) || !packs.iter().all(|p| p.depends.iter().all(known)) {
            return Err("names a pack that is not listed".into());
        }
        Ok(Self { set, default_map: s(o.get("default_map")).unwrap_or_default(), first_play, maps, packs, archive_url, archive_size, text: text.to_string() })
    }

    pub fn pack(&self, name: &str) -> Option<&Pack> {
        self.packs.iter().find(|p| p.name == name)
    }

    pub fn total(&self) -> u64 {
        self.packs.iter().map(|p| p.size).sum()
    }

    pub fn first_play_size(&self) -> u64 {
        self.first_play.iter().filter_map(|n| self.pack(n)).map(|p| p.size).sum()
    }

    /// The manifest stored by an earlier run, if it still passes the checks.
    pub fn stored(dir: &Path) -> Option<Self> {
        Self::parse(&fs::read_to_string(dir.join(STORED)).ok()?).ok()
    }

    pub fn store(&self, dir: &Path) -> bool {
        fs::create_dir_all(dir).is_ok() && write_atomic(&dir.join(STORED), self.text.as_bytes())
    }

    /// Fetches the manifest. `Ok(None)`: there is none the client can use
    /// (404, or it fails the checks): take the single archive. `Err`: the
    /// network; a stored manifest stays in charge.
    pub fn fetch(http: &dyn Http, url: &str, token: &CancelToken) -> Result<Option<Self>> {
        let mut body = Vec::new();
        let mut code = 0;
        http.get(url, &[("Accept-Encoding", "identity".into())], token, &mut |h| {
            code = h.code;
            if h.code == 200 || (400..500).contains(&h.code) { Ok(()) } else { Err(Error::Http { code: h.code, message: format!("HTTP {} for the data manifest", h.code) }) }
        }, &mut |b| {
            if body.len() + b.len() > 4 << 20 {
                return Err(Error::Fatal("The data manifest is too big".into()));
            }
            body.extend_from_slice(b);
            Ok(())
        })?;
        if code != 200 {
            return Ok(None);
        }
        Ok(std::str::from_utf8(&body).ok().and_then(|t| Self::parse(t).ok()))
    }
}

// ------------------------------------------------------------ what is here

/// `.godnotic-set`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Install {
    pub set: Option<String>,
    pub legacy: Option<String>,
    pub packs: BTreeMap<String, (u64, u32)>,
}

impl Install {
    pub fn read(dir: &Path) -> Self {
        let mut i = Self::default();
        for l in fs::read_to_string(dir.join(STATE)).unwrap_or_default().lines() {
            let w: Vec<&str> = l.split(' ').collect();
            match w.as_slice() {
                ["set", s] => i.set = Some(s.to_string()),
                ["legacy", s] => i.legacy = Some(s.to_string()),
                ["pack", f, size, crc] => {
                    if let (Ok(size), Ok(crc)) = (size.parse(), u32::from_str_radix(crc, 16)) {
                        i.packs.insert(f.to_string(), (size, crc));
                    }
                }
                _ => {}
            }
        }
        i
    }
    pub fn write(&self, dir: &Path) -> bool {
        let mut t = String::new();
        if let Some(s) = &self.set {
            t.push_str(&format!("set {s}\n"));
        }
        if let Some(s) = &self.legacy {
            t.push_str(&format!("legacy {s}\n"));
        }
        for (f, (size, crc)) in &self.packs {
            t.push_str(&format!("pack {f} {size} {crc:08x}\n"));
        }
        write_atomic(&dir.join(STATE), t.as_bytes())
    }
    pub fn has(&self, dir: &Path, p: &Pack) -> bool {
        self.packs.get(&p.install_as) == Some(&(p.size, p.crc32)) && fs::metadata(dir.join(&p.install_as)).map(|m| m.is_file() && m.len() == p.size).unwrap_or(false)
    }
}

const OLD_ROLES: [&str; 5] = ["xonotic-*-data.pk3", "xonotic-*-maps.pk3", "xonotic-*-music.pk3", "xonotic-*-nexcompat.pk3", "xonotic-*-xoncompat.pk3"];

/// The layout from before the split: the official pack names.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct OldLayout {
    pub packs: Vec<String>,
    /// Parts and bookkeeping of a single-archive download.
    pub leftovers: Vec<String>,
    /// Data, maps and music are all there.
    pub complete: bool,
}

pub fn old_layout(dir: &Path) -> OldLayout {
    let mut o = OldLayout::default();
    let old_name = |n: &str| OLD_ROLES.contains(&role(n).as_str()) || (n.starts_with("font-") && n.ends_with(".pk3"));
    for n in list(dir) {
        if n.ends_with(".pk3") && old_name(&n) {
            o.packs.push(n);
        } else if let Some(base) = n.strip_suffix(".part").or_else(|| n.strip_suffix(".part.state")) {
            if old_name(base) {
                o.leftovers.push(n);
            }
        } else if n == super::META {
            o.leftovers.push(n);
        }
    }
    o.packs.sort();
    o.complete = OLD_ROLES[..3].iter().all(|r| o.packs.iter().any(|n| role(n) == *r));
    o
}

fn hash_file(path: &Path, token: &CancelToken) -> Result<(u64, u32, String)> {
    let mut f = fs::File::open(path).map_err(|e| Error::Fatal(format!("Cannot read {}: {e}", path.display())))?;
    let (mut crc, mut sha, mut n) = (crc32::Crc32::new(), Sha256::new(), 0u64);
    let mut buf = vec![0u8; 256 * 1024];
    loop {
        token.check()?;
        let got = f.read(&mut buf).map_err(|e| Error::Fatal(e.to_string()))?;
        if got == 0 {
            return Ok((n, crc.value(), sha.hex()));
        }
        crc.update(&buf[..got]);
        sha.update(&buf[..got]);
        n += got as u64;
    }
}

// ------------------------------------------------- talking to the game

/// What the game is told (the status file, or `touch_data_status` arguments).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Status {
    pub set: String,
    /// fetching, paused, waiting, error, complete, legacy.
    pub state: String,
    pub done: u64,
    pub total: u64,
    pub want: Option<String>,
    pub want_done: u64,
    pub want_total: u64,
    pub message: String,
    /// Every pack installed since this process started.
    pub landed: Vec<String>,
}

impl Status {
    pub fn text(&self) -> String {
        let mut t = format!("format 1\nset {}\nstate {}\ndone_kb {}\ntotal_kb {}\n", self.set, self.state, self.done >> 10, self.total >> 10);
        if let Some(w) = &self.want {
            t.push_str(&format!("want {w}\nwant_done_kb {}\nwant_total_kb {}\n", self.want_done >> 10, self.want_total >> 10));
        }
        if !self.message.is_empty() {
            t.push_str(&format!("message {}\n", self.message.replace(['\n', '\r'], " ")));
        }
        for l in &self.landed {
            t.push_str(&format!("landed {l}\n"));
        }
        t
    }
    /// `engine_write_dir`: where QuakeC's fopen reaches (`<userdir>/data/data`).
    pub fn write(&self, engine_write_dir: &Path) -> bool {
        let f = engine_write_dir.join(STATUS_FILE);
        f.parent().is_some_and(|p| fs::create_dir_all(p).is_ok()) && write_atomic(&f, self.text().as_bytes())
    }
}

/// The game's `want <map> <n>`, when it was written after `since`.
pub fn read_request(engine_write_dir: &Path, since: SystemTime) -> Option<(String, u64)> {
    let f = engine_write_dir.join(REQUEST_FILE);
    if fs::metadata(&f).ok()?.modified().ok()? < since {
        return None;
    }
    let text = fs::read_to_string(f).ok()?;
    let mut w = text.lines().next()?.split(' ');
    match (w.next(), w.next(), w.next()) {
        (Some("want"), Some(map), Some(n)) if word(map) => Some((map.to_string(), n.parse().ok()?)),
        _ => None,
    }
}

// ------------------------------------------------------------- the run

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    /// Only what the game needs to start.
    FirstPlay,
    Everything,
}

/// What a run found to do.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// An old complete install that stands for the published set: nothing fetched.
    Legacy,
    /// Every pack the phase asked for is installed.
    Done,
}

#[derive(PartialEq)]
enum Step {
    Wait,
    Fetch,
    Check,
    /// Verified, waiting for the packs its map needs.
    Held,
    Installed,
    GaveUp,
}

struct Job<'m> {
    p: &'m Pack,
    step: Step,
    state: Option<ChunkState>,
    file: Option<std::sync::Arc<Mutex<fs::File>>>,
    /// Chunks handed to a worker.
    taken: Vec<bool>,
    busy: usize,
    bad: u32,
}

struct Sched<'m> {
    jobs: Vec<Job<'m>>,
    /// Indices into `jobs`, in fetch order.
    order: Vec<usize>,
    want: Option<String>,
    want_total: u64,
    install: Install,
    landed: Vec<String>,
    failure: Option<Error>,
}

pub struct Session<'a> {
    pub http: &'a dyn Http,
    pub dir: PathBuf,
    pub manifest: &'a Manifest,
    pub cfg: Config,
    pub free_space: &'a FreeSpace,
    /// Called about four times a second while fetching and at every change.
    pub listener: &'a (dyn Fn(&Status) + Send + Sync),
    /// The map the game is waiting for, polled twice a second.
    pub want: &'a (dyn Fn() -> Option<String> + Send + Sync),
}

impl<'a> Session<'a> {
    fn part(&self, p: &Pack) -> PathBuf {
        self.dir.join(format!("{}.part", p.install_as))
    }
    fn state_file(&self, p: &Pack) -> PathBuf {
        self.dir.join(format!("{}.part.state", p.install_as))
    }

    /// Whether the game can start: `first_play` is installed, or an old
    /// complete install is.
    pub fn playable(dir: &Path, m: &Manifest) -> bool {
        let i = Install::read(dir);
        m.first_play.iter().filter_map(|n| m.pack(n)).all(|p| i.has(dir, p)) || old_layout(dir).complete
    }

    /// Brings `.godnotic-set` in line with the files and applies the rules for
    /// old installs and a new set. `Some(Legacy)`: nothing to fetch.
    fn reconcile(&self, token: &CancelToken) -> Result<(Install, Option<Outcome>)> {
        let m = self.manifest;
        fs::create_dir_all(&self.dir).map_err(|e| Error::Fatal(format!("Cannot create {}: {e}", self.dir.display())))?;
        let mut i = Install::read(&self.dir);
        let before = i.clone();
        // files are the truth: forget lines without a file, adopt files without a line
        i.packs.retain(|f, (size, _)| fs::metadata(self.dir.join(f)).map(|md| md.len() == *size).unwrap_or(false));
        for p in &m.packs {
            let path = self.dir.join(&p.install_as);
            if i.packs.get(&p.install_as) == Some(&(p.size, p.crc32)) || !path.is_file() {
                continue;
            }
            let fits = fs::metadata(&path).map(|md| md.len() == p.size).unwrap_or(false) && {
                let (n, crc, sha) = hash_file(&path, token)?;
                n == p.size && crc == p.crc32 && sha == p.sha256
            };
            if fits {
                i.packs.insert(p.install_as.clone(), (p.size, p.crc32));
            }
            // a file that does not fit stays until its replacement is renamed over it
        }
        let old = old_layout(&self.dir);
        let any_new = m.packs.iter().any(|p| i.has(&self.dir, p));
        if old.complete && !any_new && i.set.is_none() {
            if i.legacy.is_none() {
                i.legacy = Some(m.set.clone());
            }
            if i.legacy.as_deref() == Some(m.set.as_str()) {
                if i != before {
                    i.write(&self.dir);
                }
                return Ok((i, Some(Outcome::Legacy)));
            }
        }
        if !old.complete {
            for n in &old.leftovers {
                let _ = fs::remove_file(self.dir.join(n));
            }
            i.legacy = None;
        }
        if i.set.as_deref() != Some(m.set.as_str()) {
            // parts of another set never carry over
            for n in list(&self.dir) {
                if n.starts_with("godnotic-") && (n.ends_with(".pk3.part") || n.ends_with(".pk3.part.state")) {
                    let _ = fs::remove_file(self.dir.join(n));
                }
            }
            i.set = Some(m.set.clone());
        }
        // what a killed run left: temporary files of atomic writes, and parts or
        // chunk state of packs that are installed or no longer listed
        for n in list(&self.dir) {
            let stale = if n.contains(".tmp") && (n.starts_with("godnotic-") || n.starts_with(STATE) || n.starts_with(STORED)) {
                true
            } else if let Some(base) = n.strip_suffix(".part.state").or_else(|| n.strip_suffix(".part")).filter(|b| b.starts_with("godnotic-")) {
                m.packs.iter().find(|p| p.install_as == base).is_none_or(|p| i.has(&self.dir, p))
            } else {
                false
            };
            if stale {
                let _ = fs::remove_file(self.dir.join(n));
            }
        }
        if i != before && !i.write(&self.dir) {
            return Err(Error::Fatal(format!("Cannot write to {}", self.dir.display())));
        }
        Ok((i, None))
    }

    /// The fetch order: the wanted map's needs, `first_play`, then by priority.
    fn order(&self, jobs: &[Job<'_>], phase: Phase, want: Option<&str>) -> Vec<usize> {
        let m = self.manifest;
        let at = |n: &String| jobs.iter().position(|j| j.p.name == *n);
        let mut out: Vec<usize> = Vec::new();
        if phase == Phase::Everything {
            if let Some(w) = want.and_then(|w| m.maps.get(w)) {
                out.extend(w.needs.iter().filter_map(at));
            }
        }
        out.extend(m.first_play.iter().filter_map(at));
        if phase == Phase::Everything {
            let mut rest: Vec<usize> = (0..jobs.len()).collect();
            rest.sort_by_key(|&i| jobs[i].p.priority);
            out.extend(rest);
        }
        let mut seen = BTreeSet::new();
        out.retain(|i| seen.insert(*i));
        out
    }

    fn status(&self, s: &Sched<'_>, state: &str, message: &str) -> Status {
        let done: u64 = s.jobs.iter().map(|j| match j.step {
            Step::Installed => j.p.size,
            _ => j.state.as_ref().map(|c| c.total()).unwrap_or(0),
        }).sum();
        let mut st = Status { set: self.manifest.set.clone(), state: state.into(), done, total: self.manifest.total(), message: message.into(), landed: s.landed.clone(), ..Default::default() };
        if let Some(w) = s.want.as_ref().filter(|w| self.manifest.maps.contains_key(*w)) {
            let left: u64 = self.manifest.maps[w].needs.iter().filter_map(|n| s.jobs.iter().find(|j| j.p.name == *n)).filter(|j| j.step != Step::Installed)
                .map(|j| j.p.size - j.state.as_ref().map(|c| c.total()).unwrap_or(0)).sum();
            if left > 0 {
                st.want = Some(w.clone());
                st.want_total = s.want_total.max(left);
                st.want_done = st.want_total - left;
            }
        }
        st
    }

    /// Whether `j` may be renamed into place: a map's own pack goes last.
    fn may_install(&self, s: &Sched<'_>, j: &Job<'_>) -> bool {
        self.manifest.maps.values().filter(|m| m.pack == j.p.name).all(|m| {
            m.needs.iter().filter(|n| **n != j.p.name).all(|n| s.jobs.iter().find(|x| x.p.name == *n).is_none_or(|x| x.step == Step::Installed))
        })
    }

    /// Renames every verified pack that may go in, then applies the deletion rules.
    fn install_ready(&self, s: &mut Sched<'_>) -> Result<()> {
        while let Some(i) = (0..s.jobs.len()).find(|&i| s.jobs[i].step == Step::Held && self.may_install(s, &s.jobs[i])) {
            let p = s.jobs[i].p;
            fs::rename(self.part(p), self.dir.join(&p.install_as)).map_err(|e| Error::Fatal(format!("Cannot move {} into place: {e}", p.install_as)))?;
            let _ = fs::remove_file(self.state_file(p));
            s.jobs[i].step = Step::Installed;
            s.jobs[i].state = None;
            s.install.packs.insert(p.install_as.clone(), (p.size, p.crc32));
            s.install.write(&self.dir);
            s.landed.push(p.install_as.clone());
        }
        let m = self.manifest;
        let here = |n: &String| s.jobs.iter().find(|j| j.p.name == *n).is_none_or(|j| j.step == Step::Installed);
        let first = m.first_play.iter().all(here);
        let all = s.jobs.iter().all(|j| j.step == Step::Installed);
        if first {
            // packs of an earlier set that this one no longer lists
            for n in list(&self.dir) {
                if n.starts_with("godnotic-") && n.ends_with(".pk3") && !m.packs.iter().any(|p| p.install_as == n) {
                    let _ = fs::remove_file(self.dir.join(&n));
                    s.install.packs.remove(&n);
                }
            }
        }
        let old = old_layout(&self.dir);
        if !old.packs.is_empty() && ((first && !old.complete) || all) {
            // never both gone: every pack that replaces these is verified and in place
            for n in old.packs.iter().chain(&old.leftovers) {
                let _ = fs::remove_file(self.dir.join(n));
            }
            s.install.legacy = None;
            s.install.write(&self.dir);
        }
        Ok(())
    }

    /// Fetches what `phase` asks for. Cancel `token` to stop: parts and chunk
    /// state stay for the next run. A pack that fails its check [`CHECKS`]
    /// times is left out and the run ends with `Damaged` after the others.
    pub fn run(&self, phase: Phase, token: &CancelToken) -> Result<Outcome> {
        let m = self.manifest;
        let (install, early) = self.reconcile(token)?;
        if let Some(o) = early {
            (self.listener)(&Status { set: m.set.clone(), state: "legacy".into(), done: m.total(), total: m.total(), ..Default::default() });
            return Ok(o);
        }
        let mut jobs: Vec<Job<'_>> = m.packs.iter().map(|p| Job { p, step: if install.has(&self.dir, p) { Step::Installed } else { Step::Wait }, state: None, file: None, taken: vec![], busy: 0, bad: 0 }).collect();
        // what is on disk of each part counts from the start
        for j in jobs.iter_mut().filter(|j| j.step == Step::Wait) {
            let st = ChunkState::load(&self.state_file(j.p), &self.part(j.p), j.p.size, self.cfg.chunk, true);
            j.taken = vec![false; st.chunks];
            j.state = Some(st);
        }
        let order = self.order(&jobs, phase, None);
        let need: u64 = order.iter().map(|&i| &jobs[i]).filter(|j| j.step != Step::Installed).map(|j| j.p.size - j.state.as_ref().map(|c| c.total()).unwrap_or(0)).sum();
        let sched = Mutex::new(Sched { jobs, order, want: None, want_total: 0, install, landed: vec![], failure: None });
        let lock = || sched.lock().unwrap_or_else(|e| e.into_inner());
        self.install_ready(&mut lock())?; // deletions a killed run did not get to
        if need > 0 {
            let free = (self.free_space)(&self.dir);
            if free < need.saturating_add(self.cfg.slack) {
                return Err(Error::NoSpace { needed: need + self.cfg.slack, available: free });
            }
        }
        // nothing left to fetch or check (a held pack goes in from the thread that checks its last need)
        let target_done = |s: &Sched<'_>| s.order.iter().all(|&i| matches!(s.jobs[i].step, Step::Installed | Step::GaveUp | Step::Held));
        let take_want = |s: &mut Sched<'_>| {
            let w = (self.want)().filter(|w| m.maps.contains_key(w));
            if phase == Phase::Everything && w.is_some() && w != s.want {
                s.want = w;
                s.order = self.order(&s.jobs, phase, s.want.as_deref());
                s.want_total = 0;
                s.want_total = self.status(s, "fetching", "").want_total;
            }
        };
        take_want(&mut lock());
        let stop = CancelToken::new();
        let s2 = stop.clone();
        let hook = token.register(Box::new(move || s2.cancel()));
        let finished = AtomicBool::new(false);
        (self.listener)(&self.status(&lock(), "fetching", ""));

        std::thread::scope(|scope| {
            let workers: Vec<_> = (0..self.cfg.connections.max(1)).map(|_| scope.spawn(|| {
                while !stop.cancelled() {
                    // pick: the first pack in order with a chunk nobody has, or one to check
                    enum Work { Chunk(usize, usize), Check(usize), Idle, End }
                    let work = {
                        let mut s = lock();
                        if s.failure.is_some() || target_done(&s) {
                            Work::End
                        } else {
                            let mut w = Work::Idle;
                            for k in 0..s.order.len() {
                                let i = s.order[k];
                                let j = &mut s.jobs[i];
                                if !matches!(j.step, Step::Wait | Step::Fetch) {
                                    continue;
                                }
                                let st = j.state.as_ref().expect("a pack to fetch has its chunk state");
                                if let Some(c) = (0..st.chunks).find(|&c| !j.taken[c] && st.pos(c) < st.size_of(c)) {
                                    j.taken[c] = true;
                                    j.busy += 1;
                                    j.step = Step::Fetch;
                                    w = Work::Chunk(i, c);
                                    break;
                                }
                                if j.busy == 0 && (0..st.chunks).all(|c| st.pos(c) >= st.size_of(c)) {
                                    j.step = Step::Check;
                                    w = Work::Check(i);
                                    break;
                                }
                            }
                            w
                        }
                    };
                    let r = match work {
                        Work::End => break,
                        Work::Idle => {
                            std::thread::sleep(Duration::from_millis(20));
                            continue;
                        }
                        Work::Chunk(i, c) => {
                            let r = self.fetch_chunk(&sched, i, c, &stop);
                            let mut s = lock();
                            s.jobs[i].busy -= 1;
                            s.jobs[i].taken[c] = false;
                            r
                        }
                        Work::Check(i) => self.check(&sched, i, &stop),
                    };
                    if let Err(e) = r {
                        let mut s = lock();
                        if s.failure.is_none() && e != Error::Cancelled {
                            s.failure = Some(e);
                        }
                        drop(s);
                        stop.cancel();
                        break;
                    }
                }
            })).collect();
            scope.spawn(|| {
                let (mut saved, mut polled, mut told) = (Instant::now(), Instant::now(), Instant::now());
                while !finished.load(Ordering::SeqCst) {
                    std::thread::sleep(Duration::from_millis(25));
                    if polled.elapsed() >= Duration::from_millis(500) {
                        polled = Instant::now();
                        take_want(&mut lock());
                    }
                    if saved.elapsed() >= Duration::from_millis(self.cfg.save_every_ms) {
                        saved = Instant::now();
                        for j in lock().jobs.iter().filter(|j| j.step == Step::Fetch) {
                            if let Some(st) = &j.state {
                                st.save();
                            }
                        }
                    }
                    if told.elapsed() >= Duration::from_millis(250) {
                        told = Instant::now();
                        (self.listener)(&self.status(&lock(), "fetching", ""));
                    }
                }
            });
            for w in workers {
                let _ = w.join();
            }
            finished.store(true, Ordering::SeqCst);
        });
        token.unregister(hook);
        let mut s = lock();
        for j in s.jobs.iter_mut() {
            if let (Step::Fetch | Step::Wait, Some(st)) = (&j.step, &j.state) {
                if st.total() > 0 {
                    st.save();
                }
            }
            j.file = None;
        }
        if token.cancelled() {
            (self.listener)(&self.status(&s, "paused", ""));
            return Err(Error::Cancelled);
        }
        if let Some(e) = s.failure.take() {
            (self.listener)(&self.status(&s, if e.transient() { "waiting" } else { "error" }, &e.to_string()));
            return Err(e);
        }
        if let Some(j) = s.jobs.iter().find(|j| j.step == Step::GaveUp) {
            let e = Error::Damaged(j.p.install_as.clone());
            (self.listener)(&self.status(&s, "error", &e.to_string()));
            return Err(e);
        }
        if let Some(j) = s.order.iter().map(|&i| &s.jobs[i]).find(|j| j.step == Step::Held) {
            return Err(Error::Fatal(format!("{} waits for a pack the manifest does not give it", j.p.install_as)));
        }
        let all = s.jobs.iter().all(|j| j.step == Step::Installed);
        (self.listener)(&self.status(&s, if all { "complete" } else { "fetching" }, ""));
        Ok(Outcome::Done)
    }

    fn fetch_chunk(&self, sched: &Mutex<Sched<'_>>, ji: usize, c: usize, stop: &CancelToken) -> Result<()> {
        let lock = || sched.lock().unwrap_or_else(|e| e.into_inner());
        let (p, file) = {
            let mut s = lock();
            let p = s.jobs[ji].p;
            if s.jobs[ji].file.is_none() {
                let f = fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(self.part(p)).map_err(|e| Error::Fatal(format!("Cannot open {}: {e}", p.install_as)))?;
                s.jobs[ji].file = Some(std::sync::Arc::new(Mutex::new(f)));
            }
            (p, s.jobs[ji].file.clone().expect("just opened"))
        };
        let chunk_start = c as u64 * self.cfg.chunk;
        let size = self.cfg.chunk.min(p.size - chunk_start);
        let pos = || lock().jobs[ji].state.as_ref().map(|s| s.pos(c)).unwrap_or(size);
        let mut failures = 0;
        while pos() < size {
            stop.check()?;
            let (from, to) = (p.offset + chunk_start + pos(), p.offset + chunk_start + size - 1);
            let r = self.http.get(&self.manifest.archive_url, &range_headers(from, to, None), stop, &mut |h| {
                // the archive never changes: anything but exactly this range is an error
                if h.code == 200 {
                    return Err(Error::Transient("The server ignored the range".into()));
                }
                if h.code != 206 {
                    return Err(Error::Http { code: h.code, message: format!("HTTP {} from the download server", h.code) });
                }
                match h.header("Content-Range") {
                    Some(cr) if cr.trim().starts_with(&format!("bytes {from}-{to}/")) => Ok(()),
                    _ => Err(Error::Transient("The server answered another range".into())),
                }
            }, &mut |b| {
                stop.check()?;
                let at = pos();
                let n = (size - at).min(b.len() as u64) as usize;
                if n == 0 {
                    return Ok(());
                }
                write_at(&mut file.lock().unwrap_or_else(|e| e.into_inner()), chunk_start + at, &b[..n]).map_err(|e| Error::Fatal(format!("Cannot write {}: {e}", p.install_as)))?;
                if let Some(st) = lock().jobs[ji].state.as_ref() {
                    st.advance(c, n);
                }
                failures = 0;
                Ok(())
            });
            let r = r.and_then(|_| if pos() < size { Err(Error::Transient("Connection closed early".into())) } else { Ok(()) });
            if let Err(e) = r {
                stop.check()?;
                if !e.transient() {
                    return Err(e);
                }
                failures += 1;
                if failures >= self.cfg.attempts {
                    return Err(e);
                }
                let until = Instant::now() + Duration::from_millis((self.cfg.backoff_step_ms * failures as u64).min(self.cfg.backoff_max_ms));
                while Instant::now() < until {
                    stop.check()?;
                    std::thread::sleep(Duration::from_millis(10));
                }
            }
        }
        Ok(())
    }

    /// A whole part: size, crc32 and sha256, then into place (or held for its map's needs).
    fn check(&self, sched: &Mutex<Sched<'_>>, ji: usize, stop: &CancelToken) -> Result<()> {
        let lock = || sched.lock().unwrap_or_else(|e| e.into_inner());
        let p = {
            let mut s = lock();
            if let Some(st) = &s.jobs[ji].state {
                st.save();
            }
            if let Some(f) = s.jobs[ji].file.take() {
                let _ = f.lock().unwrap_or_else(|e| e.into_inner()).sync_all();
            }
            s.jobs[ji].p
        };
        let (n, crc, sha) = hash_file(&self.part(p), stop)?;
        let mut s = lock();
        if n != p.size || crc != p.crc32 || sha != p.sha256 {
            // which chunk is bad is not known: the pack starts over
            let _ = fs::remove_file(self.part(p));
            let _ = fs::remove_file(self.state_file(p));
            let j = &mut s.jobs[ji];
            j.bad += 1;
            let st = ChunkState::load(&self.state_file(p), &self.part(p), p.size, self.cfg.chunk, true);
            j.taken = vec![false; st.chunks];
            j.state = Some(st);
            j.step = if j.bad >= CHECKS { Step::GaveUp } else { Step::Wait };
            return Ok(());
        }
        s.jobs[ji].step = Step::Held;
        self.install_ready(&mut s)?;
        (self.listener)(&self.status(&s, "fetching", ""));
        Ok(())
    }
}
