// SPDX-License-Identifier: GPL-3.0-or-later
//! The game's pk3s, taken straight out of the release zip over HTTP Range
//! requests. A port of launcher/core's PackFetch.kt and the Android app's
//! PackFetch.java, writing the same files, so one data folder can be resumed
//! by any of them.
//!
//! The zip is never stored. Its central directory comes from its last bytes
//! (found with `bytes=0-0` and explicit offsets: GitHub's release storage
//! answers suffix ranges with 501), and each `Xonotic/data/*.pk3` is fetched
//! from its own byte range into `<name>.part`, checked against the CRC32 the
//! directory gives, then renamed into place.
//!
//! A stored entry is cut into [`Config::chunk`] pieces that
//! [`Config::connections`] connections fetch at once, each writing at its own
//! offset; how far each chunk got is saved beside the part
//! (`<name>.part.state`, big-endian "XTPC", csize, count, one i32 per chunk, as
//! Java's DataOutputStream writes it) about once a second and on stop. A
//! deflated entry is inflated as one stream and starts over after a break.
//! `.packs-setup` pins the archive (ETag and size) while a setup runs; every
//! range request carries `If-Range`, so a new build mid-download is caught.

pub mod crc32;
pub mod datasets;
#[cfg(feature = "http-ureq")]
pub mod http_ureq;

use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Every failure a run can end with, in words a player can read.
#[derive(Clone, Debug, PartialEq)]
pub enum Error {
    /// Network trouble worth another attempt.
    Transient(String),
    Cancelled,
    ArchiveChanged,
    NoSpace { needed: u64, available: u64 },
    Http { code: u16, message: String },
    Damaged(String),
    Fatal(String),
}

impl Error {
    pub fn transient(&self) -> bool {
        match self {
            Error::Transient(_) => true,
            Error::Http { code, .. } => *code >= 500 || *code == 408 || *code == 429,
            _ => false,
        }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Transient(m) | Error::Fatal(m) => write!(f, "{m}"),
            Error::Cancelled => write!(f, "Download stopped"),
            Error::ArchiveChanged => write!(f, "The download server published a new build. Retry starts over with it."),
            Error::NoSpace { needed, available } => write!(f, "Not enough free storage: needs {} MB, {} MB free", needed >> 20, available >> 20),
            Error::Http { message, .. } => write!(f, "{message}"),
            Error::Damaged(file) => write!(f, "Damaged download of {file}; Retry fetches it again"),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Stops a run. Hooks registered by the HTTP layer close what is blocked; everything else polls.
#[derive(Clone, Default)]
pub struct CancelToken {
    flag: Arc<AtomicBool>,
    #[allow(clippy::type_complexity)]
    hooks: Arc<Mutex<Vec<(u64, Box<dyn Fn() + Send>)>>>,
    next: Arc<AtomicI64>,
}

impl CancelToken {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn cancelled(&self) -> bool {
        self.flag.load(Ordering::SeqCst)
    }
    pub fn cancel(&self) {
        if self.flag.swap(true, Ordering::SeqCst) {
            return;
        }
        let hooks = std::mem::take(&mut *self.hooks.lock().unwrap_or_else(|e| e.into_inner()));
        for (_, h) in hooks {
            h();
        }
    }
    /// Runs `hook` on cancel (at once if already cancelled); the id unregisters it.
    pub fn register(&self, hook: Box<dyn Fn() + Send>) -> u64 {
        let id = self.next.fetch_add(1, Ordering::SeqCst) as u64;
        if self.cancelled() {
            hook();
            return id;
        }
        self.hooks.lock().unwrap_or_else(|e| e.into_inner()).push((id, hook));
        if self.cancelled() {
            self.cancel_late();
        }
        id
    }
    fn cancel_late(&self) {
        let hooks = std::mem::take(&mut *self.hooks.lock().unwrap_or_else(|e| e.into_inner()));
        for (_, h) in hooks {
            h();
        }
    }
    pub fn unregister(&self, id: u64) {
        self.hooks.lock().unwrap_or_else(|e| e.into_inner()).retain(|(i, _)| *i != id);
    }
    pub fn check(&self) -> Result<()> {
        if self.cancelled() { Err(Error::Cancelled) } else { Ok(()) }
    }
}

/// The final response's status and headers (names lower-case), after redirects.
pub struct Head {
    pub code: u16,
    pub headers: BTreeMap<String, String>,
}

impl Head {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(&name.to_ascii_lowercase()).map(String::as_str)
    }
}

/// One GET with headers. `head` runs once before any body; `body` gets the
/// body in pieces. Either may fail to abort. After `cancel`, `get` must return
/// within about a second even on a silent connection.
pub trait Http: Send + Sync {
    fn get(
        &self,
        url: &str,
        headers: &[(&str, String)],
        cancel: &CancelToken,
        head: &mut dyn FnMut(&Head) -> Result<()>,
        body: &mut dyn FnMut(&[u8]) -> Result<()>,
    ) -> Result<()>;
}

/// Tunables; the defaults are the Android app's. Tests shrink the chunk.
#[derive(Clone, Debug)]
pub struct Config {
    pub connections: usize,
    pub chunk: u64,
    /// Room for the filesystem, later updates and the engine's own files.
    pub slack: u64,
    /// Consecutive failures on one chunk before the run gives up.
    pub attempts: u32,
    pub backoff_max_ms: u64,
    pub backoff_step_ms: u64,
    pub save_every_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self { connections: 8, chunk: 8 << 20, slack: 64 << 20, attempts: 10, backoff_max_ms: 15_000, backoff_step_ms: 2000, save_every_ms: 1000 }
    }
}

