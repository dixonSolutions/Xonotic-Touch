// SPDX-License-Identifier: GPL-3.0-or-later
//! The split-set downloader (docs/DATA_FETCH.md) against a local stand-in for
//! the release storage: explicit ranges only (a suffix range gets 501), an
//! optional delay, corruption, a 200 that ignores the range.

use godnotic_core::fetch::http_ureq::UreqHttp;
use godnotic_core::fetch::manifest::*;
use godnotic_core::fetch::sha256::Sha256;
use godnotic_core::fetch::*;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};

#[derive(Default)]
struct Knobs {
    archive: Mutex<Vec<u8>>,
    manifest: Mutex<Option<String>>,
    served: AtomicU64,
    ranges: Mutex<Vec<String>>,
    suffix: AtomicU64,
    delay_per_16k_ms: AtomicU64,
    /// Flip the byte at this archive offset in the next N responses that cover it.
    corrupt_at: AtomicI64,
    corrupt_times: AtomicI64,
    ignore_range: AtomicBool,
    wrong_range: AtomicBool,
}

struct Server {
    k: Arc<Knobs>,
    base: String,
}

impl Server {
    fn start() -> Self {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
        let k = Arc::new(Knobs::default());
        k.corrupt_at.store(-1, Ordering::SeqCst);
        let k2 = k.clone();
        std::thread::spawn(move || {
            for s in l.incoming().flatten() {
                let k3 = k2.clone();
                std::thread::spawn(move || {
                    let _ = serve(s, &k3);
                });
            }
        });
        Self { k, base }
    }
    fn archive_url(&self) -> String {
        format!("{}/r1/Godnotic-data-astc.zip", self.base)
    }
    fn manifest_url(&self) -> String {
        format!("{}/slim/godnotic-data-astc.json", self.base)
    }
}

fn serve(s: TcpStream, k: &Knobs) -> std::io::Result<()> {
    let mut r = BufReader::new(s.try_clone()?);
    let mut w = s;
    loop {
        let mut line = String::new();
        if r.read_line(&mut line)? == 0 {
            return Ok(());
        }
        let path = line.split_whitespace().nth(1).unwrap_or("").to_string();
        let mut range = None;
        loop {
            let mut h = String::new();
            r.read_line(&mut h)?;
            let h = h.trim_end();
            if h.is_empty() {
                break;
            }
            if let Some((a, b)) = h.split_once(':') {
                if a.eq_ignore_ascii_case("range") {
                    range = Some(b.trim().to_string());
                }
            }
        }
        let head = |code: &str, extra: &str, len: usize| format!("HTTP/1.1 {code}\r\nAccept-Ranges: bytes\r\nContent-Length: {len}\r\n{extra}\r\n");
        if path.ends_with(".json") {
            match k.manifest.lock().unwrap().clone() {
                Some(m) => {
                    w.write_all(head("200 OK", "", m.len()).as_bytes())?;
                    w.write_all(m.as_bytes())?;
                }
                None => w.write_all(head("404 Not Found", "", 0).as_bytes())?,
            }
            continue;
        }
        if !path.ends_with(".zip") {
            w.write_all(head("404 Not Found", "", 0).as_bytes())?;
            continue;
        }
        k.ranges.lock().unwrap().push(range.clone().unwrap_or_default());
        if range.as_deref().is_some_and(|r| r.starts_with("bytes=-")) {
            k.suffix.fetch_add(1, Ordering::SeqCst);
            w.write_all(head("501 Not Implemented", "", 0).as_bytes())?;
            continue;
        }
        let data = k.archive.lock().unwrap().clone();
        let parsed = range.as_deref().and_then(|r| r.strip_prefix("bytes=")).and_then(|r| r.split_once('-')).and_then(|(a, b)| Some((a.parse::<usize>().ok()?, b.parse::<usize>().ok()?)));
        let Some((from, to)) = parsed.filter(|_| !k.ignore_range.load(Ordering::SeqCst)) else {
            w.write_all(head("200 OK", "", data.len()).as_bytes())?;
            w.write_all(&data)?;
            continue;
        };
        let to = to.min(data.len() - 1);
        let mut body = data[from..=to].to_vec();
        let c = k.corrupt_at.load(Ordering::SeqCst);
        if c >= from as i64 && c <= to as i64 && k.corrupt_times.fetch_sub(1, Ordering::SeqCst) > 0 {
            body[(c as usize) - from] ^= 0xFF;
        }
        let shown = if k.wrong_range.load(Ordering::SeqCst) { from + 1 } else { from };
        w.write_all(head("206 Partial Content", &format!("Content-Range: bytes {shown}-{to}/{}\r\n", data.len()), body.len()).as_bytes())?;
        for piece in body.chunks(16 * 1024) {
            w.write_all(piece)?;
            k.served.fetch_add(piece.len() as u64, Ordering::SeqCst);
            let d = k.delay_per_16k_ms.load(Ordering::SeqCst);
            if d > 0 {
                std::thread::sleep(Duration::from_millis(d));
            }
        }
        w.flush()?;
    }
}

