// SPDX-License-Identifier: GPL-3.0-or-later
//! The pack downloader against a local stand-in for GitHub's release storage
//! (Range, If-Range, ETag, 501 for suffix ranges), ported from the Kotlin
//! launcher's PackFetchTest: plan, full run, resume after a stop and after a
//! kill, an archive that changes mid-run, a finished pack of an old build,
//! CRC failure, 503 retries, 404 and the slim-to-full fallback, no space,
//! roles, and the chunk-state file format the Android app shares.

use godnotic_core::fetch::datasets::{self, Choice, Kind};
use godnotic_core::fetch::http_ureq::UreqHttp;
use godnotic_core::fetch::*;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

// ------------------------------------------------------------- the archive

struct ZipEntry {
    name: String,
    data: Vec<u8>,
    deflate: bool,
}

fn crc(b: &[u8]) -> u32 {
    let mut c = crc32::Crc32::new();
    c.update(b);
    c.value()
}

/// A zip with the given entries (stored or deflated), the way release archives are built.
fn make_zip(entries: &[ZipEntry]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut cd = Vec::new();
    for e in entries {
        let (method, body) = if e.deflate { (8u16, miniz_oxide::deflate::compress_to_vec(&e.data, 6)) } else { (0u16, e.data.clone()) };
        let off = out.len() as u32;
        let c = crc(&e.data);
        let extra = b"xt";
        out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        out.extend_from_slice(&[20, 0, 0, 0]);
        out.extend_from_slice(&method.to_le_bytes());
        out.extend_from_slice(&[0; 4]);
        out.extend_from_slice(&c.to_le_bytes());
        out.extend_from_slice(&(body.len() as u32).to_le_bytes());
        out.extend_from_slice(&(e.data.len() as u32).to_le_bytes());
        out.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
        out.extend_from_slice(&(extra.len() as u16).to_le_bytes());
        out.extend_from_slice(e.name.as_bytes());
        out.extend_from_slice(extra);
        out.extend_from_slice(&body);
        cd.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        cd.extend_from_slice(&[20, 0, 20, 0, 0, 0]);
        cd.extend_from_slice(&method.to_le_bytes());
        cd.extend_from_slice(&[0; 4]);
        cd.extend_from_slice(&c.to_le_bytes());
        cd.extend_from_slice(&(body.len() as u32).to_le_bytes());
        cd.extend_from_slice(&(e.data.len() as u32).to_le_bytes());
        cd.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
        cd.extend_from_slice(&[0; 12]);
        cd.extend_from_slice(&off.to_le_bytes());
        cd.extend_from_slice(e.name.as_bytes());
    }
    let cd_off = out.len() as u32;
    let n = entries.len() as u16;
    out.extend_from_slice(&cd);
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&(cd.len() as u32).to_le_bytes());
    out.extend_from_slice(&cd_off.to_le_bytes());
    out.extend_from_slice(&[0, 0]);
    out
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

fn build(date: &str, seed: u64) -> (Vec<u8>, Vec<(String, Vec<u8>)>) {
    let files = vec![
        (format!("xonotic-{date}-data.pk3"), bytes(seed, 300_000), false),
        (format!("xonotic-{date}-maps.pk3"), bytes(seed + 1, 200_000), false),
        (format!("xonotic-{date}-music.pk3"), bytes(seed + 2, 90_000), false),
        ("font-xolonium.pk3".to_string(), b"font font font ".repeat(4000), true),
    ];
    let mut entries: Vec<ZipEntry> = files.iter().map(|(n, d, z)| ZipEntry { name: format!("Xonotic/data/{n}"), data: d.clone(), deflate: *z }).collect();
    entries.push(ZipEntry { name: "Xonotic/readme.txt".into(), data: b"skip me".to_vec(), deflate: false });
    entries.push(ZipEntry { name: "Xonotic/data/sub/deeper.pk3".into(), data: b"skip".to_vec(), deflate: false });
    entries.push(ZipEntry { name: "Xonotic/data/.hidden.pk3".into(), data: b"skip".to_vec(), deflate: false });
    (make_zip(&entries), files.into_iter().map(|(n, d, _)| (n, d)).collect())
}

// ---------------------------------------------------------- the server

#[derive(Default)]
struct Knobs {
    archive: Mutex<Vec<u8>>,
    etag: Mutex<String>,
    served: AtomicU64,
    requests: Mutex<Vec<(String, Option<String>)>>,
    delay_per_16k_ms: AtomicU64,
    corrupt_at: AtomicI64,
    fail503: AtomicI64,
    swap_after: AtomicI64,
    next: Mutex<Option<(Vec<u8>, String)>>,
    missing: AtomicBool,
}

