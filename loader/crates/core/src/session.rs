// SPDX-License-Identifier: GPL-3.0-or-later
//! Starting the engine and learning how a session ended.
//!
//! The engine goes through packaging/start.sh, which stays the one place that
//! prepares a game session (packs, screen metrics, profiles). The loader adds
//! its choices as environment and `+cvar` arguments, and for a direct join the
//! native front end mode of engine/darkplaces/touch_nativefe.c:
//! `GODNOTIC_FRONTEND=native`, `GODNOTIC_FE_RESULT=<file>`, and the join
//! password on an inherited pipe (`GODNOTIC_FE_SECRET_FD=3`), never in argv or
//! the environment. With the UI bridge (work/uibridge) the engine is instead
//! started once as the UI backend and becomes the game itself; [`Backend`]
//! holds that process and its stream.

use crate::config::{keys, Config};
use crate::link::ServerAddress;
use std::collections::BTreeMap;
use std::io::{self, Read, Write};
use std::process::{Child, Command, Stdio};

pub const SECRET_FD: i32 = 3;
/// The desktop app id: the game window carries it too, so the dock shows one Godnotic entry.
pub const APP_ID: &str = "io.github.dixonSolutions.Godnotic";
pub const ENV_MODE: &str = "GODNOTIC_FRONTEND";
pub const ENV_RESULT: &str = "GODNOTIC_FE_RESULT";
pub const ENV_SECRET_FD: &str = "GODNOTIC_FE_SECRET_FD";

#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    /// The game as it always started, with its own menu.
    Play,
    /// Straight into a server (a `godnotic://` link, a typed address).
    Connect { address: ServerAddress, password: Option<String> },
    /// The game opened on one of its own menu dialogs (`menu_cmd directmenu <name>`:
    /// servers, settings, TouchLanHost, touchmods, Credits): what the app routes to
    /// while it does not show that screen itself.
    Menu { dialog: String },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Plan {
    pub program: String,
    pub args: Vec<String>,
    pub env: BTreeMap<String, String>,
    /// Written to the pipe the engine reads as fd [`SECRET_FD`].
    pub secret: Option<String>,
    pub cwd: Option<String>,
}

/// The launcher settings start.sh understands, as environment and `+cvar` arguments
/// (the same mapping as the Kotlin launcher's Launch.game).
pub fn base_args(cfg: &Config) -> (BTreeMap<String, String>, Vec<String>) {
    let mut env = BTreeMap::new();
    let mut put = |k: &str, e: &str| {
        if let Some(v) = cfg.get(k).filter(|v| !v.is_empty()) {
            env.insert(e.to_string(), v.trim().to_string());
        }
    };
    put(keys::TOUCH_PROFILE, "GODNOTIC_PROFILE");
    put(keys::PERF_PROFILE, "GODNOTIC_PERF_PROFILE");
    put(keys::TOUCH_MODE, "GODNOTIC_MODE");
    put(keys::USER_BASE, "GODNOTIC_USER_BASE");
    let mut args: Vec<String> = Vec::new();
    match cfg.get(keys::WINDOW_MODE) {
        Some("fullscreen") => args.extend(["+vid_fullscreen".into(), "1".into()]),
        Some("windowed") => args.extend(["+vid_fullscreen".into(), "0".into()]),
        _ => {}
    }
    if let Some((w, h)) = cfg.get(keys::RESOLUTION).and_then(parse_resolution) {
        args.extend(["+vid_width".into(), w.to_string(), "+vid_height".into(), h.to_string(), "+vid_conheight".into(), h.to_string()]);
    }
    args.extend(split_args(cfg.get(keys::EXTRA_ARGS).unwrap_or("")));
    (env, args)
}