fn bytes(seed: u64, n: usize) -> Vec<u8> {
    let mut x = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    (0..n).map(|_| {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        (x >> 24) as u8
    }).collect()
}

fn crc(b: &[u8]) -> u32 {
    let mut c = crc32::Crc32::new();
    c.update(b);
    c.value()
}

fn sha(b: &[u8]) -> String {
    let mut s = Sha256::new();
    s.update(b);
    s.hex()
}

/// A small set shaped like the real one: core, two shared packs, three maps
/// (alpha is the first map), music, extras. `seed` changes every pack but
/// those named in `same`, which are as in seed 1.
struct Set {
    archive: Vec<u8>,
    manifest: String,
    files: Vec<(String, Vec<u8>)>,
}

fn make_set(set: &str, date: &str, seed: u64, same: &[&str], url: &str) -> Set {
    // name, kind, size, depends, maps
    #[allow(clippy::type_complexity)]
    let spec: [(&str, &str, usize, &[&str], &[&str]); 8] = [
        ("core", "core", 300_000, &[], &[]),
        ("shared-1", "shared", 40_000, &[], &[]),
        ("map-alpha", "map", 70_000, &["shared-1"], &["alpha"]),
        ("music", "music", 150_000, &[], &[]),
        ("shared-2", "shared", 30_000, &[], &[]),
        ("map-beta", "map", 60_000, &["shared-1", "shared-2"], &["beta"]),
        ("map-gamma", "map", 50_000, &["shared-2"], &["gamma"]),
        ("extras", "extras", 200_000, &[], &[]),
    ];
    let mut archive = b"PK-junk-before".to_vec();
    let (mut packs, mut files, mut maps) = (Vec::new(), Vec::new(), serde_json::Map::new());
    for (i, (name, kind, size, deps, m)) in spec.iter().enumerate() {
        let data = bytes(if same.contains(name) { 100 + i as u64 } else { seed * 100 + i as u64 }, *size);
        archive.extend_from_slice(format!("local-header-{name}").as_bytes());
        let file = format!("godnotic-{date}-{name}.pk3");
        packs.push(serde_json::json!({"name": name, "file": file, "install_as": file, "kind": kind, "size": size, "sha256": sha(&data), "crc32": format!("{:08x}", crc(&data)),
            "priority": i, "depends": deps, "maps": m, "offset": archive.len()}));
        archive.extend_from_slice(&data);
        for map in *m {
            let mut needs: Vec<&str> = vec!["core"];
            needs.extend(deps.iter());
            needs.push(name);
            maps.insert(map.to_string(), serde_json::json!({"pack": name, "needs": needs}));
        }
        files.push((file, data));
    }
    archive.extend_from_slice(b"central-directory");
    let manifest = serde_json::json!({"format": 1, "set": set, "default_map": "alpha", "first_play": ["core", "shared-1", "map-alpha"], "maps": maps, "packs": packs,
        "archive": {"file": "Godnotic-data-astc.zip", "size": archive.len(), "sha256": sha(&archive), "folder": "Xonotic/data/", "url": url}}).to_string();
    Set { archive, manifest, files }
}

