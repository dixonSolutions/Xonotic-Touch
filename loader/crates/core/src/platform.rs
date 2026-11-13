// SPDX-License-Identifier: GPL-3.0-or-later
//! What this device is ("detect OS"): one binary deciding how to present itself.
//! Cheap checks only (environment, a few sysfs files): nothing here may slow
//! the first frame.

use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Os { Linux, Android, Other }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormFactor { Desktop, Phone }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Session { Wayland, X11, None }

#[derive(Clone, Debug, PartialEq)]
pub struct Platform {
    pub os: Os,
    pub form: FormFactor,
    pub session: Session,
    pub flatpak: bool,
    /// Running on battery, or a power-saver profile is active (None: unknown).
    pub saving_power: Option<bool>,
}

impl Platform {
    pub fn detect(env: &HashMap<String, String>, read: &dyn Fn(&str) -> Option<String>, list: &dyn Fn(&str) -> Vec<String>) -> Self {
        let os = if cfg!(target_os = "android") { Os::Android } else if cfg!(target_os = "linux") { Os::Linux } else { Os::Other };
        let has = |k: &str| env.get(k).is_some_and(|v| !v.is_empty());
        let session = if has("WAYLAND_DISPLAY") { Session::Wayland } else if has("DISPLAY") { Session::X11 } else { Session::None };
        // Phone shells (Phosh, Plasma Mobile, Lomiri on Ubuntu Touch) or an explicit override.
        let desk = env.get("XDG_CURRENT_DESKTOP").map(|s| s.to_ascii_lowercase()).unwrap_or_default();
        let phone_shell = ["phosh", "plasma-mobile", "lomiri", "sxmo"].iter().any(|s| desk.contains(s));
        let form = match env.get("GODNOTIC_FORM").map(String::as_str) {
            Some("phone") => FormFactor::Phone,
            Some("desktop") => FormFactor::Desktop,
            _ if os == Os::Android || phone_shell => FormFactor::Phone,
            _ => FormFactor::Desktop,
        };
        let flatpak = has("FLATPAK_ID") || read("/.flatpak-info").is_some();
        Self { os, form, session, flatpak, saving_power: saving_power(read, list) }
    }
}

/// Battery discharging, or the ACPI platform profile is low-power. None when neither is readable.
pub fn saving_power(read: &dyn Fn(&str) -> Option<String>, list: &dyn Fn(&str) -> Vec<String>) -> Option<bool> {
    let profile = read("/sys/firmware/acpi/platform_profile").map(|s| s.trim() == "low-power");
    let mut battery = None;
    for supply in list("/sys/class/power_supply") {
        let base = format!("/sys/class/power_supply/{supply}");
        if read(&format!("{base}/type")).is_some_and(|t| t.trim() == "Battery") {
            let discharging = read(&format!("{base}/status")).is_some_and(|s| s.trim() == "Discharging");
            battery = Some(battery.unwrap_or(false) || discharging);
        }
    }
    match (profile, battery) {
        (None, None) => None,
        (a, b) => Some(a.unwrap_or(false) || b.unwrap_or(false)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_session_form_and_power() {
        let env: HashMap<String, String> = [("WAYLAND_DISPLAY", "wayland-0"), ("XDG_CURRENT_DESKTOP", "Phosh:GNOME")]
            .iter().map(|(a, b)| (a.to_string(), b.to_string())).collect();
        let files: HashMap<&str, &str> = [("/sys/class/power_supply/BAT0/type", "Battery\n"), ("/sys/class/power_supply/BAT0/status", "Discharging\n"),
            ("/sys/class/power_supply/AC/type", "Mains\n")].into_iter().collect();
        let read = |p: &str| files.get(p).map(|s| s.to_string());
        let list = |_: &str| vec!["AC".to_string(), "BAT0".to_string()];
        let p = Platform::detect(&env, &read, &list);
        assert_eq!(p.session, Session::Wayland);
        assert_eq!(p.form, FormFactor::Phone);
        assert_eq!(p.saving_power, Some(true));
        assert_eq!(saving_power(&|_| None, &|_| vec![]), None);
        assert_eq!(saving_power(&|p| (p.ends_with("platform_profile")).then(|| "balanced".into()), &|_| vec![]), Some(false));
    }
}
