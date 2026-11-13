// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the game lives. Mirrors packaging/start.sh, which stays the one place
//! that prepares a session (and the Kotlin launcher's PathResolver).

use std::collections::HashMap;

#[derive(Clone, Debug, PartialEq)]
pub struct GamePaths {
    /// Install root holding bin/xonotic and bin/start.sh (/app in the Flatpak).
    pub app_root: Option<String>,
    pub start_script: Option<String>,
    pub engine: Option<String>,
    /// start.sh's USER_BASE: the engine's cwd; packs in `<user_base>/data`.
    pub user_base: String,
    /// The engine's userdir (~/.xonotic): config, Lua mods.
    pub engine_user_dir: String,
    pub flatpak: bool,
}

impl GamePaths {
    pub fn user_data(&self) -> String {
        format!("{}/data", self.user_base)
    }

    /// Folders the engine searches for packs, highest priority first.
    pub fn data_dirs(&self) -> Vec<String> {
        let mut v = vec![format!("{}/data", self.engine_user_dir), self.user_data()];
        if let Some(r) = &self.app_root {
            v.push(format!("{r}/data"));
        }
        v.dedup();
        v
    }

    /// Where the launcher's own settings live.
    pub fn config_file(env: &HashMap<String, String>) -> String {
        let base = env.get("XDG_CONFIG_HOME").filter(|s| !s.is_empty()).cloned()
            .unwrap_or_else(|| format!("{}/.config", env.get("HOME").map(String::as_str).unwrap_or("/tmp")));
        format!("{base}/xonotic-touch/launcher.conf")
    }

    /// `exists` is injected so this stays testable.
    pub fn resolve(env: &HashMap<String, String>, exe_dir: Option<&str>, exists: &dyn Fn(&str) -> bool, user_base_override: &str) -> Self {
        let get = |k: &str| env.get(k).filter(|s| !s.is_empty()).cloned();
        let home = get("HOME").unwrap_or_else(|| "/tmp".into());
        let flatpak = get("FLATPAK_ID").is_some();
        let mut roots: Vec<String> = Vec::new();
        if let Some(r) = get("XONOTIC_TOUCH_APP_ROOT") { roots.push(r); }
        if flatpak { roots.push("/app".into()); }
        if let Some(d) = exe_dir { roots.push(parent(d)); }
        roots.push("/app".into());
        let app_root = roots.into_iter().find(|r| exists(&format!("{r}/bin/start.sh")) || exists(&format!("{r}/bin/xonotic")));
        let start = get("XONOTIC_TOUCH_START").filter(|s| exists(s))
            .or_else(|| app_root.as_ref().map(|r| format!("{r}/bin/start.sh")).filter(|s| exists(s)));
        let engine = app_root.as_ref().map(|r| format!("{r}/bin/xonotic")).filter(|s| exists(s));
        let user_base = if !user_base_override.trim().is_empty() {
            user_base_override.trim().trim_end_matches('/').to_string()
        } else if let Some(b) = get("XONOTIC_TOUCH_USER_BASE") {
            b
        } else if let (true, Some(x)) = (flatpak, get("XDG_DATA_HOME")) {
            format!("{x}/xonotic-touch")
        } else {
            format!("{home}/.local/share/xonotic-touch")
        };
        Self { app_root, start_script: start, engine, user_base, engine_user_dir: format!("{home}/.xonotic"), flatpak }
    }
}

pub fn parent(path: &str) -> String {
    let p = path.trim_end_matches('/');
    match p.rfind('/') {
        Some(0) | None => "/".into(),
        Some(i) => p[..i].into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env(kv: &[(&str, &str)]) -> HashMap<String, String> {
        kv.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    #[test]
    fn flatpak_and_plain_installs() {
        let e = env(&[("HOME", "/home/p"), ("FLATPAK_ID", "x"), ("XDG_DATA_HOME", "/home/p/.var/app/x/data")]);
        let p = GamePaths::resolve(&e, Some("/app/bin"), &|f| f == "/app/bin/start.sh" || f == "/app/bin/xonotic", "");
        assert_eq!(p.app_root.as_deref(), Some("/app"));
        assert_eq!(p.user_base, "/home/p/.var/app/x/data/xonotic-touch");
        assert_eq!(p.user_data(), "/home/p/.var/app/x/data/xonotic-touch/data");
        assert_eq!(p.data_dirs(), vec!["/home/p/.xonotic/data", "/home/p/.var/app/x/data/xonotic-touch/data", "/app/data"]);
        let q = GamePaths::resolve(&env(&[("HOME", "/h")]), Some("/opt/g/bin"), &|f| f == "/opt/g/bin/start.sh", "/data/g/");
        assert_eq!(q.app_root.as_deref(), Some("/opt/g"));
        assert_eq!(q.engine, None);
        assert_eq!(q.user_base, "/data/g");
        assert_eq!(GamePaths::config_file(&env(&[("HOME", "/h")])), "/h/.config/xonotic-touch/launcher.conf");
    }
}