fn tmpdir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("godnotic-mfetch-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn cfg() -> Config {
    Config { chunk: 64 * 1024, connections: 4, slack: 0, backoff_step_ms: 20, backoff_max_ms: 50, attempts: 5, save_every_ms: 50 }
}

fn unlimited(_: &Path) -> u64 {
    u64::MAX
}

fn nothing() -> Option<String> {
    None
}

struct Rig {
    srv: Server,
    set: Set,
    m: Manifest,
    dir: PathBuf,
    http: UreqHttp,
    seen: Mutex<Vec<Status>>,
}

fn rig(name: &str) -> Rig {
    let srv = Server::start();
    let set = make_set("t-1", "20261107", 1, &[], &srv.archive_url());
    *srv.k.archive.lock().unwrap() = set.archive.clone();
    *srv.k.manifest.lock().unwrap() = Some(set.manifest.clone());
    let m = Manifest::parse(&set.manifest).unwrap();
    Rig { srv, set, m, dir: tmpdir(name), http: UreqHttp::new(), seen: Mutex::new(vec![]) }
}

impl Rig {
    fn run_with(&self, m: &Manifest, phase: Phase, token: &CancelToken, c: Config, want: &(dyn Fn() -> Option<String> + Send + Sync)) -> Result<Outcome> {
        let listen = |s: &Status| self.seen.lock().unwrap().push(s.clone());
        Session { http: &self.http, dir: self.dir.clone(), manifest: m, cfg: c, free_space: &unlimited, listener: &listen, want }.run(phase, token)
    }
    fn run(&self, phase: Phase) -> Result<Outcome> {
        self.run_with(&self.m, phase, &CancelToken::new(), cfg(), &nothing)
    }
    fn has(&self, name: &str) -> bool {
        self.dir.join(name).exists()
    }
    fn names(&self) -> Vec<String> {
        let mut v: Vec<String> = std::fs::read_dir(&self.dir).unwrap().filter_map(|e| e.ok()?.file_name().into_string().ok()).collect();
        v.sort();
        v
    }
    fn whole(&self, files: &[(String, Vec<u8>)]) {
        for (n, d) in files {
            assert_eq!(&std::fs::read(self.dir.join(n)).unwrap_or_else(|_| panic!("{n} missing")), d, "{n}");
        }
        let left: Vec<_> = self.names().into_iter().filter(|n| n.contains(".part")).collect();
        assert!(left.is_empty(), "left over: {left:?}");
    }
    fn last(&self) -> Status {
        self.seen.lock().unwrap().last().cloned().unwrap()
    }
    fn old_install(&self, music: bool) -> Vec<String> {
        let mut names = vec!["xonotic-20261107-data.pk3", "xonotic-20261107-maps.pk3", "xonotic-20261107-xoncompat.pk3", "font-unifont-20261107.pk3"];
        if music {
            names.push("xonotic-20261107-music.pk3");
        }
        for n in &names {
            std::fs::write(self.dir.join(n), b"old pack").unwrap();
        }
        names.into_iter().map(String::from).collect()
    }
}