impl Config {
    fn backoff(&self, attempt: u32) -> u64 {
        (self.backoff_step_ms * attempt as u64).min(self.backoff_max_ms)
    }
}

/// One pk3 in the archive's central directory.
#[derive(Debug)]
pub struct Entry {
    pub file_name: String,
    pub method: u16,
    pub crc: u32,
    pub csize: u64,
    pub usize: u64,
    pub offset: u64,
    data_start: AtomicI64,
}

impl Entry {
    pub fn new(file_name: &str, method: u16, crc: u32, csize: u64, usize: u64, offset: u64) -> Self {
        Self { file_name: file_name.into(), method, crc, csize, usize, offset, data_start: AtomicI64::new(-1) }
    }
    pub fn is_music(&self) -> bool {
        self.file_name.contains("-music")
    }
    /// The name with its build date as a wildcard: which installed file plays this part.
    pub fn role(&self) -> String {
        role(&self.file_name)
    }
}

/// The archive as a plan: every pk3 under its data/ folder, and its identity.
#[derive(Debug)]
pub struct Plan {
    pub url: String,
    pub identity: String,
    pub etag: Option<String>,
    pub size: u64,
    pub entries: Vec<Entry>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Need {
    pub fetch: u64,
    pub disk: u64,
    pub resumed: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Progress {
    pub done: u64,
    pub total: u64,
    pub file: Option<String>,
    pub verifying: bool,
}

pub const META: &str = ".packs-setup";

/// "xonotic-20261107-maps.pk3" and "xonotic-20261029_2-maps.pk3" -> "xonotic-*-maps.pk3".
pub fn role(name: &str) -> String {
    let b = name.as_bytes();
    let mut out = String::new();
    let mut i = 0;
    while i < b.len() {
        if i + 8 <= b.len() && b[i..i + 8].iter().all(u8::is_ascii_digit) && (i + 8 == b.len() || !b[i + 8].is_ascii_digit()) && (i == 0 || !b[i - 1].is_ascii_digit()) {
            let mut j = i + 8;
            if j + 1 < b.len() && b[j] == b'_' && b[j + 1].is_ascii_digit() {
                j += 1;
                while j < b.len() && b[j].is_ascii_digit() {
                    j += 1;
                }
            }
            out.push('*');
            i = j;
        } else {
            out.push(b[i] as char);
            i += 1;
        }
    }
    out
}

/// Whether a pk3 that plays `role` is in `dir`, whatever its build date.
pub fn installed(dir: &Path, r: &str) -> bool {
    list(dir).iter().any(|n| n.ends_with(".pk3") && role(n) == r)
}

fn list(dir: &Path) -> Vec<String> {
    fs::read_dir(dir).map(|it| it.filter_map(|e| e.ok()?.file_name().into_string().ok()).collect()).unwrap_or_default()
}

pub fn part_file(dir: &Path, e: &Entry) -> PathBuf {
    dir.join(format!("{}.part", e.file_name))
}
pub fn state_file(dir: &Path, e: &Entry) -> PathBuf {
    dir.join(format!("{}.part.state", e.file_name))
}

/// Writes through a temporary file and a rename.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> bool {
    let tmp = path.with_extension(format!("tmp{}", std::process::id()));
    let ok = fs::File::create(&tmp).and_then(|mut f| f.write_all(bytes).and_then(|_| f.sync_all())).is_ok() && fs::rename(&tmp, path).is_ok();
    if !ok {
        let _ = fs::remove_file(&tmp);
    }
    ok
}

fn le16(b: &[u8], o: usize) -> u64 {
    u16::from_le_bytes([b[o], b[o + 1]]) as u64
}
fn le32(b: &[u8], o: usize) -> u64 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) as u64
}

fn range_headers(from: u64, to: u64, etag: Option<&str>) -> Vec<(&'static str, String)> {
    let mut h = vec![("Range", format!("bytes={from}-{to}")), ("Accept-Encoding", "identity".into())];
    if let Some(t) = etag {
        h.push(("If-Range", t.into()));
    }
    h
}

