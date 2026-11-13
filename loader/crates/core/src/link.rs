// SPDX-License-Identifier: GPL-3.0-or-later
//! Server addresses typed or linked by the player, and `godnotic://join/<host>:<port>`.
//! Discovery, passwords and joining are the engine's; this only validates what
//! comes from outside before it is handed to the engine.

use std::fmt;

pub const DEFAULT_PORT: u16 = 26000;
pub const SCHEME: &str = "godnotic";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerAddress {
    pub host: String,
    pub port: u16,
}

impl ServerAddress {
    pub fn is_ipv6(&self) -> bool {
        self.host.contains(':')
    }

    /// `1.2.3.4`, `1.2.3.4:26001`, `[fe80::1]:26000`, `::1`, `example.org:26000`.
    /// None for spaces, quotes, `;`, `\`, `$`, control characters (it goes into a
    /// console command) or a bad port.
    pub fn parse(text: &str) -> Option<Self> {
        let s = text.trim();
        if s.is_empty() || s.len() > 255 || s.chars().any(|c| c.is_whitespace() || c.is_control() || "\";\\$".contains(c)) {
            return None;
        }
        if let Some(rest) = s.strip_prefix('[') {
            let end = rest.find(']')?;
            let host = &rest[..end];
            let tail = &rest[end + 1..];
            let port = if tail.is_empty() { DEFAULT_PORT } else { tail.strip_prefix(':')?.parse().ok()? };
            return (!host.is_empty() && port != 0).then(|| Self { host: host.into(), port });
        }
        let colons = s.matches(':').count();
        if colons > 1 {
            return Some(Self { host: s.into(), port: DEFAULT_PORT });
        }
        let (host, port) = match s.split_once(':') {
            Some((h, p)) => (h, p.parse::<u16>().ok()?),
            None => (s, DEFAULT_PORT),
        };
        (!host.is_empty() && port != 0).then(|| Self { host: host.into(), port })
    }
}

impl fmt::Display for ServerAddress {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_ipv6() { write!(f, "[{}]:{}", self.host, self.port) } else { write!(f, "{}:{}", self.host, self.port) }
    }
}

/// `godnotic://join/<address>`.
pub fn join_link(a: &ServerAddress) -> String {
    format!("{SCHEME}://join/{a}")
}

/// A join link or a plain address; None when it is neither.
pub fn parse_join(text: &str) -> Option<ServerAddress> {
    let t = text.trim();
    let lower = t.to_ascii_lowercase();
    if lower.starts_with("godnotic:") {
        let rest = lower.strip_prefix("godnotic://join/").map(|_| &t["godnotic://join/".len()..])?;
        return ServerAddress::parse(rest.trim_end_matches('/'));
    }
    ServerAddress::parse(t)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn addresses_and_links() {
        assert_eq!(ServerAddress::parse("192.168.1.23").unwrap().to_string(), "192.168.1.23:26000");
        assert_eq!(ServerAddress::parse(" 10.0.0.1:26001 ").unwrap().port, 26001);
        assert_eq!(ServerAddress::parse("[fe80::1]:26002").unwrap().to_string(), "[fe80::1]:26002");
        assert_eq!(ServerAddress::parse("::1").unwrap().host, "::1");
        assert!(ServerAddress::parse("1.2.3.4; quit").is_none());
        assert!(ServerAddress::parse("1.2.3.4:70000").is_none());
        assert!(ServerAddress::parse("a$b").is_none());
        let a = ServerAddress::parse("100.101.1.2").unwrap();
        assert_eq!(join_link(&a), "godnotic://join/100.101.1.2:26000");
        assert_eq!(parse_join("godnotic://join/100.101.1.2:26001/"), ServerAddress::parse("100.101.1.2:26001"));
        assert_eq!(parse_join("GODNOTIC://join/1.2.3.4"), ServerAddress::parse("1.2.3.4"));
        assert!(parse_join("godnotic://other/1.2.3.4").is_none());
        assert!(parse_join("godnotic://join/1.2.3.4\"x").is_none());
    }
}