struct RangeServer {
    k: Arc<Knobs>,
    url: String,
}

impl RangeServer {
    fn start(archive: Vec<u8>) -> Self {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("http://127.0.0.1:{}/Godnotic-data.zip", l.local_addr().unwrap().port());
        let k = Arc::new(Knobs::default());
        *k.archive.lock().unwrap() = archive;
        *k.etag.lock().unwrap() = "\"build-a\"".into();
        k.corrupt_at.store(-1, Ordering::SeqCst);
        k.swap_after.store(-1, Ordering::SeqCst);
        let k2 = k.clone();
        std::thread::spawn(move || {
            for s in l.incoming().flatten() {
                let k3 = k2.clone();
                std::thread::spawn(move || {
                    let _ = serve(s, &k3);
                });
            }
        });
        Self { k, url }
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
        let (mut range, mut if_range) = (None, None);
        loop {
            let mut h = String::new();
            r.read_line(&mut h)?;
            let h = h.trim_end();
            if h.is_empty() {
                break;
            }
            if let Some((a, b)) = h.split_once(':') {
                match a.to_ascii_lowercase().as_str() {
                    "range" => range = Some(b.trim().to_string()),
                    "if-range" => if_range = Some(b.trim().to_string()),
                    _ => {}
                }
            }
        }
        // A new build appears once enough has been served.
        let after = k.swap_after.load(Ordering::SeqCst);
        if after >= 0 && k.served.load(Ordering::SeqCst) as i64 >= after {
            if let Some((a, t)) = k.next.lock().unwrap().take() {
                *k.archive.lock().unwrap() = a;
                *k.etag.lock().unwrap() = t;
            }
        }
        k.requests.lock().unwrap().push((range.clone().unwrap_or_default(), if_range.clone()));
        let data = k.archive.lock().unwrap().clone();
        let tag = k.etag.lock().unwrap().clone();
        let head = |code: &str, extra: &str, len: usize| format!("HTTP/1.1 {code}\r\nETag: {tag}\r\nAccept-Ranges: bytes\r\nContent-Length: {len}\r\n{extra}\r\n");
        if path != "/Godnotic-data.zip" || k.missing.load(Ordering::SeqCst) {
            w.write_all(head("404 Not Found", "", 0).as_bytes())?;
            continue;
        }
        if range.as_deref().is_some_and(|r| r.starts_with("bytes=-")) {
            w.write_all(head("501 Not Implemented", "", 0).as_bytes())?;
            continue;
        }
        let parsed = range.as_deref().and_then(|r| r.strip_prefix("bytes=")).and_then(|r| r.split_once('-')).and_then(|(a, b)| Some((a.parse::<usize>().ok()?, b.parse::<usize>().ok()?)));
        let full = parsed.is_none() || if_range.as_ref().is_some_and(|t| *t != tag);
        let (code, from, to) = if full { ("200 OK", 0, data.len() - 1) } else {
            let (a, b) = parsed.unwrap();
            if range.as_deref() != Some("bytes=0-0") && k.fail503.load(Ordering::SeqCst) > 0 {
                k.fail503.fetch_sub(1, Ordering::SeqCst);
                w.write_all(head("503 Service Unavailable", "", 0).as_bytes())?;
                continue;
            }
            ("206 Partial Content", a, b.min(data.len() - 1))
        };
        let mut body = data[from..=to].to_vec();
        let c = k.corrupt_at.load(Ordering::SeqCst);
        if c >= from as i64 && c <= to as i64 {
            body[(c as usize) - from] ^= 0xFF;
            k.corrupt_at.store(-1, Ordering::SeqCst);
        }
        let cr = if full { String::new() } else { format!("Content-Range: bytes {from}-{to}/{}\r\n", data.len()) };
        w.write_all(head(code, &cr, body.len()).as_bytes())?;
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

// --------------------------------------------------------------- helpers

fn tmpdir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("godnotic-fetch-{name}-{}", std::process::id()));
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

fn plan_of(http: &UreqHttp, url: &str) -> Plan {
    plan(http, url, &CancelToken::new(), datasets::FOLDER).unwrap()
}

fn run(http: &UreqHttp, dir: &Path, p: &Plan, c: Config, token: &CancelToken) -> Result<()> {
    let listen = |_: Progress| {};
    PackFetch::new(http, dir, p, c, &unlimited, &listen).run(token, true)
}

fn installed_ok(dir: &Path, files: &[(String, Vec<u8>)]) {
    for (n, d) in files {
        assert_eq!(&std::fs::read(dir.join(n)).unwrap_or_else(|_| panic!("{n} missing")), d, "{n}");
    }
    let leftovers: Vec<_> = std::fs::read_dir(dir).unwrap().filter_map(|e| e.ok()?.file_name().into_string().ok()).filter(|n| n.contains(".part") || n == META).collect();
    assert!(leftovers.is_empty(), "left over: {leftovers:?}");
}

// ---------------------------------------------------------------- tests

#[test]
fn plan_reads_only_data_packs_and_never_a_suffix_range() {
    let (zip, files) = build("20261107", 1);
    let s = RangeServer::start(zip.clone());
    let http = UreqHttp::new();
    let p = plan_of(&http, &s.url);
    let mut names: Vec<_> = p.entries.iter().map(|e| e.file_name.clone()).collect();
    names.sort();
    let mut want: Vec<_> = files.iter().map(|f| f.0.clone()).collect();
    want.sort();
    assert_eq!(names, want);
    assert_eq!(p.size, zip.len() as u64);
    assert_eq!(p.etag.as_deref(), Some("\"build-a\""));
    let reqs = s.k.requests.lock().unwrap().clone();
    assert_eq!(reqs[0].0, "bytes=0-0");
    assert!(reqs.iter().all(|r| !r.0.starts_with("bytes=-")), "{reqs:?}");
    // The server really does refuse suffix ranges, as GitHub's storage does.
    let mut code = 0;
    let _ = http.get(&s.url, &[("Range", "bytes=-100".into())], &CancelToken::new(), &mut |h| { code = h.code; Ok(()) }, &mut |_| Ok(()));
    assert_eq!(code, 501);
}

#[test]
fn full_download_installs_every_pack_and_marks_it_ready() {
    let (zip, files) = build("20261107", 2);
    let s = RangeServer::start(zip);
    let http = UreqHttp::new();
    let dir = tmpdir("full");
    let p = plan_of(&http, &s.url);
    let seen = Mutex::new(Vec::new());
    let listen = |pr: Progress| seen.lock().unwrap().push(pr);
    PackFetch::new(&http, &dir, &p, cfg(), &unlimited, &listen).run(&CancelToken::new(), true).unwrap();
    installed_ok(&dir, &files);
    datasets::mark_installed(&dir, &p);
    assert!(dir.join(datasets::READY_MARKER).exists());
    assert!(dir.join(datasets::MUSIC_MARKER).exists(), "music is always part of the set");
    let last = seen.lock().unwrap().last().cloned().unwrap();
    assert_eq!(last.done, last.total);
    // Nothing left to do: a second run fetches nothing.
    let before = s.k.served.load(Ordering::SeqCst);
    run(&http, &dir, &p, cfg(), &CancelToken::new()).unwrap();
    assert_eq!(s.k.served.load(Ordering::SeqCst), before);
}

#[test]
fn a_stop_resumes_without_fetching_done_bytes_again() {
    let (zip, files) = build("20261107", 3);
    let s = RangeServer::start(zip.clone());
    s.k.delay_per_16k_ms.store(60, Ordering::SeqCst);
    let http = UreqHttp::new();
    let dir = tmpdir("resume");
    let p = plan_of(&http, &s.url);
    let token = CancelToken::new();
    let t2 = token.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(400));
        t2.cancel();
    });
    let r = run(&http, &dir, &p, cfg(), &token);
    assert_eq!(r, Err(Error::Cancelled));
    let names: Vec<String> = std::fs::read_dir(&dir).unwrap().filter_map(|e| e.ok()?.file_name().into_string().ok()).collect();
    assert!(names.iter().any(|n| n.ends_with(".part.state")), "state saved on stop: {names:?}");
    let done = need(&dir, &todo(&dir, &p, true), &p, &cfg()).resumed;
    assert!(done > 0);
    s.k.delay_per_16k_ms.store(0, Ordering::SeqCst);
    let before = s.k.served.load(Ordering::SeqCst);
    run(&http, &dir, &p, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files);
    let second = s.k.served.load(Ordering::SeqCst) - before;
    assert!(second < zip.len() as u64 - done / 2, "refetched too much: {second} of {} (resumed {done})", zip.len());
}