fn fetch_range(http: &dyn Http, url: &str, etag: Option<&str>, from: u64, to: u64, token: &CancelToken) -> Result<Vec<u8>> {
    let want = (to - from + 1) as usize;
    let mut out = Vec::with_capacity(want);
    http.get(url, &range_headers(from, to, etag), token, &mut |h| match h.code {
        200 => Err(Error::ArchiveChanged),
        206 => Ok(()),
        c => Err(Error::Http { code: c, message: format!("HTTP {c} for a range request") }),
    }, &mut |b| {
        if out.len() + b.len() > want {
            return Err(Error::Transient("Response larger than expected".into()));
        }
        out.extend_from_slice(b);
        Ok(())
    })?;
    if out.len() != want {
        return Err(Error::Transient("Short range response".into()));
    }
    Ok(out)
}

fn total_of(content_range: Option<&str>) -> Option<u64> {
    content_range?.rsplit_once('/')?.1.trim().parse().ok()
}

/// Reads the archive's central directory. `folder` is the zip folder whose pk3s are wanted.
pub fn plan(http: &dyn Http, url: &str, token: &CancelToken, folder: &str) -> Result<Plan> {
    let mut length = None;
    let mut etag = None;
    http.get(url, &[("Range", "bytes=0-0".into()), ("Accept-Encoding", "identity".into())], token, &mut |h| {
        match h.code {
            206 => {
                length = total_of(h.header("Content-Range"));
                etag = h.header("ETag").map(str::to_string);
                Ok(())
            }
            200 => Err(Error::Fatal("The download server does not support partial downloads (HTTP 200)".into())),
            404 => Err(Error::Http { code: 404, message: format!("The data archive is not on the server (HTTP 404): {url}") }),
            c => Err(Error::Http { code: c, message: format!("HTTP {c} from the download server") }),
        }
    }, &mut |_| Ok(()))?;
    let length = length.filter(|l| *l > 0).ok_or_else(|| Error::Fatal(format!("No size from {url}")))?;
    let tail_start = length.saturating_sub(65557);
    let tail = fetch_range(http, url, etag.as_deref(), tail_start, length - 1, token)?;
    let eocd = (0..=tail.len().saturating_sub(22)).rev().find(|&i| tail.len() >= 22 && le32(&tail, i) == 0x0605_4b50)
        .ok_or_else(|| Error::Fatal(format!("Not a zip file: {url}")))?;
    let cd_size = le32(&tail, eocd + 12);
    let cd_offset = le32(&tail, eocd + 16);
    if cd_size == 0xFFFF_FFFF || cd_offset == 0xFFFF_FFFF || le16(&tail, eocd + 10) == 0xFFFF {
        return Err(Error::Fatal("zip64 archives are not supported".into()));
    }
    if cd_offset + cd_size > length || cd_size > 64 << 20 {
        return Err(Error::Fatal(format!("Damaged zip directory in {url}")));
    }
    let cd = if cd_offset >= tail_start {
        let from = (cd_offset - tail_start) as usize;
        tail[from..from + cd_size as usize].to_vec()
    } else {
        fetch_range(http, url, etag.as_deref(), cd_offset, cd_offset + cd_size - 1, token)?
    };
    let mut entries = Vec::new();
    let mut q = 0usize;
    while q + 46 <= cd.len() && le32(&cd, q) == 0x0201_4b50 {
        let (n, x, k) = (le16(&cd, q + 28) as usize, le16(&cd, q + 30) as usize, le16(&cd, q + 32) as usize);
        if q + 46 + n > cd.len() {
            break;
        }
        let name = String::from_utf8_lossy(&cd[q + 46..q + 46 + n]).to_string();
        if let Some(file) = name.strip_prefix(folder).filter(|_| name.ends_with(".pk3")) {
            // One level only, and never a name that could leave the folder.
            if !file.is_empty() && !file.contains(['/', '\\']) && !file.starts_with('.') {
                let (csize, usize, off) = (le32(&cd, q + 20), le32(&cd, q + 24), le32(&cd, q + 42));
                if csize == 0xFFFF_FFFF || usize == 0xFFFF_FFFF || off == 0xFFFF_FFFF {
                    return Err(Error::Fatal("zip64 archives are not supported".into()));
                }
                entries.push(Entry::new(file, le16(&cd, q + 10) as u16, le32(&cd, q + 16) as u32, csize, usize, off));
            }
        }
        q += 46 + n + x + k;
    }
    if entries.is_empty() {
        return Err(Error::Fatal(format!("No pk3 in {url}")));
    }
    let identity = format!("{}\n{}", etag.clone().unwrap_or_else(|| "null".into()), length);
    Ok(Plan { url: url.into(), identity, etag, size: length, entries })
}

/// The `.packs-setup` file: identity (two lines), then "want <role>" and "done <file>".
#[derive(Debug, Default, PartialEq)]
pub struct Meta {
    pub identity: Option<String>,
    pub want: Vec<String>,
    pub finished: Vec<String>,
}