#[test]
fn manifest_checks() {
    let srv = Server::start();
    let set = make_set("t-1", "20261107", 1, &[], &srv.archive_url());
    let m = Manifest::parse(&set.manifest).unwrap();
    assert_eq!((m.packs.len(), m.first_play.len(), m.maps.len()), (8, 3, 3));
    assert_eq!(m.first_play_size(), 410_000);
    assert_eq!(manifest_url("https://h/x/data-slim/Godnotic-data.zip", "dxt"), "https://h/x/data-slim/godnotic-data-dxt.json");
    let edit = |f: &dyn Fn(&mut serde_json::Value)| {
        let mut v: serde_json::Value = serde_json::from_str(&set.manifest).unwrap();
        f(&mut v);
        Manifest::parse(&v.to_string())
    };
    assert!(edit(&|v| v["format"] = 2.into()).is_err(), "unknown format");
    assert!(edit(&|v| v["set"] = "".into()).is_err());
    assert!(edit(&|v| v["archive"]["url"] = "ftp://x/y".into()).is_err());
    assert!(edit(&|v| { v["archive"].as_object_mut().unwrap().remove("url"); }).is_err(), "an unpublished manifest has no url");
    assert!(edit(&|v| v["packs"][0]["install_as"] = "../evil.pk3".into()).is_err());
    assert!(edit(&|v| v["packs"][0]["install_as"] = "xonotic-20261107-data.pk3".into()).is_err(), "never an official name");
    assert!(edit(&|v| v["packs"][0]["crc32"] = "xyz".into()).is_err());
    assert!(edit(&|v| v["packs"][0]["sha256"] = "00".into()).is_err());
    assert!(edit(&|v| v["packs"][7]["offset"] = 99_999_999u64.into()).is_err(), "outside the archive");
    assert!(edit(&|v| v["first_play"] = serde_json::json!(["core", "nope"])).is_err());
    assert!(edit(&|v| v["maps"]["beta"]["needs"] = serde_json::json!(["core", "nope"])).is_err());
    assert!(edit(&|v| v["packs"][1]["name"] = "core".into()).is_err(), "twice");
    assert!(Manifest::parse("[]").is_err() && Manifest::parse("{").is_err());
}

#[test]
fn the_real_manifest_parses_once_it_has_a_url() {
    let Some(home) = std::env::var_os("HOME") else { return };
    let p = Path::new(&home).join(".cache/godnotic-work/data-out/split-G2/godnotic-data.json");
    let Ok(text) = std::fs::read_to_string(p) else { return };
    assert!(Manifest::parse(&text).is_err(), "as built it has no archive.url");
    let mut v: serde_json::Value = serde_json::from_str(&text).unwrap();
    v["archive"]["url"] = "https://example.invalid/Godnotic-data-astc.zip".into();
    let m = Manifest::parse(&v.to_string()).unwrap();
    assert!(m.packs.len() >= 70 && m.first_play.len() >= 10 && m.maps.contains_key("stormkeep"));
    assert!(m.first_play_size() < m.total() / 4);
}

#[test]
fn fetch_falls_back_when_there_is_no_usable_manifest() {
    let r = rig("fallback");
    let t = CancelToken::new();
    assert_eq!(Manifest::fetch(&r.http, &r.srv.manifest_url(), &t).unwrap().unwrap().set, "t-1");
    *r.srv.k.manifest.lock().unwrap() = Some(r.set.manifest.replace("\"format\":1", "\"format\":7"));
    assert!(Manifest::fetch(&r.http, &r.srv.manifest_url(), &t).unwrap().is_none(), "unknown format: the single archive");
    *r.srv.k.manifest.lock().unwrap() = None;
    assert!(Manifest::fetch(&r.http, &r.srv.manifest_url(), &t).unwrap().is_none(), "404: the single archive");
    assert!(Manifest::fetch(&r.http, "http://127.0.0.1:1/x.json", &t).is_err(), "no network is not 'no manifest'");
    assert!(r.m.store(&r.dir));
    assert_eq!(Manifest::stored(&r.dir).unwrap(), r.m);
}