pub fn plan(action: &Action, cfg: &Config, start_script: &str, result_path: &str) -> Plan {
    let (mut env, mut args) = base_args(cfg);
    env.insert(ENV_RESULT.into(), result_path.into());
    // SDL names the game window after the app (X11 WM_CLASS, Wayland app_id): one dock entry.
    env.insert("SDL_VIDEO_X11_WMCLASS".into(), APP_ID.into());
    env.insert("SDL_VIDEO_WAYLAND_WMCLASS".into(), APP_ID.into());
    let mut secret = None;
    if let Action::Menu { dialog } = action {
        if !dialog.is_empty() && dialog.chars().all(|c| c.is_ascii_alphanumeric()) {
            // The menu is up after the first frame; then it opens the dialog.
            args.extend(["+defer".into(), "1".into(), format!("menu_cmd directmenu {dialog}")]);
        }
    }
    if let Action::Connect { address, password } = action {
        env.insert(ENV_MODE.into(), "native".into());
        args.extend(["+connect".into(), address.to_string()]);
        if let Some(p) = password.as_ref().filter(|p| !p.is_empty() && !p.contains('\n')) {
            env.insert(ENV_SECRET_FD.into(), SECRET_FD.to_string());
            secret = Some(format!("join\t{address}\t{p}\n"));
        }
    }
    Plan { program: start_script.into(), args, env, secret, cwd: None }
}

pub fn parse_resolution(s: &str) -> Option<(u32, u32)> {
    let s = s.to_lowercase();
    let (w, h) = s.split_once(['x', '×'])?;
    let (w, h) = (w.trim().parse::<u32>().ok()?, h.trim().parse::<u32>().ok()?);
    ((320..=16384).contains(&w) && (200..=16384).contains(&h)).then_some((w, h))
}

/// Splits like a shell: spaces separate, quotes group, backslash escapes.
pub fn split_args(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quote: Option<char> = None;
    let mut any = false;
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match (quote, c) {
            (Some(q), c) if c == q => quote = None,
            (Some('\''), c) => cur.push(c),
            (_, '\\') if it.peek().is_some() => {
                cur.push(it.next().unwrap_or_default());
                any = true;
            }
            (Some(_), c) => cur.push(c),
            (None, '"' | '\'') => {
                quote = Some(c);
                any = true;
            }
            (None, c) if c.is_whitespace() => {
                if any || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                any = false;
            }
            (None, c) => {
                cur.push(c);
                any = true;
            }
        }
    }
    if any || !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// How a session ended (the result file touch_nativefe.c writes, and the exit status).
#[derive(Clone, Debug, PartialEq)]
pub struct Outcome {
    /// quit, left, kicked, connect_failed, password_required, password_wrong, error, crashed
    pub kind: String,
    pub reason: String,
    pub server: String,
    pub exit_code: i32,
    pub log_path: String,
}

impl Outcome {
    pub fn parse(text: Option<&str>, exit_code: i32, signalled: bool, log_path: &str) -> Self {
        let mk = |kind: &str, reason: String, server: String| Self { kind: kind.into(), reason, server, exit_code, log_path: log_path.into() };
        let Some(text) = text else {
            return if exit_code == 0 && !signalled { mk("quit", String::new(), String::new()) } else { mk("crashed", String::new(), String::new()) };
        };
        let mut kv = BTreeMap::new();
        for line in text.lines() {
            if let Some((k, v)) = line.split_once('=') {
                kv.insert(k.trim().to_string(), v.trim().to_string());
            }
        }
        let kind = kv.get("kind").filter(|k| !k.is_empty()).cloned().unwrap_or_else(|| "quit".into());
        let reason = kv.get("reason").cloned().unwrap_or_default();
        let server = kv.get("server").cloned().unwrap_or_default();
        match kind.as_str() {
            "quit" if signalled => mk("crashed", String::new(), server),
            // Sys_Error exits non-zero through atexit, which wrote kind=quit.
            "quit" if exit_code != 0 => mk("error", format!("exit status {exit_code}"), server),
            _ => mk(&kind, reason, server),
        }
    }

    pub fn wants_password(&self) -> bool {
        matches!(self.kind.as_str(), "password_required" | "password_wrong")
    }