impl Meta {
    pub fn read(dir: &Path) -> Self {
        let Ok(text) = fs::read_to_string(dir.join(META)) else { return Self::default() };
        let lines: Vec<&str> = text.split('\n').collect();
        if lines.len() < 2 {
            return Self::default();
        }
        let mut m = Meta { identity: Some(format!("{}\n{}", lines[0], lines[1])), ..Default::default() };
        for l in &lines[2..] {
            if let Some(w) = l.strip_prefix("want ") {
                if !m.want.iter().any(|x| x == w) {
                    m.want.push(w.into());
                }
            } else if let Some(d) = l.strip_prefix("done ") {
                if !m.finished.iter().any(|x| x == d) {
                    m.finished.push(d.into());
                }
            }
        }
        m
    }
    pub fn write(&self, dir: &Path) -> bool {
        let mut t = format!("{}\n", self.identity.clone().unwrap_or_default());
        for w in &self.want {
            t.push_str(&format!("want {w}\n"));
        }
        for d in &self.finished {
            t.push_str(&format!("done {d}\n"));
        }
        write_atomic(&dir.join(META), t.as_bytes())
    }
    /// Roles a setup wants that are not installed yet.
    pub fn missing(&self, dir: &Path) -> Vec<String> {
        self.want.iter().filter(|w| !installed(dir, w)).cloned().collect()
    }
}

/// How many bytes of each fixed-size chunk of a stored entry are on disk.
pub struct ChunkState {
    file: PathBuf,
    csize: u64,
    chunk: u64,
    pub chunks: usize,
    done: Vec<AtomicI32>,
}

const MAGIC: u32 = 0x5854_5043; // "XTPC"

impl ChunkState {
    fn empty(file: &Path, csize: u64, chunk: u64) -> Self {
        let count = csize.div_ceil(chunk).max(1) as usize;
        Self { file: file.into(), csize, chunk, chunks: count, done: (0..count).map(|_| AtomicI32::new(0)).collect() }
    }
    pub fn pos(&self, i: usize) -> u64 {
        self.done[i].load(Ordering::SeqCst) as u64
    }
    pub fn size_of(&self, i: usize) -> u64 {
        self.chunk.min(self.csize - i as u64 * self.chunk)
    }
    pub fn advance(&self, i: usize, n: usize) {
        self.done[i].fetch_add(n as i32, Ordering::SeqCst);
    }
    pub fn total(&self) -> u64 {
        (0..self.chunks).map(|i| self.pos(i)).sum()
    }
    pub fn encode(&self) -> Vec<u8> {
        let mut b = Vec::with_capacity(16 + 4 * self.chunks);
        b.extend_from_slice(&MAGIC.to_be_bytes());
        b.extend_from_slice(&(self.csize as i64).to_be_bytes());
        b.extend_from_slice(&(self.chunks as i32).to_be_bytes());
        for d in &self.done {
            b.extend_from_slice(&d.load(Ordering::SeqCst).to_be_bytes());
        }
        b
    }
    pub fn save(&self) {
        write_atomic(&self.file, &self.encode());
    }
    /// `clean`: delete a part and state that do not belong together (false: only look).
    pub fn load(file: &Path, part: &Path, csize: u64, chunk: u64, clean: bool) -> Self {
        let s = Self::empty(file, csize, chunk);
        if part.is_file() {
            if let Ok(b) = fs::read(file) {
                let n = s.chunks;
                let be32 = |o: usize| i32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
                if b.len() == 16 + 4 * n && be32(0) as u32 == MAGIC && i64::from_be_bytes(b[4..12].try_into().unwrap_or([0; 8])) == csize as i64 && be32(12) == n as i32 {
                    let ok = (0..n).all(|i| {
                        let v = be32(16 + 4 * i);
                        v >= 0 && (v as u64) <= s.size_of(i)
                    });
                    if ok {
                        for i in 0..n {
                            s.done[i].store(be32(16 + 4 * i), Ordering::SeqCst);
                        }
                        return s;
                    }
                }
            }
        }
        if clean {
            let _ = fs::remove_file(part);
            let _ = fs::remove_file(file);
        }
        s
    }
}

/// The wanted entries that no installed file plays yet.
pub fn todo<'a>(dir: &Path, plan: &'a Plan, music: bool) -> Vec<&'a Entry> {
    plan.entries.iter().filter(|e| (music || !e.is_music()) && !installed(dir, &e.role())).collect()
}

/// What fetching `todo` costs in download and storage, counting resumable parts.
pub fn need(dir: &Path, todo: &[&Entry], plan: &Plan, cfg: &Config) -> Need {
    let same = Meta::read(dir).identity.as_deref() == Some(plan.identity.as_str());
    let mut n = Need { fetch: 0, disk: 0, resumed: 0 };
    for e in todo {
        let r = if same && e.method == 0 { ChunkState::load(&state_file(dir, e), &part_file(dir, e), e.csize, cfg.chunk, false).total() } else { 0 };
        n.fetch += e.csize - r;
        n.disk += e.usize.saturating_sub(r);
        n.resumed += r;
    }
    n
}

/// Free bytes an unprivileged process may write in `dir`; u64::MAX when unknown.
pub type FreeSpace = dyn Fn(&Path) -> u64 + Send + Sync;