#[test]
fn first_play_then_the_rest() {
    let r = rig("fresh");
    assert!(!Session::playable(&r.dir, &r.m));
    assert_eq!(r.run(Phase::FirstPlay).unwrap(), Outcome::Done);
    // exactly the first_play bytes, nothing of the rest
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst), 410_000);
    assert!(Session::playable(&r.dir, &r.m));
    r.whole(&r.set.files[..3]);
    assert!(!r.has("godnotic-20261107-music.pk3"));
    assert_eq!(r.last().state, "fetching", "not complete: the rest is to come");
    assert_eq!(r.last().done, 410_000);
    assert_eq!(r.last().landed.len(), 3);

    assert_eq!(r.run(Phase::Everything).unwrap(), Outcome::Done);
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst), 900_000, "nothing fetched twice");
    r.whole(&r.set.files);
    let s = r.last();
    assert_eq!((s.state.as_str(), s.done, s.total), ("complete", 900_000, 900_000));
    // landed in this run: the five that were missing, by priority where nothing holds a pack back
    assert_eq!(s.landed.len(), 5);
    assert_eq!(r.srv.k.suffix.load(Ordering::SeqCst), 0, "never a suffix range");
    assert!(r.srv.k.ranges.lock().unwrap().iter().all(|x| x.starts_with("bytes=") && !x.starts_with("bytes=-")));
    let i = Install::read(&r.dir);
    assert_eq!((i.set.as_deref(), i.legacy, i.packs.len()), (Some("t-1"), None, 8));
    // a third run asks the server for nothing
    assert_eq!(r.run(Phase::Everything).unwrap(), Outcome::Done);
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst), 900_000);
}

#[test]
fn a_map_pack_goes_in_after_what_it_needs() {
    let r = rig("maplast");
    // one connection, and the map pack first in line: it must still land last
    let mut v: serde_json::Value = serde_json::from_str(&r.set.manifest).unwrap();
    v["first_play"] = serde_json::json!(["map-alpha", "shared-1", "core"]);
    let m = Manifest::parse(&v.to_string()).unwrap();
    r.run_with(&m, Phase::FirstPlay, &CancelToken::new(), Config { connections: 1, ..cfg() }, &nothing).unwrap();
    let landed = r.last().landed;
    assert_eq!(landed.last().unwrap(), "godnotic-20261107-map-alpha.pk3", "{landed:?}");
    // and every status that lists the map pack lists its needs too
    for s in r.seen.lock().unwrap().iter() {
        if s.landed.iter().any(|l| l.contains("map-alpha")) {
            assert!(s.landed.iter().any(|l| l.contains("shared-1")) && s.landed.iter().any(|l| l.contains("core")));
        }
    }
}

#[test]
fn stop_and_resume_fetches_no_finished_byte_again() {
    let r = rig("resume");
    r.srv.k.delay_per_16k_ms.store(3, Ordering::SeqCst);
    let token = CancelToken::new();
    let t2 = token.clone();
    let k = r.srv.k.clone();
    let stopper = std::thread::spawn(move || {
        while k.served.load(Ordering::SeqCst) < 250_000 {
            std::thread::sleep(Duration::from_millis(2));
        }
        t2.cancel();
    });
    assert_eq!(r.run_with(&r.m, Phase::Everything, &token, cfg(), &nothing), Err(Error::Cancelled));
    stopper.join().unwrap();
    assert_eq!(r.last().state, "paused");
    assert!(r.names().iter().any(|n| n.ends_with(".part.state")), "{:?}", r.names());
    let asked_before = r.srv.k.ranges.lock().unwrap().len();
    r.srv.k.delay_per_16k_ms.store(0, Ordering::SeqCst);
    r.seen.lock().unwrap().clear();
    r.run(Phase::Everything).unwrap();
    r.whole(&r.set.files);
    // what the second run asked for is exactly what the first had not put on disk
    // (the server's own count of bytes written includes what sat in socket buffers at the stop)
    let on_disk = r.seen.lock().unwrap()[0].done;
    assert!(on_disk > 0 && on_disk < 900_000, "progress starts from what is on disk: {on_disk}");
    let asked: u64 = r.srv.k.ranges.lock().unwrap()[asked_before..].iter().map(|x| {
        let (a, b) = x.strip_prefix("bytes=").unwrap().split_once('-').unwrap();
        b.parse::<u64>().unwrap() - a.parse::<u64>().unwrap() + 1
    }).sum();
    assert_eq!(asked, 900_000 - on_disk, "no byte on disk is asked for again");
}

