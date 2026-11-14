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
        format!("{base}/godnotic/launcher.conf")
    }

    /// An install from before the rename kept its data and settings in folders
    /// with the old name. Take them over once, before anything creates the new
    /// ones: a rename on the same filesystem, so nothing is downloaded again.
    pub fn adopt_old_name(env: &HashMap<String, String>) {
        const OLD_NAME: &str = "xonotic-touch";
        let get = |k: &str| env.get(k).filter(|s| !s.is_empty()).cloned();
        let Some(home) = get("HOME") else { return };
        let data = get("XDG_DATA_HOME").filter(|_| get("FLATPAK_ID").is_some()).unwrap_or_else(|| format!("{home}/.local/share"));
        let config = get("XDG_CONFIG_HOME").unwrap_or_else(|| format!("{home}/.config"));
        for base in [data, config] {
            let (old, new) = (format!("{base}/{OLD_NAME}"), format!("{base}/godnotic"));
            if std::path::Path::new(&old).is_dir() && !std::path::Path::new(&new).exists() {
                let _ = std::fs::rename(&old, &new);
            }
        }
    }

    /// `exists` is injected so this stays testable.
    pub fn resolve(env: &HashMap<String, String>, exe_dir: Option<&str>, exists: &dyn Fn(&str) -> bool, user_base_override: &str) -> Self {
        let get = |k: &str| env.get(k).filter(|s| !s.is_empty()).cloned();
        let home = get("HOME").unwrap_or_else(|| "/tmp".into());
        let flatpak = get("FLATPAK_ID").is_some();
        let mut roots: Vec<String> = Vec::new();
        if let Some(r) = get("GODNOTIC_APP_ROOT") { roots.push(r); }
        if flatpak { roots.push("/app".into()); }
        if let Some(d) = exe_dir { roots.push(parent(d)); }
        roots.push("/app".into());
        let app_root = roots.into_iter().find(|r| exists(&format!("{r}/bin/start.sh")) || exists(&format!("{r}/bin/xonotic")));
        let start = get("GODNOTIC_START").filter(|s| exists(s))
            .or_else(|| app_root.as_ref().map(|r| format!("{r}/bin/start.sh")).filter(|s| exists(s)));
        let engine = app_root.as_ref().map(|r| format!("{r}/bin/xonotic")).filter(|s| exists(s));
        let user_base = if !user_base_override.trim().is_empty() {
            user_base_override.trim().trim_end_matches('/').to_string()
        } else if let Some(b) = get("GODNOTIC_USER_BASE") {
            b
        } else if let (true, Some(x)) = (flatpak, get("XDG_DATA_HOME")) {
            format!("{x}/godnotic")
        } else {
            format!("{home}/.local/share/godnotic")
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
        assert_eq!(p.user_base, "/home/p/.var/app/x/data/godnotic");
        assert_eq!(p.user_data(), "/home/p/.var/app/x/data/godnotic/data");
        assert_eq!(p.data_dirs(), vec!["/home/p/.xonotic/data", "/home/p/.var/app/x/data/godnotic/data", "/app/data"]);
        let q = GamePaths::resolve(&env(&[("HOME", "/h")]), Some("/opt/g/bin"), &|f| f == "/opt/g/bin/start.sh", "/data/g/");
        assert_eq!(q.app_root.as_deref(), Some("/opt/g"));
        assert_eq!(q.engine, None);
        assert_eq!(q.user_base, "/data/g");
        assert_eq!(GamePaths::config_file(&env(&[("HOME", "/h")])), "/h/.config/godnotic/launcher.conf");
    }

    #[test]
    fn old_name_folders_are_taken_over_once() {
        const OLD_NAME: &str = "xonotic-touch";
        let tmp = std::env::temp_dir().join(format!("godnotic-adopt-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        let home = tmp.to_str().unwrap().to_string();
        std::fs::create_dir_all(tmp.join(".local/share").join(OLD_NAME).join("data")).unwrap();
        std::fs::write(tmp.join(".local/share").join(OLD_NAME).join("data/pack.pk3"), b"x").unwrap();
        std::fs::create_dir_all(tmp.join(".config").join(OLD_NAME)).unwrap();
        std::fs::create_dir_all(tmp.join(".config/godnotic")).unwrap();
        GamePaths::adopt_old_name(&env(&[("HOME", &home)]));
        assert!(tmp.join(".local/share/godnotic/data/pack.pk3").is_file());
        assert!(!tmp.join(".local/share").join(OLD_NAME).exists());
        // A new folder that is already there is never replaced.
        assert!(tmp.join(".config").join(OLD_NAME).is_dir());
        // Flatpak: the data folder is the sandbox's own.
        std::fs::create_dir_all(tmp.join("var/data").join(OLD_NAME)).unwrap();
        let xdg = tmp.join("var/data").to_str().unwrap().to_string();
        GamePaths::adopt_old_name(&env(&[("HOME", &home), ("FLATPAK_ID", "x"), ("XDG_DATA_HOME", &xdg)]));
        assert!(tmp.join("var/data/godnotic").is_dir());
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