#[test]
fn a_kill_with_stale_state_still_finishes_correctly() {
    let (zip, files) = build("20261107", 4);
    let s = RangeServer::start(zip);
    let http = UreqHttp::new();
    let dir = tmpdir("kill");
    let p = plan_of(&http, &s.url);
    // A killed run: parts with more bytes on disk than the state says (state lags), and a state for a missing part.
    let e = p.entries.iter().find(|e| e.method == 0).unwrap();
    std::fs::write(part_file(&dir, e), vec![0u8; 100]).unwrap();
    let st = ChunkState::load(&state_file(&dir, e), &part_file(&dir, e), e.csize, cfg().chunk, false);
    st.save();
    run(&http, &dir, &p, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files);
}

#[test]
fn a_new_build_mid_run_is_caught_and_old_parts_discarded() {
    let (zip_a, _) = build("20261107", 5);
    let (zip_b, files_b) = build("20261120", 6);
    let s = RangeServer::start(zip_a);
    s.k.delay_per_16k_ms.store(5, Ordering::SeqCst);
    *s.k.next.lock().unwrap() = Some((zip_b, "\"build-b\"".into()));
    let http = UreqHttp::new();
    let dir = tmpdir("changed");
    let p = plan_of(&http, &s.url);
    s.k.swap_after.store(150_000, Ordering::SeqCst);
    let r = run(&http, &dir, &p, cfg(), &CancelToken::new());
    assert_eq!(r, Err(Error::ArchiveChanged));
    s.k.delay_per_16k_ms.store(0, Ordering::SeqCst);
    // Try again plans the new build; parts of the old one go.
    let p2 = plan_of(&http, &s.url);
    assert_ne!(p2.identity, p.identity);
    run(&http, &dir, &p2, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files_b);
}