#[test]
fn a_damaged_pack_is_fetched_again_and_given_up_after_three() {
    let r = rig("damaged");
    let music = r.m.pack("music").unwrap().offset as i64;
    r.srv.k.corrupt_at.store(music + 5, Ordering::SeqCst);
    r.srv.k.corrupt_times.store(1, Ordering::SeqCst);
    r.run(Phase::Everything).unwrap();
    r.whole(&r.set.files);
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst), 900_000 + 150_000, "the music twice");

    let r = rig("damaged3");
    let beta = r.m.pack("map-beta").unwrap().offset as i64;
    r.srv.k.corrupt_at.store(beta + 5, Ordering::SeqCst);
    r.srv.k.corrupt_times.store(99, Ordering::SeqCst);
    assert_eq!(r.run(Phase::Everything), Err(Error::Damaged("godnotic-20261107-map-beta.pk3".into())));
    assert!(!r.has("godnotic-20261107-map-beta.pk3") && !r.has("godnotic-20261107-map-beta.pk3.part"), "never a bad pack in place");
    assert_eq!(r.last().state, "error");
    // the others went on
    assert!(r.has("godnotic-20261107-extras.pk3") && r.has("godnotic-20261107-map-gamma.pk3"));
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst), 900_000 + 2 * 60_000);
}

#[test]
fn a_server_that_ignores_or_changes_the_range_is_an_error() {
    let r = rig("norange");
    r.srv.k.ignore_range.store(true, Ordering::SeqCst);
    assert!(matches!(r.run(Phase::FirstPlay), Err(Error::Transient(_))));
    assert!(r.names().iter().all(|n| !n.ends_with(".pk3")), "{:?}", r.names());
    assert_eq!(r.last().state, "waiting");
    r.srv.k.ignore_range.store(false, Ordering::SeqCst);
    r.srv.k.wrong_range.store(true, Ordering::SeqCst);
    assert!(matches!(r.run(Phase::FirstPlay), Err(Error::Transient(_))));
    r.srv.k.wrong_range.store(false, Ordering::SeqCst);
    r.run(Phase::FirstPlay).unwrap();
    r.whole(&r.set.files[..3]);
}

#[test]
fn a_wanted_map_jumps_the_queue() {
    let r = rig("want");
    r.run(Phase::FirstPlay).unwrap();
    r.seen.lock().unwrap().clear();
    r.srv.k.delay_per_16k_ms.store(4, Ordering::SeqCst);
    // by priority gamma is after music, shared-2 and beta
    let want = || Some("gamma".to_string());
    r.run_with(&r.m, Phase::Everything, &CancelToken::new(), Config { connections: 1, ..cfg() }, &want).unwrap();
    let landed = r.last().landed;
    let at = |n: &str| landed.iter().position(|l| l.contains(n)).unwrap();
    assert!(at("shared-2") < at("map-gamma"), "{landed:?}");
    assert!(at("map-gamma") < at("map-beta") && at("map-gamma") < at("extras"), "{landed:?}");
    let seen = r.seen.lock().unwrap();
    let w: Vec<&Status> = seen.iter().filter(|s| s.want.as_deref() == Some("gamma")).collect();
    assert!(!w.is_empty());
    assert!(w.iter().all(|s| s.want_total == 80_000 && s.want_done <= 80_000), "shared-2 + map-gamma");
    assert!(seen.last().unwrap().want.is_none(), "nothing wanted once it is here");
    assert!(seen.last().unwrap().text().starts_with("format 1\nset t-1\nstate complete\ndone_kb 878\ntotal_kb 878\nlanded "));
}