    /// One sentence for a toast or a status page; None when there is nothing to say.
    pub fn message(&self) -> Option<String> {
        let clean = |s: &str| {
            let t = s.trim_start_matches("Disconnect: ").trim_start_matches("Connect: ").trim_start_matches("rejected, ");
            let t = t.trim_start_matches("[pw-required]").trim_start_matches("[pw-wrong]");
            crate::bridge::plain_text(t)
        };
        let log = if self.log_path.is_empty() { String::new() } else { format!(". Log: {}", self.log_path) };
        match self.kind.as_str() {
            "quit" | "left" => None,
            "kicked" => Some(if self.reason.is_empty() { "The server disconnected you".into() } else { format!("Disconnected: {}", clean(&self.reason)) }),
            "connect_failed" => Some(if self.reason.is_empty() { "Could not join the server".into() } else { format!("Could not join: {}", clean(&self.reason)) }),
            "password_required" => Some("This game needs a password".into()),
            "password_wrong" => Some("Wrong password. Try again".into()),
            "error" => Some(format!("The game stopped with an error: {}{log}", clean(&self.reason))),
            "crashed" => Some(format!("The game crashed (exit {}){log}", self.exit_code)),
            _ => Some(clean(&self.reason)).filter(|s| !s.is_empty()),
        }
    }
}

/// Starts a plan. Output goes to `log_path`; a secret goes to the child as fd 3 through a pipe.
pub fn spawn(plan: &Plan, log_path: &str) -> io::Result<Child> {
    let log = std::fs::File::create(log_path)?;
    let mut cmd = Command::new(&plan.program);
    cmd.args(&plan.args).envs(&plan.env).stdin(Stdio::null()).stdout(log.try_clone()?).stderr(log);
    if let Some(cwd) = &plan.cwd {
        cmd.current_dir(cwd);
    }
    #[cfg(unix)]
    if let Some(secret) = &plan.secret {
        let (reader, mut writer) = std::io::pipe()?;
        // A password is short: it fits the pipe buffer, so writing before the spawn cannot block.
        writer.write_all(secret.as_bytes())?;
        drop(writer);
        fd::give_fd(&mut cmd, reader, SECRET_FD);
        return cmd.spawn();
    }
    cmd.spawn()
}

#[cfg(unix)]
mod fd {
    use std::os::fd::{AsRawFd, OwnedFd};
    use std::os::unix::process::CommandExt;
    use std::process::Command;

    /// The child gets `fd` as descriptor `target` (dup2 in the child before exec).
    pub fn give_fd(cmd: &mut Command, fd: impl Into<OwnedFd>, target: i32) {
        let fd: OwnedFd = fd.into();
        let raw = fd.as_raw_fd();
        // SAFETY: between fork and exec only async-signal-safe calls are made (dup2, fcntl);
        // `fd` lives in the closure until the Command is dropped.
        unsafe {
            cmd.pre_exec(move || {
                let _keep = &fd;
                if libc::dup2(raw, target) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
                let flags = libc::fcntl(target, libc::F_GETFD);
                libc::fcntl(target, libc::F_SETFD, flags & !libc::FD_CLOEXEC);
                Ok(())
            });
        }
    }
}

/// The engine as UI backend (work/uibridge): the process and its JSON stream
/// (newline-delimited, over the child's stdin and stdout until the bridge's
/// transport is final). Reading runs on a thread; messages come out of
/// [`Backend::messages`].
pub struct Backend {
    pub child: Child,
    input: std::process::ChildStdin,
    pub messages: std::sync::mpsc::Receiver<serde_json::Map<String, serde_json::Value>>,
}