pub struct PackFetch<'a> {
    pub http: &'a dyn Http,
    pub dir: PathBuf,
    pub plan: &'a Plan,
    pub cfg: Config,
    pub free_space: &'a FreeSpace,
    pub listener: &'a (dyn Fn(Progress) + Send + Sync),
    done: AtomicI64,
    total: AtomicI64,
    last_report: AtomicI64,
}

struct Pack<'p> {
    e: &'p Entry,
    state: Mutex<Option<ChunkState>>,
    file: Mutex<Option<fs::File>>,
    left: AtomicUsize,
    head: Mutex<()>,
}

impl<'a> PackFetch<'a> {
    pub fn new(http: &'a dyn Http, dir: &Path, plan: &'a Plan, cfg: Config, free_space: &'a FreeSpace, listener: &'a (dyn Fn(Progress) + Send + Sync)) -> Self {
        Self { http, dir: dir.into(), plan, cfg, free_space, listener, done: AtomicI64::new(0), total: AtomicI64::new(0), last_report: AtomicI64::new(i64::MIN / 2) }
    }

    fn report(&self, file: Option<&str>, verifying: bool) {
        (self.listener)(Progress { done: self.done.load(Ordering::SeqCst).max(0) as u64, total: self.total.load(Ordering::SeqCst) as u64, file: file.map(str::to_string), verifying });
    }

    fn advance(&self, n: i64) {
        let now = self.done.fetch_add(n, Ordering::SeqCst) + n;
        let last = self.last_report.load(Ordering::SeqCst);
        if now - last >= 1 << 20 && self.last_report.compare_exchange(last, now, Ordering::SeqCst, Ordering::SeqCst).is_ok() {
            self.report(None, false);
        }
    }

    fn pause(&self, attempt: u32, stop: &CancelToken) -> Result<()> {
        let until = Instant::now() + Duration::from_millis(self.cfg.backoff(attempt));
        while Instant::now() < until {
            stop.check()?;
            std::thread::sleep(Duration::from_millis(20).min(until.saturating_duration_since(Instant::now())).max(Duration::from_millis(1)));
        }
        stop.check()
    }

    /// Fetches the wanted packs; returns when all are installed. Cancel `token`
    /// to stop: parts and chunk state stay for the next run, and this returns Cancelled.
    pub fn run(&self, token: &CancelToken, music: bool) -> Result<()> {
        fs::create_dir_all(&self.dir).map_err(|e| Error::Fatal(format!("Cannot create {}: {e}", self.dir.display())))?;
        let mut meta = Meta::read(&self.dir);
        if meta.identity.as_deref() != Some(self.plan.identity.as_str()) {
            self.discard(&meta);
            meta = Meta { identity: Some(self.plan.identity.clone()), ..Default::default() };
        }
        let todo = todo(&self.dir, self.plan, music);
        for e in &self.plan.entries {
            if (music || !e.is_music()) && !meta.want.contains(&e.role()) {
                meta.want.push(e.role());
            }
        }
        if todo.is_empty() {
            let _ = fs::remove_file(self.dir.join(META));
            return Ok(());
        }
        let need = need(&self.dir, &todo, self.plan, &self.cfg);
        let free = (self.free_space)(&self.dir);
        if free < need.disk + self.cfg.slack {
            return Err(Error::NoSpace { needed: need.disk + self.cfg.slack, available: free });
        }
        if !meta.write(&self.dir) {
            return Err(Error::Fatal(format!("Cannot write to {}", self.dir.display())));
        }
        let meta = Mutex::new(meta);
        self.total.store((need.fetch + need.resumed) as i64, Ordering::SeqCst);
        self.done.store(need.resumed as i64, Ordering::SeqCst);
        self.report(None, false);
        let stop = CancelToken::new();
        let s2 = stop.clone();
        let hook = token.register(Box::new(move || s2.cancel()));
        let mut packs = Vec::new();
        for e in &todo {
            packs.push(self.open_pack(e)?);
        }
        let result = self.run_jobs(&packs, &stop, &meta);
        token.unregister(hook);
        for p in &packs {
            self.close(p);
        }
        if token.cancelled() {
            return Err(Error::Cancelled);
        }
        result?;
        let _ = fs::remove_file(self.dir.join(META));
        self.report(None, false);
        Ok(())
    }