#[test]
fn a_finished_pack_of_the_old_build_is_replaced() {
    let (zip_a, files_a) = build("20261107", 7);
    let s = RangeServer::start(zip_a);
    let http = UreqHttp::new();
    let dir = tmpdir("oldpack");
    let p = plan_of(&http, &s.url);
    // A setup of build A finished one pack and stopped.
    let e = &p.entries[0];
    std::fs::write(dir.join(&e.file_name), &files_a.iter().find(|f| f.0 == e.file_name).unwrap().1).unwrap();
    let m = Meta { identity: Some(p.identity.clone()), want: p.entries.iter().map(|e| e.role()).collect(), finished: vec![e.file_name.clone()] };
    assert!(m.write(&dir));
    // Build B (same file names, other content and size) is published.
    let (zip_b, files_b) = build("20261107", 8);
    let mut files_b = files_b;
    files_b[0].1.extend_from_slice(b"longer");
    let entries: Vec<ZipEntry> = files_b.iter().enumerate().map(|(i, (n, d))| ZipEntry { name: format!("Xonotic/data/{n}"), data: d.clone(), deflate: i == 3 }).collect();
    let _ = zip_b;
    *s.k.archive.lock().unwrap() = make_zip(&entries);
    *s.k.etag.lock().unwrap() = "\"build-b\"".into();
    let p2 = plan_of(&http, &s.url);
    run(&http, &dir, &p2, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files_b);
}

#[test]
fn a_crc_failure_removes_the_part_and_retry_fetches_it_again() {
    let (zip, files) = build("20261107", 9);
    let s = RangeServer::start(zip);
    let http = UreqHttp::new();
    let dir = tmpdir("crc");
    let p = plan_of(&http, &s.url);
    let e = p.entries.iter().find(|e| e.method == 0 && e.file_name.contains("maps")).unwrap();
    // Somewhere inside the maps pack's data.
    s.k.corrupt_at.store((e.offset + 30 + e.file_name.len() as u64 + 13 + 2 + 5000) as i64, Ordering::SeqCst);
    let r = run(&http, &dir, &p, cfg(), &CancelToken::new());
    assert!(matches!(r, Err(Error::Damaged(ref f)) if f.contains("maps")), "{r:?}");
    assert!(!part_file(&dir, e).exists());
    run(&http, &dir, &p, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files);
}

#[test]
fn transient_server_errors_are_retried() {
    let (zip, files) = build("20261107", 10);
    let s = RangeServer::start(zip);
    let http = UreqHttp::new();
    let dir = tmpdir("503");
    let p = plan_of(&http, &s.url);
    s.k.fail503.store(3, Ordering::SeqCst);
    run(&http, &dir, &p, cfg(), &CancelToken::new()).unwrap();
    installed_ok(&dir, &files);
    assert_eq!(s.k.fail503.load(Ordering::SeqCst), 0);
}

