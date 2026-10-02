//! `~/.config/deskhud/config.toml` (user settings) and `~/.local/state/deskhud/` (pairing tokens).

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Config {
    /// Stable identity of this PC towards the display. Generated on first run.
    #[serde(default)]
    pub pc_id: String,
    /// Display to use: a device id (MAC without colons, as `deskhud devices` lists) or a static
    /// `host[:port]`. Empty: the only display found on the network.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub device: String,
    /// Name shown on the display. Empty: the hostname.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub host_name: String,
}

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| "/".into())
}

pub fn config_dir() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".config")).join("deskhud")
}

pub fn state_dir() -> PathBuf {
    std::env::var_os("XDG_STATE_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".local/state")).join("deskhud")
}

pub fn socket_path() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from).unwrap_or_else(std::env::temp_dir).join("deskhud.sock")
}

impl Config {
    pub fn path() -> PathBuf {
        config_dir().join("config.toml")
    }

    /// Load the config, creating it (with a fresh `pc_id`) when missing.
    pub fn load() -> Result<Self> {
        let path = Self::path();
        let mut cfg: Config = match fs::read_to_string(&path) {
            Ok(text) => toml::from_str(&text).with_context(|| format!("parsing {}", path.display()))?,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Config::default(),
            Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
        };
        if cfg.pc_id.is_empty() {
            cfg.pc_id = uuid::Uuid::new_v4().to_string();
            cfg.save()?;
        }
        Ok(cfg)
    }

    pub fn save(&self) -> Result<()> {
        write_atomic(&Self::path(), toml::to_string_pretty(self)?.as_bytes())
    }

    pub fn host_name(&self) -> String {
        if !self.host_name.is_empty() {
            return self.host_name.clone();
        }
        fs::read_to_string("/proc/sys/kernel/hostname").map(|h| h.trim().to_string()).unwrap_or_else(|_| "pc".into())
    }
}

/// Pairing tokens by device id, and the last address each device was reached at.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct State {
    #[serde(default)]
    pub tokens: BTreeMap<String, String>,
    #[serde(default)]
    pub last_addr: BTreeMap<String, String>,
}

impl State {
    pub fn load(dir: &Path) -> State {
        fs::read_to_string(dir.join("state.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
    }

    pub fn save(&self, dir: &Path) -> Result<()> {
        write_atomic(&dir.join("state.json"), serde_json::to_string_pretty(self)?.as_bytes())?;
        // Tokens authorize firmware uploads: keep them private.
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = fs::set_permissions(dir.join("state.json"), fs::Permissions::from_mode(0o600));
        }
        Ok(())
    }
}

pub fn write_atomic(path: &Path, data: &[u8]) -> Result<()> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    }
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, data).with_context(|| format!("writing {}", tmp.display()))?;
    fs::rename(&tmp, path).with_context(|| format!("replacing {}", path.display()))?;
    Ok(())
}

/// Is `device` a static address rather than a device id?
pub fn is_static_host(device: &str) -> bool {
    device.contains('.') || device.contains(':')
}
