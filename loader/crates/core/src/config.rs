// SPDX-License-Identifier: GPL-3.0-or-later
//! `~/.config/xonotic-touch/launcher.conf`: `key = value` lines, the same file
//! and format the Kotlin launcher used, unknown keys kept.

use std::collections::BTreeMap;

#[derive(Default, Clone, Debug, PartialEq)]
pub struct Config {
    values: BTreeMap<String, String>,
    order: Vec<String>,
}

impl Config {
    pub fn parse(text: &str) -> Self {
        let mut c = Self::default();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = line.split_once('=') {
                let k = k.trim();
                if !k.is_empty() {
                    c.set(k, v.trim());
                }
            }
        }
        c
    }

    pub fn get(&self, k: &str) -> Option<&str> {
        self.values.get(k).map(String::as_str)
    }

    pub fn string(&self, k: &str, default: &str) -> String {
        self.get(k).unwrap_or(default).to_string()
    }

    pub fn bool(&self, k: &str, default: bool) -> bool {
        match self.get(k).map(str::to_ascii_lowercase).as_deref() {
            Some("1" | "true" | "yes" | "on") => true,
            Some("0" | "false" | "no" | "off") => false,
            _ => default,
        }
    }

    /// An empty value removes the key.
    pub fn set(&mut self, k: &str, v: &str) {
        if v.is_empty() {
            self.values.remove(k);
            self.order.retain(|x| x != k);
        } else {
            if !self.values.contains_key(k) {
                self.order.push(k.to_string());
            }
            self.values.insert(k.to_string(), v.replace('\n', " "));
        }
    }

    pub fn serialize(&self) -> String {
        let mut s = String::from("# Godnotic launcher settings\n");
        for k in &self.order {
            s.push_str(&format!("{k} = {}\n", self.values[k]));
        }
        s
    }
}

/// Keys shared with the Kotlin launcher (launcher/core Config.kt).
pub mod keys {
    pub const TOUCH_PROFILE: &str = "touch_profile";
    pub const PERF_PROFILE: &str = "perf_profile";
    pub const WINDOW_MODE: &str = "window_mode";
    pub const RESOLUTION: &str = "resolution";
    pub const EXTRA_ARGS: &str = "extra_args";
    pub const TOUCH_MODE: &str = "touch_mode";
    pub const USER_BASE: &str = "user_base";
    pub const SKIP_LAUNCHER: &str = "skip_launcher";
    pub const DATA_SET: &str = "data_set";
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_keeps_unknown_keys_and_order() {
        let c = Config::parse("# x\nb = 2\n a=1 \nbad line\nskip_launcher = yes\n");
        assert_eq!(c.get("a"), Some("1"));
        assert!(c.bool("skip_launcher", false));
        assert_eq!(c.serialize(), "# Godnotic launcher settings\nb = 2\na = 1\nskip_launcher = yes\n");
        let mut d = c.clone();
        d.set("a", "");
        d.set("new", "x\ny");
        assert_eq!(d.serialize(), "# Godnotic launcher settings\nb = 2\nskip_launcher = yes\nnew = x y\n");
    }
}
