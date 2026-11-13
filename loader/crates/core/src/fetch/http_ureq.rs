// SPDX-License-Identifier: GPL-3.0-or-later
//! [`Http`] over ureq (rustls, the platform's certificate store). Redirects are
//! followed; a cancel is noticed between reads, and a silent connection gives
//! up after the read timeout (a few seconds), so a stop returns promptly.

use super::{CancelToken, Error, Head, Http, Result};
use std::io::Read;
use std::time::Duration;

pub struct UreqHttp {
    agent: ureq::Agent,
}

impl Default for UreqHttp {
    fn default() -> Self {
        Self::new()
    }
}

impl UreqHttp {
    pub fn new() -> Self {
        let cfg = ureq::Agent::config_builder()
            .http_status_as_error(false)
            .timeout_connect(Some(Duration::from_secs(15)))
            .timeout_recv_response(Some(Duration::from_secs(30)))
            .user_agent("godnotic-loader")
            .build();
        Self { agent: cfg.into() }
    }
}

impl Http for UreqHttp {
    fn get(&self, url: &str, headers: &[(&str, String)], cancel: &CancelToken, head: &mut dyn FnMut(&Head) -> Result<()>, body: &mut dyn FnMut(&[u8]) -> Result<()>) -> Result<()> {
        cancel.check()?;
        let mut req = self.agent.get(url);
        for (k, v) in headers {
            req = req.header(*k, v);
        }
        let resp = req.call().map_err(|e| Error::Transient(format!("Network error: {e}")))?;
        let h = Head {
            code: resp.status().as_u16(),
            headers: resp.headers().iter().filter_map(|(k, v)| Some((k.as_str().to_ascii_lowercase(), v.to_str().ok()?.to_string()))).collect(),
        };
        head(&h)?;
        let mut reader = resp.into_body().into_reader();
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            cancel.check()?;
            let n = reader.read(&mut buf).map_err(|e| Error::Transient(format!("Network error: {e}")))?;
            if n == 0 {
                return Ok(());
            }
            body(&buf[..n])?;
        }
    }
}