#[test]
fn a_complete_old_install_fetches_nothing() {
    let r = rig("legacy");
    let old = r.old_install(true);
    assert!(Session::playable(&r.dir, &r.m));
    for _ in 0..2 {
        assert_eq!(r.run(Phase::Everything).unwrap(), Outcome::Legacy);
    }
    assert_eq!(r.srv.k.ranges.lock().unwrap().len(), 0, "not one request to the archive");
    assert_eq!(r.last().state, "legacy");
    assert_eq!(std::fs::read_to_string(r.dir.join(STATE)).unwrap(), "legacy t-1\n");
    assert!(old.iter().all(|n| r.has(n)) && !r.names().iter().any(|n| n.starts_with("godnotic-")));

    // a new set is published: pack by pack, the old packs until the last one is in
    let set2 = make_set("t-2", "20261201", 2, &[], &r.srv.archive_url());
    *r.srv.k.archive.lock().unwrap() = set2.archive.clone();
    let m2 = Manifest::parse(&set2.manifest).unwrap();
    assert_eq!(r.run_with(&m2, Phase::FirstPlay, &CancelToken::new(), cfg(), &nothing).unwrap(), Outcome::Done);
    assert!(old.iter().all(|n| r.has(n)), "first_play alone does not replace a complete install");
    assert_eq!(Install::read(&r.dir).legacy.as_deref(), Some("t-1"));
    r.run_with(&m2, Phase::Everything, &CancelToken::new(), cfg(), &nothing).unwrap();
    r.whole(&set2.files);
    assert!(old.iter().all(|n| !r.has(n)), "the new set is whole: the old packs go");
    assert_eq!(Install::read(&r.dir).legacy, None);
}

#[test]
fn an_incomplete_old_install_gives_way_after_first_play() {
    let r = rig("oldpartial");
    let old = r.old_install(false); // no music: never finished
    std::fs::write(r.dir.join("xonotic-20261107-music.pk3.part"), b"half").unwrap();
    std::fs::write(r.dir.join("xonotic-20261107-music.pk3.part.state"), b"x").unwrap();
    std::fs::write(r.dir.join(META), b"id\nid\n").unwrap();
    assert!(!Session::playable(&r.dir, &r.m));
    // stop before first_play is in: the old packs are still there
    r.srv.k.delay_per_16k_ms.store(3, Ordering::SeqCst);
    let token = CancelToken::new();
    let t2 = token.clone();
    let k = r.srv.k.clone();
    let stopper = std::thread::spawn(move || {
        while k.served.load(Ordering::SeqCst) < 100_000 {
            std::thread::sleep(Duration::from_millis(2));
        }
        t2.cancel();
    });
    assert_eq!(r.run_with(&r.m, Phase::FirstPlay, &token, cfg(), &nothing), Err(Error::Cancelled));
    stopper.join().unwrap();
    assert!(old.iter().all(|n| r.has(n)), "never neither");
    assert!(!r.has("xonotic-20261107-music.pk3.part") && !r.has(META), "the old download's parts go at once");
    r.srv.k.delay_per_16k_ms.store(0, Ordering::SeqCst);
    r.run(Phase::FirstPlay).unwrap();
    assert!(old.iter().all(|n| !r.has(n)), "{:?}", r.names());
    r.whole(&r.set.files[..3]);
}