impl Backend {
    pub fn start(mut cmd: Command) -> io::Result<Self> {
        let mut child = cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).spawn()?;
        let input = child.stdin.take().ok_or_else(|| io::Error::other("no stdin"))?;
        let mut out = child.stdout.take().ok_or_else(|| io::Error::other("no stdout"))?;
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::Builder::new().name("uib-read".into()).spawn(move || {
            let mut framer = crate::bridge::LineFramer::new();
            let mut buf = [0u8; 65536];
            while let Ok(n) = out.read(&mut buf) {
                if n == 0 {
                    break;
                }
                for m in framer.feed(&buf[..n]) {
                    if tx.send(m).is_err() {
                        return;
                    }
                }
            }
        })?;
        Ok(Self { child, input, messages: rx })
    }

    /// Sends one message (a line of JSON).
    pub fn send(&mut self, line: &str) -> io::Result<()> {
        self.input.write_all(line.as_bytes())?;
        self.input.write_all(b"\n")?;
        self.input.flush()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn connect_keeps_the_password_out_of_argv_and_env() {
        let a = Action::Connect { address: ServerAddress::parse("192.168.1.23:26001").unwrap(), password: Some("hunter2 x".into()) };
        let mut cfg = Config::default();
        cfg.set(keys::WINDOW_MODE, "windowed");
        cfg.set(keys::TOUCH_PROFILE, "casual");
        cfg.set(keys::EXTRA_ARGS, "+cl_maxfps 90 \"+name\" 'a b'");
        let p = plan(&a, &cfg, "/app/bin/start.sh", "/tmp/r");
        assert_eq!(p.env.get(ENV_MODE).map(String::as_str), Some("native"));
        assert_eq!(p.env.get(ENV_SECRET_FD).map(String::as_str), Some("3"));
        assert_eq!(p.env.get("GODNOTIC_PROFILE").map(String::as_str), Some("casual"));
        assert_eq!(p.args, vec!["+vid_fullscreen", "0", "+cl_maxfps", "90", "+name", "a b", "+connect", "192.168.1.23:26001"]);
        assert_eq!(p.secret.as_deref(), Some("join\t192.168.1.23:26001\thunter2 x\n"));
        assert!(p.args.iter().chain(p.env.values()).all(|x| !x.contains("hunter2")));
        let menu = plan(&Action::Menu { dialog: "servers".into() }, &Config::default(), "/s", "/r");
        assert_eq!(menu.args, vec!["+defer", "1", "menu_cmd directmenu servers"]);
        assert!(!menu.env.contains_key(ENV_MODE));
        let bad = plan(&Action::Menu { dialog: "x; quit".into() }, &Config::default(), "/s", "/r");
        assert!(bad.args.is_empty());
        let play = plan(&Action::Play, &Config::default(), "/s", "/r");
        assert!(!play.env.contains_key(ENV_MODE) && play.secret.is_none());
    }

    #[test]
    fn outcomes() {
        let k = Outcome::parse(Some("kind=kicked\nreason=Disconnect: You were kicked: AFK\nserver=1.2.3.4:26000\n"), 0, false, "");
        assert_eq!(k.message().unwrap(), "Disconnected: You were kicked: AFK");
        assert!(Outcome::parse(Some("kind=password_wrong\n"), 0, false, "").wants_password());
        assert_eq!(Outcome::parse(Some("kind=left\n"), 0, false, "").message(), None);
        assert_eq!(Outcome::parse(None, 139, true, "/l").kind, "crashed");
        assert!(Outcome::parse(None, 139, true, "/l").message().unwrap().contains("/l"));
        assert_eq!(Outcome::parse(None, 0, false, "").kind, "quit");
        assert_eq!(Outcome::parse(Some("kind=quit\n"), 1, false, "").kind, "error");
        assert_eq!(Outcome::parse(Some("kind=connect_failed\nreason=Connect: rejected, [pw-required] This game needs a password.\n"), 0, false, "").message().unwrap(),
            "Could not join: This game needs a password.");
        assert_eq!(parse_resolution("1920x1080"), Some((1920, 1080)));
        assert_eq!(parse_resolution("10x10"), None);
    }

    #[cfg(unix)]
    #[test]
    fn the_secret_arrives_on_fd_3_and_not_in_the_command_line() {
        let dir = std::env::temp_dir().join(format!("godnotic-secret-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let out = dir.join("out");
        let script = format!("cat <&3 > '{}'; tr '\\0' ' ' < /proc/$$/cmdline >> '{}'", out.display(), out.display());
        let p = Plan { program: "/bin/sh".into(), args: vec!["-c".into(), script], env: BTreeMap::new(), secret: Some("join\t1.2.3.4:26000\ts3cret\n".into()), cwd: None };
        let log = dir.join("log");
        let st = spawn(&p, log.to_str().unwrap()).unwrap().wait().unwrap();
        assert!(st.success());
        let got = std::fs::read_to_string(&out).unwrap();
        assert!(got.starts_with("join\t1.2.3.4:26000\ts3cret\n"), "{got}");
        assert_eq!(got.matches("s3cret").count(), 1, "only on fd 3: {got}");
        std::fs::remove_dir_all(&dir).ok();
    }
}
