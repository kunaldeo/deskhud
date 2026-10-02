//! `deskhud ota`: upload a firmware image over HTTP and wait for the display to come back on it.

use std::io::Write;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use serde_json::json;

use crate::config::{self, Config, State, is_static_host};
use crate::{daemon, discover, http};

/// What an ESP-IDF app image says about itself (`esp_app_desc_t`).
#[derive(Debug, PartialEq)]
pub struct AppDesc {
    pub version: String,
    pub project: String,
    pub idf: String,
    pub built: String,
}

fn cstr(b: &[u8]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..end]).into_owned()
}

/// Validate an app image and read its description (first segment, right after the headers).
pub fn app_desc(img: &[u8]) -> Result<AppDesc> {
    if img.len() < 256 || img[0] != 0xE9 {
        bail!("not an ESP32 app image (bad magic); use build/<project>.bin, not the merged flash image");
    }
    let magic = u32::from_le_bytes(img[32..36].try_into()?);
    if magic != 0xABCD_5432 {
        bail!("ESP image without an app description: is this a bootloader or partition table?");
    }
    Ok(AppDesc {
        version: cstr(&img[48..80]),
        project: cstr(&img[80..112]),
        built: format!("{} {}", cstr(&img[128..144]), cstr(&img[112..128])),
        idf: cstr(&img[144..176]),
    })
}

/// Address and id of the display to update.
fn target(host: Option<&str>) -> Result<(String, String)> {
    if let Some(h) = host {
        let addr = http::with_port(h);
        let info = http::get_json(&addr, "/api/info", None)?;
        return Ok((addr, info["id"].as_str().unwrap_or_default().to_string()));
    }
    if let Ok(v) = daemon::request(&json!({"op":"status"}))
        && let (Some(addr), Some(id)) = (v["status"]["addr"].as_str().filter(|a| !a.is_empty()), v["status"]["device_id"].as_str())
    {
        return Ok((addr.to_string(), id.to_string()));
    }
    let cfg = Config::load()?;
    if is_static_host(&cfg.device) {
        return target(Some(&cfg.device));
    }
    let want = (!cfg.device.is_empty()).then_some(cfg.device.as_str());
    let found = discover::browse(Duration::from_secs(4), want)?;
    let f = match want {
        Some(id) => found.into_iter().find(|f| f.id == id),
        None if found.len() == 1 => found.into_iter().next(),
        None if found.is_empty() => None,
        None => bail!("several displays found: pick one with --host or `deskhud use`"),
    };
    let f = f.ok_or_else(|| anyhow!("no display found; pass --host"))?;
    Ok((f.addr, f.id))
}

pub fn run(file: &std::path::Path, host: Option<&str>) -> Result<()> {
    let img = std::fs::read(file).with_context(|| format!("reading {}", file.display()))?;
    let desc = app_desc(&img)?;
    let (addr, id) = target(host)?;
    let token =
        State::load(&config::state_dir()).tokens.get(&id).cloned().ok_or_else(|| {
            anyhow!("this PC is not paired with display {id}: start the daemon and accept the pairing on the display first")
        })?;
    let info = http::get_json(&addr, "/api/info", None)?;
    let old = info["fw"].as_str().unwrap_or("?").to_string();
    eprintln!("display {} ({id}) at {addr}: firmware {old}", info["name"].as_str().unwrap_or("DeskHUD"));
    eprintln!(
        "image {}: {} {} (IDF {}, built {}), {} KiB",
        file.display(),
        desc.project,
        desc.version,
        desc.idf,
        desc.built,
        img.len() / 1024
    );
    if desc.version == old {
        eprintln!("note: the display already runs {old}; uploading anyway");
    }

    let started = Instant::now();
    let mut last_pct = u32::MAX;
    let r = http::request(
        &addr,
        "POST",
        "/api/ota",
        Some(&token),
        Some((&img, "application/octet-stream")),
        Duration::from_secs(60),
        |sent, total| {
            let pct = (sent as u64 * 100 / total.max(1) as u64) as u32;
            if pct != last_pct {
                last_pct = pct;
                let bar = "█".repeat((pct / 4) as usize) + &"░".repeat(25 - (pct / 4) as usize);
                let rate = sent as f64 / 1024.0 / started.elapsed().as_secs_f64().max(0.001);
                eprint!("\r  {bar} {pct:3}%  {:.0} KiB/s ", rate);
                let _ = std::io::stderr().flush();
            }
        },
    )?;
    eprintln!();
    if r.status != 200 {
        bail!("display rejected the image: HTTP {} {}", r.status, r.text());
    }
    eprintln!("uploaded in {:.1}s; display is rebooting into the new firmware…", started.elapsed().as_secs_f64());

    // Wait for it to go away and come back.
    let deadline = Instant::now() + Duration::from_secs(90);
    std::thread::sleep(Duration::from_secs(3));
    while Instant::now() < deadline {
        if let Ok(v) = http::get_json(&addr, "/api/info", None)
            && v["uptime"].as_u64().is_some_and(|u| u < 120)
        {
            let fw = v["fw"].as_str().unwrap_or("?");
            if fw == desc.version {
                eprintln!("display is back on firmware {fw}");
                return Ok(());
            }
            bail!("display came back on firmware {fw}, not {}: the new image was rolled back", desc.version);
        }
        std::thread::sleep(Duration::from_secs(1));
    }
    bail!("display did not come back within 90 s")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_app_desc() {
        let mut img = vec![0u8; 512];
        img[0] = 0xE9;
        img[32..36].copy_from_slice(&0xABCD_5432u32.to_le_bytes());
        img[48..53].copy_from_slice(b"0.2.0");
        img[80..87].copy_from_slice(b"deskhud");
        img[112..120].copy_from_slice(b"12:00:00");
        img[128..139].copy_from_slice(b"Oct  1 2026");
        img[144..150].copy_from_slice(b"v5.5.5");
        let d = app_desc(&img).unwrap();
        assert_eq!(
            d,
            AppDesc { version: "0.2.0".into(), project: "deskhud".into(), idf: "v5.5.5".into(), built: "Oct  1 2026 12:00:00".into() }
        );
        img[0] = 0;
        assert!(app_desc(&img).is_err());
    }
}