    fn open_pack<'p>(&self, e: &'p Entry) -> Result<Pack<'p>> {
        let (state, file) = match e.method {
            0 => {
                let st = ChunkState::load(&state_file(&self.dir, e), &part_file(&self.dir, e), e.csize, self.cfg.chunk, true);
                let f = fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(part_file(&self.dir, e))
                    .map_err(|x| Error::Fatal(format!("Cannot open {}: {x}", e.file_name)))?;
                (Some(st), Some(f))
            }
            8 => (None, None),
            _ => return Err(Error::Fatal(format!("Unsupported compression in {}", e.file_name))),
        };
        Ok(Pack { e, state: Mutex::new(state), file: Mutex::new(file), left: AtomicUsize::new(0), head: Mutex::new(()) })
    }

    fn run_jobs(&self, packs: &[Pack<'_>], stop: &CancelToken, meta: &Mutex<Meta>) -> Result<()> {
        // A job: (pack index, Some(chunk) or None for deflate/verify-only).
        let mut jobs: Vec<(usize, Option<usize>)> = Vec::new();
        for (pi, p) in packs.iter().enumerate() {
            if p.e.method == 8 {
                p.left.store(1, Ordering::SeqCst);
                jobs.push((pi, None));
                continue;
            }
            let st = p.state.lock().unwrap_or_else(|e| e.into_inner());
            let st = st.as_ref().ok_or_else(|| Error::Fatal("lost chunk state".into()))?;
            let chunks: Vec<usize> = (0..st.chunks).filter(|&i| st.pos(i) < st.size_of(i)).collect();
            if chunks.is_empty() {
                p.left.store(1, Ordering::SeqCst);
                jobs.push((pi, None));
            } else {
                p.left.store(chunks.len(), Ordering::SeqCst);
                jobs.extend(chunks.into_iter().map(|c| (pi, Some(c))));
            }
        }
        let next = AtomicUsize::new(0);
        let failure: Mutex<Option<Error>> = Mutex::new(None);
        let finished = AtomicBool::new(false);
        std::thread::scope(|s| {
            let workers: Vec<_> = (0..self.cfg.connections.min(jobs.len()).max(1)).map(|_| {
                s.spawn(|| {
                    while !stop.cancelled() {
                        let i = next.fetch_add(1, Ordering::SeqCst);
                        let Some(&(pi, chunk)) = jobs.get(i) else { break };
                        let p = &packs[pi];
                        let r = match (p.e.method, chunk) {
                            (8, _) => self.retrying(stop, || self.fetch_deflated(p, stop, meta)),
                            (_, None) => self.verify_and_publish(p, stop, meta),
                            (_, Some(c)) => self.fetch_chunk(p, c, stop).and_then(|_| {
                                if p.left.fetch_sub(1, Ordering::SeqCst) == 1 { self.verify_and_publish(p, stop, meta) } else { Ok(()) }
                            }),
                        };
                        if let Err(e) = r {
                            let mut f = failure.lock().unwrap_or_else(|e| e.into_inner());
                            if f.is_none() && e != Error::Cancelled {
                                *f = Some(e);
                            }
                            drop(f);
                            stop.cancel();
                            break;
                        }
                    }
                })
            }).collect();
            s.spawn(|| {
                let mut last = Instant::now();
                while !finished.load(Ordering::SeqCst) {
                    std::thread::sleep(Duration::from_millis(50));
                    if last.elapsed() < Duration::from_millis(self.cfg.save_every_ms) {
                        continue;
                    }
                    last = Instant::now();
                    for p in packs {
                        self.save(p);
                    }
                    self.report(None, false);
                    self.last_report.store(self.done.load(Ordering::SeqCst), Ordering::SeqCst);
                }
            });
            for w in workers {
                let _ = w.join();
            }
            finished.store(true, Ordering::SeqCst);
        });
        for p in packs {
            self.save(p);
        }
        if let Some(e) = failure.into_inner().unwrap_or_else(|e| e.into_inner()) {
            return Err(e);
        }
        if stop.cancelled() {
            return Err(Error::Cancelled);
        }
        Ok(())
    }

    fn retrying<T>(&self, stop: &CancelToken, mut job: impl FnMut() -> Result<T>) -> Result<T> {
        let mut attempt = 1;
        loop {
            stop.check()?;
            match job() {
                Ok(v) => return Ok(v),
                Err(e) => {
                    stop.check()?;
                    if !e.transient() || attempt >= self.cfg.attempts {
                        return Err(e);
                    }
                    self.pause(attempt, stop)?;
                    attempt += 1;
                }
            }
        }
    }

    fn check_response(h: &Head, etag: Option<&str>) -> Result<()> {
        if h.code == 200 {
            return Err(Error::ArchiveChanged);
        }
        if h.code != 206 {
            return Err(Error::Http { code: h.code, message: format!("HTTP {} from the download server", h.code) });
        }
        if let (Some(want), Some(got)) = (etag, h.header("ETag")) {
            if want != got {
                return Err(Error::ArchiveChanged);
            }
        }
        Ok(())
    }

    /// Where the entry's bytes start: its local header is read once.
    fn data_start(&self, p: &Pack<'_>, stop: &CancelToken) -> Result<u64> {
        let _g = p.head.lock().unwrap_or_else(|e| e.into_inner());
        let s = p.e.data_start.load(Ordering::SeqCst);
        if s >= 0 {
            return Ok(s as u64);
        }
        let lh = self.retrying(stop, || fetch_range(self.http, &self.plan.url, self.plan.etag.as_deref(), p.e.offset, p.e.offset + 29, stop))?;
        if le32(&lh, 0) != 0x0403_4b50 {
            return Err(Error::Fatal(format!("Bad local header for {}", p.e.file_name)));
        }
        let start = p.e.offset + 30 + le16(&lh, 26) + le16(&lh, 28);
        p.e.data_start.store(start as i64, Ordering::SeqCst);
        Ok(start)
    }

    fn fetch_chunk(&self, p: &Pack<'_>, i: usize, stop: &CancelToken) -> Result<()> {
        let start = self.data_start(p, stop)?;
        let chunk_start = i as u64 * self.cfg.chunk;
        let size = {
            let st = p.state.lock().unwrap_or_else(|e| e.into_inner());
            st.as_ref().map(|s| s.size_of(i)).unwrap_or(0)
        };
        let pos = || p.state.lock().unwrap_or_else(|e| e.into_inner()).as_ref().map(|s| s.pos(i)).unwrap_or(size);
        let mut failures = 0;
        while pos() < size {
            stop.check()?;
            let from = chunk_start + pos();
            let r = self.http.get(&self.plan.url, &range_headers(start + from, start + chunk_start + size - 1, self.plan.etag.as_deref()), stop,
                &mut |h| Self::check_response(h, self.plan.etag.as_deref()), &mut |b| {
                    stop.check()?;
                    let at = pos();
                    let n = (size - at).min(b.len() as u64) as usize;
                    if n == 0 {
                        return Ok(());
                    }
                    {
                        let mut f = p.file.lock().unwrap_or_else(|e| e.into_inner());
                        let f = f.as_mut().ok_or(Error::Cancelled)?;
                        write_at(f, chunk_start + at, &b[..n]).map_err(|e| Error::Fatal(format!("Cannot write {}: {e}", p.e.file_name)))?;
                    }
                    if let Some(s) = p.state.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
                        s.advance(i, n);
                    }
                    self.advance(n as i64);
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
                self.pause(failures, stop)?;
            }
        }
        Ok(())
    }

    fn fetch_deflated(&self, p: &Pack<'_>, stop: &CancelToken, meta: &Mutex<Meta>) -> Result<()> {
        use miniz_oxide::inflate::stream::{inflate, InflateState};
        use miniz_oxide::{DataFormat, MZFlush, MZStatus};
        let start = self.data_start(p, stop)?;
        let part = part_file(&self.dir, p.e);
        let mut out = fs::File::create(&part).map_err(|e| Error::Fatal(format!("Cannot write {}: {e}", part.display())))?;
        let mut state = InflateState::new_boxed(DataFormat::Raw);
        let mut obuf = vec![0u8; 128 * 1024];
        let mut got = 0u64;
        let mut written = 0u64;
        let mut crc = crc32::Crc32::new();
        let mut ended = false;
        let r = self.http.get(&self.plan.url, &range_headers(start, start + p.e.csize - 1, self.plan.etag.as_deref()), stop,
            &mut |h| Self::check_response(h, self.plan.etag.as_deref()), &mut |b| {
                stop.check()?;
                let n = (p.e.csize - got).min(b.len() as u64) as usize;
                if n == 0 {
                    return Ok(());
                }
                got += n as u64;
                self.advance(n as i64);
                let mut input = &b[..n];
                while !ended && !input.is_empty() {
                    let res = inflate(&mut state, input, &mut obuf, MZFlush::None);
                    input = &input[res.bytes_consumed..];
                    if res.bytes_written > 0 {
                        out.write_all(&obuf[..res.bytes_written]).map_err(|e| Error::Fatal(e.to_string()))?;
                        crc.update(&obuf[..res.bytes_written]);
                        written += res.bytes_written as u64;
                    }
                    match res.status {
                        Ok(MZStatus::StreamEnd) => ended = true,
                        Ok(_) if res.bytes_consumed == 0 && res.bytes_written == 0 => break,
                        Ok(_) => {}
                        Err(miniz_oxide::MZError::Buf) => break,
                        Err(_) => return Err(Error::Transient(format!("Damaged download of {}", p.e.file_name))),
                    }
                }
                Ok(())
            });
        // Drain what the inflater still holds.
        if r.is_ok() && !ended {
            loop {
                let res = inflate(&mut state, &[], &mut obuf, MZFlush::Finish);
                if res.bytes_written > 0 {
                    let _ = out.write_all(&obuf[..res.bytes_written]);
                    crc.update(&obuf[..res.bytes_written]);
                    written += res.bytes_written as u64;
                }
                match res.status {
                    Ok(MZStatus::StreamEnd) => {
                        ended = true;
                        break;
                    }
                    _ if res.bytes_written == 0 => break,
                    _ => {}
                }
            }
        }
        let r = r.and_then(|_| if ended { Ok(()) } else { Err(Error::Transient("Connection closed early".into())) });
        if let Err(e) = r {
            // A deflated pack starts over: take back what it counted.
            self.done.fetch_sub(got as i64, Ordering::SeqCst);
            return Err(e);
        }
        drop(out);
        if written != p.e.usize || crc.value() != p.e.crc {
            let _ = fs::remove_file(&part);
            self.done.fetch_sub(got as i64, Ordering::SeqCst);
            return Err(Error::Transient(format!("Damaged download of {}", p.e.file_name)));
        }
        self.publish(p, meta)
    }

    fn verify_and_publish(&self, p: &Pack<'_>, stop: &CancelToken, meta: &Mutex<Meta>) -> Result<()> {
        if let Some(s) = p.state.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
            s.save();
        }
        self.report(Some(&p.e.file_name), true);
        let mut crc = crc32::Crc32::new();
        let mut at = 0u64;
        {
            let mut g = p.file.lock().unwrap_or_else(|e| e.into_inner());
            let f = g.as_mut().ok_or(Error::Cancelled)?;
            let _ = f.sync_all();
            f.seek(SeekFrom::Start(0)).map_err(|e| Error::Fatal(e.to_string()))?;
            let mut buf = vec![0u8; 256 * 1024];
            loop {
                stop.check()?;
                let n = f.read(&mut buf).map_err(|e| Error::Fatal(e.to_string()))?;
                if n == 0 {
                    break;
                }
                crc.update(&buf[..n]);
                at += n as u64;
            }
        }
        if at != p.e.usize || crc.value() != p.e.crc {
            // Which chunk is bad is not known: the pack starts over.
            *p.file.lock().unwrap_or_else(|e| e.into_inner()) = None;
            *p.state.lock().unwrap_or_else(|e| e.into_inner()) = None;
            let _ = fs::remove_file(part_file(&self.dir, p.e));
            let _ = fs::remove_file(state_file(&self.dir, p.e));
            self.done.fetch_sub(p.e.csize as i64, Ordering::SeqCst);
            return Err(Error::Damaged(p.e.file_name.clone()));
        }
        self.close(p);
        self.publish(p, meta)?;
        let _ = fs::remove_file(state_file(&self.dir, p.e));
        Ok(())
    }

    fn publish(&self, p: &Pack<'_>, meta: &Mutex<Meta>) -> Result<()> {
        let dest = self.dir.join(&p.e.file_name);
        fs::rename(part_file(&self.dir, p.e), &dest).map_err(|e| Error::Fatal(format!("Cannot move {} into place: {e}", p.e.file_name)))?;
        let mut m = meta.lock().unwrap_or_else(|e| e.into_inner());
        m.finished.push(p.e.file_name.clone());
        m.write(&self.dir);
        Ok(())
    }

    fn save(&self, p: &Pack<'_>) {
        if p.file.lock().unwrap_or_else(|e| e.into_inner()).is_some() {
            if let Some(s) = p.state.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
                s.save();
            }
        }
    }

    fn close(&self, p: &Pack<'_>) {
        let mut f = p.file.lock().unwrap_or_else(|e| e.into_inner());
        if f.is_some() {
            if let Some(s) = p.state.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
                s.save();
            }
            *f = None;
        }
    }

    /// Removes what a setup pinned to another build left: its parts and the packs it finished.
    fn discard(&self, old: &Meta) {
        for n in list(&self.dir) {
            if n.ends_with(".pk3.part") || n.ends_with(".pk3.part.state") {
                let _ = fs::remove_file(self.dir.join(&n));
            }
        }
        if old.identity.is_none() {
            return;
        }
        let current: Vec<(String, u64)> = self.plan.entries.iter().map(|e| (e.file_name.clone(), e.usize)).collect();
        for name in &old.finished {
            let path = self.dir.join(name);
            if let Ok(md) = fs::metadata(&path) {
                if !current.contains(&(name.clone(), md.len())) {
                    let _ = fs::remove_file(path);
                }
            }
        }
    }
}

