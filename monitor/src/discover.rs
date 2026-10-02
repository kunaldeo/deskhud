//! Finding displays on the LAN: mDNS `_deskhud._tcp`.

use std::time::{Duration, Instant};

use anyhow::Result;
use mdns_sd::{ServiceDaemon, ServiceEvent};
use serde::Serialize;

pub const SERVICE: &str = "_deskhud._tcp.local.";

#[derive(Clone, Debug, Serialize)]
pub struct Found {
    pub id: String,
    pub name: String,
    pub fw: String,
    /// `ip:port`
    pub addr: String,
    pub host: String,
}

/// Browse for up to `timeout`; returns early once `want` (a device id) is seen.
pub fn browse(timeout: Duration, want: Option<&str>) -> Result<Vec<Found>> {
    let mdns = ServiceDaemon::new()?;
    let rx = mdns.browse(SERVICE)?;
    let deadline = Instant::now() + timeout;
    let mut found: Vec<Found> = Vec::new();
    while let Some(left) = deadline.checked_duration_since(Instant::now()) {
        let Ok(ev) = rx.recv_timeout(left) else { break };
        let ServiceEvent::ServiceResolved(info) = ev else { continue };
        let Some(ip) = info.get_addresses_v4().into_iter().min() else { continue };
        let txt = |k: &str| info.get_property_val_str(k).unwrap_or_default().to_string();
        let id = txt("id");
        if id.is_empty() {
            continue;
        }
        let f = Found {
            name: txt("name"),
            fw: txt("fw"),
            addr: format!("{ip}:{}", info.get_port()),
            host: info.get_hostname().trim_end_matches('.').to_string(),
            id,
        };
        let done = want == Some(f.id.as_str());
        found.retain(|o| o.id != f.id);
        found.push(f);
        if done {
            break;
        }
    }
    let _ = mdns.shutdown();
    found.sort_by(|a, b| a.id.cmp(&b.id));
    Ok(found)
}