#[test]
fn a_new_set_keeps_what_did_not_change() {
    let r = rig("newset");
    r.run(Phase::Everything).unwrap();
    let served = r.srv.k.served.load(Ordering::SeqCst);
    // same date (same file names): music and shared-1 unchanged, the rest new; and one pack no longer listed
    let set2 = make_set("t-2", "20261107", 2, &["music", "shared-1"], &r.srv.archive_url());
    let mut v: serde_json::Value = serde_json::from_str(&set2.manifest).unwrap();
    let packs = v["packs"].as_array_mut().unwrap();
    packs.retain(|p| p["name"] != "extras");
    let m2 = Manifest::parse(&v.to_string()).unwrap();
    *r.srv.k.archive.lock().unwrap() = set2.archive.clone();
    std::fs::write(r.dir.join("godnotic-20261107-map-beta.pk3.part"), b"of the old set").unwrap();

    r.run_with(&m2, Phase::FirstPlay, &CancelToken::new(), cfg(), &nothing).unwrap();
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst) - served, 300_000 + 70_000, "core and map-alpha; shared-1 kept");
    assert!(!r.has("godnotic-20261107-extras.pk3"), "unlisted packs go once first_play is in");
    assert_eq!(std::fs::read(r.dir.join("godnotic-20261107-map-beta.pk3")).unwrap(), r.set.files[5].1, "the old beta stays until its replacement is renamed over it");
    r.run_with(&m2, Phase::Everything, &CancelToken::new(), cfg(), &nothing).unwrap();
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst) - served, 300_000 + 70_000 + 30_000 + 60_000 + 50_000, "music kept");
    r.whole(&set2.files[..7]);
    assert_eq!(Install::read(&r.dir).set.as_deref(), Some("t-2"));
}

#[test]
fn a_pack_without_its_line_is_hashed_and_adopted_or_replaced() {
    let r = rig("adopt");
    r.run(Phase::FirstPlay).unwrap();
    // killed between the rename and the save: the line is gone
    let mut i = Install::read(&r.dir);
    i.packs.remove("godnotic-20261107-core.pk3");
    i.write(&r.dir);
    // and a file of the right size with the wrong bytes
    std::fs::write(r.dir.join("godnotic-20261107-music.pk3"), vec![7u8; 150_000]).unwrap();
    let before = r.srv.k.served.load(Ordering::SeqCst);
    r.run(Phase::Everything).unwrap();
    assert_eq!(r.srv.k.served.load(Ordering::SeqCst) - before, 900_000 - 410_000, "core adopted, the music fetched");
    r.whole(&r.set.files);
}

#[test]
fn no_space_is_refused_before_anything_is_fetched() {
    let r = rig("nospace");
    let listen = |_: &Status| {};
    let tight = |_: &Path| 100_000u64;
    let s = Session { http: &r.http, dir: r.dir.clone(), manifest: &r.m, cfg: cfg(), free_space: &tight, listener: &listen, want: &nothing };
    assert!(matches!(s.run(Phase::FirstPlay, &CancelToken::new()), Err(Error::NoSpace { needed: 410_000, available: 100_000 })));
    assert_eq!(r.srv.k.ranges.lock().unwrap().len(), 0);
}

#[test]
fn status_and_request_files() {
    let d = tmpdir("files");
    let s = Status { set: "s".into(), state: "fetching".into(), done: 184_320 << 10, total: 363_868 << 10, want: Some("boil".into()), want_done: 2048 << 10, want_total: 9216 << 10, message: String::new(),
        landed: vec!["godnotic-20261107-map-boil.pk3".into()] };
    assert_eq!(s.text(), "format 1\nset s\nstate fetching\ndone_kb 184320\ntotal_kb 363868\nwant boil\nwant_done_kb 2048\nwant_total_kb 9216\nlanded godnotic-20261107-map-boil.pk3\n");
    assert!(s.write(&d));
    assert_eq!(std::fs::read_to_string(d.join(STATUS_FILE)).unwrap(), s.text());
    let start = SystemTime::now() - Duration::from_secs(5);
    assert_eq!(read_request(&d, start), None);
    std::fs::write(d.join(REQUEST_FILE), "want boil 3\n").unwrap();
    assert_eq!(read_request(&d, start), Some(("boil".into(), 3)));
    assert_eq!(read_request(&d, SystemTime::now() + Duration::from_secs(60)), None, "older than the host");
    std::fs::write(d.join(REQUEST_FILE), "want ../x 3\n").unwrap();
    assert_eq!(read_request(&d, start), None);
}