#[test]
fn a_missing_archive_fails_at_once_and_automatic_slim_falls_back_to_full() {
    let (zip, _) = build("20261107", 11);
    let full = RangeServer::start(zip);
    let slim = RangeServer::start(vec![0; 10]);
    slim.k.missing.store(true, Ordering::SeqCst);
    let http = UreqHttp::new();
    let r = plan(&http, &slim.url, &CancelToken::new(), datasets::FOLDER);
    assert!(matches!(r, Err(Error::Http { code: 404, .. })));
    let auto = Choice { kind: Kind::Slim, url: slim.url.clone(), reason: String::new(), automatic: true };
    let (c, p) = datasets::plan_choice(&http, &auto, &CancelToken::new(), &full.url).unwrap();
    assert_eq!(c.kind, Kind::Full);
    assert!(!p.entries.is_empty());
    let chosen = Choice { automatic: false, ..auto };
    assert!(datasets::plan_choice(&http, &chosen, &CancelToken::new(), &full.url).is_err(), "a slim set chosen by hand reports the 404");
}

#[test]
fn no_space_stops_before_fetching_anything() {
    let (zip, _) = build("20261107", 12);
    let s = RangeServer::start(zip);
    let http = UreqHttp::new();
    let dir = tmpdir("nospace");
    let p = plan_of(&http, &s.url);
    let before = s.k.served.load(Ordering::SeqCst);
    let tiny = |_: &Path| 1000u64;
    let listen = |_: Progress| {};
    let r = PackFetch::new(&http, &dir, &p, cfg(), &tiny, &listen).run(&CancelToken::new(), true);
    assert!(matches!(r, Err(Error::NoSpace { available: 1000, .. })));
    assert_eq!(s.k.served.load(Ordering::SeqCst), before);
}

#[test]
fn roles_match_any_build_date() {
    assert_eq!(role("xonotic-20261107-maps.pk3"), "xonotic-*-maps.pk3");
    assert_eq!(role("xonotic-20261029_2-maps.pk3"), "xonotic-*-maps.pk3");
    assert_eq!(role("font-xolonium.pk3"), "font-xolonium.pk3");
    let dir = tmpdir("roles");
    std::fs::write(dir.join("xonotic-20250101-maps.pk3"), b"x").unwrap();
    assert!(installed(&dir, "xonotic-*-maps.pk3"));
    assert!(!installed(&dir, "xonotic-*-data.pk3"));
}

#[test]
fn chunk_state_is_the_android_format() {
    let dir = tmpdir("state");
    let part = dir.join("a.pk3.part");
    std::fs::write(&part, b"").unwrap();
    let st = ChunkState::load(&dir.join("a.pk3.part.state"), &part, 20 * 1024 * 1024 + 5, 8 << 20, true);
    assert_eq!(st.chunks, 3);
    // No state yet: a part without one is cleaned away; this run writes both.
    std::fs::write(&part, b"").unwrap();
    st.advance(0, 1000);
    st.advance(2, 5);
    st.save();
    let b = std::fs::read(dir.join("a.pk3.part.state")).unwrap();
    // DataOutputStream: int 0x58545043, long csize, int count, ints.
    assert_eq!(&b[0..4], &[0x58, 0x54, 0x50, 0x43]);
    assert_eq!(&b[4..12], &(20i64 * 1024 * 1024 + 5).to_be_bytes());
    assert_eq!(&b[12..16], &3i32.to_be_bytes());
    assert_eq!(&b[16..20], &1000i32.to_be_bytes());
    assert_eq!(&b[24..28], &5i32.to_be_bytes());
    let back = ChunkState::load(&dir.join("a.pk3.part.state"), &part, 20 * 1024 * 1024 + 5, 8 << 20, true);
    assert_eq!((back.pos(0), back.pos(1), back.pos(2)), (1000, 0, 5));
    // A state that does not fit the entry starts over.
    let other = ChunkState::load(&dir.join("a.pk3.part.state"), &part, 999, 8 << 20, true);
    assert_eq!(other.total(), 0);
    assert!(!part.exists());
}

#[test]
fn cancel_token_runs_late_hooks() {
    let t = CancelToken::new();
    let hit = Arc::new(AtomicBool::new(false));
    let h2 = hit.clone();
    t.cancel();
    t.register(Box::new(move || h2.store(true, Ordering::SeqCst)));
    assert!(hit.load(Ordering::SeqCst));
    let mut buf = Vec::new();
    let _ = std::io::Cursor::new(b"x").read_to_end(&mut buf);
}

#[test]
fn data_set_choice() {
    assert_eq!(datasets::choose(None, Some("slim"), None, None).kind, Kind::Slim);
    assert_eq!(datasets::choose(Some("full"), None, Some(true), None).kind, Kind::Full);
    let c = datasets::choose(None, None, Some(true), Some("Mali-G68"));
    assert!(c.automatic && c.kind == Kind::Slim && c.reason.contains("Mali-G68"));
    assert_eq!(datasets::choose(None, None, None, None).kind, Kind::Full);
    assert_eq!(datasets::choose(None, Some("https://x/y.zip"), None, None).url, "https://x/y.zip");
}