#[cfg(unix)]
fn write_at(f: &mut fs::File, at: u64, b: &[u8]) -> std::io::Result<()> {
    use std::os::unix::fs::FileExt;
    f.write_all_at(b, at)
}

#[cfg(not(unix))]
fn write_at(f: &mut fs::File, at: u64, b: &[u8]) -> std::io::Result<()> {
    f.seek(SeekFrom::Start(at))?;
    f.write_all(b)
}

/// Speed and time left, smoothed: an exponential average of the rate over samples at least `window_ms` apart.
pub struct RateMeter {
    window_ms: u64,
    mark: Option<(u64, u64)>,
    pub bytes_per_sec: f64,
}

impl RateMeter {
    pub fn new(window_ms: u64) -> Self {
        Self { window_ms, mark: None, bytes_per_sec: 0.0 }
    }
    pub fn sample(&mut self, now_ms: u64, done: u64) {
        match self.mark {
            Some((t, d)) if done >= d => {
                let dt = now_ms.saturating_sub(t);
                if dt < self.window_ms {
                    return;
                }
                let r = (done - d) as f64 * 1000.0 / dt as f64;
                self.bytes_per_sec = if self.bytes_per_sec == 0.0 { r } else { self.bytes_per_sec * 0.7 + r * 0.3 };
                self.mark = Some((now_ms, done));
            }
            _ => self.mark = Some((now_ms, done)),
        }
    }
}
