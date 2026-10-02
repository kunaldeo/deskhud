//! `deskhud install` / `uninstall`: the systemd user unit and the Claude Code hooks.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail};
use serde_json::{Map, Value, json};

use crate::agents::home;
use crate::config::write_atomic;

/// Hook events that drive session state.
pub const HOOK_EVENTS: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionRequest",
    "Notification",
    "Stop",
    "StopFailure",
];

const MARK: &str = "deskhud hook";

fn unit_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".config")).join("systemd/user/deskhud.service")
}

pub fn claude_settings_path() -> PathBuf {
    std::env::var_os("CLAUDE_CONFIG_DIR").map(PathBuf::from).unwrap_or_else(|| home().join(".claude")).join("settings.json")
}

fn exe() -> Result<String> {
    let p = std::env::current_exe()?.canonicalize()?;
    let s = p.to_string_lossy().into_owned();
    Ok(if s.contains(' ') { format!("'{s}'") } else { s })
}

pub fn unit_text(exe: &str) -> String {
    format!(
        "[Unit]\nDescription=DeskHUD monitoring service (PC stats and coding-agent activity)\nAfter=network-online.target\nWants=network-online.target\n\n\
         [Service]\nExecStart={exe} daemon\nRestart=always\nRestartSec=3\nEnvironment=RUST_LOG=info\n\n[Install]\nWantedBy=default.target\n"
    )
}

/// Add `deskhud hook` to every event in `HOOK_EVENTS`, leaving everything else as it was.
/// Returns how many events were added (0: already installed).
pub fn merge_hooks(settings: &mut Value, command: &str) -> Result<usize> {
    if !settings.is_object() {
        bail!("settings.json is not a JSON object");
    }
    let hooks = settings.as_object_mut().unwrap().entry("hooks").or_insert_with(|| Value::Object(Map::new()));
    let Some(hooks) = hooks.as_object_mut() else { bail!("\"hooks\" in settings.json is not an object") };
    let mut added = 0;
    for ev in HOOK_EVENTS {
        let list = hooks.entry(*ev).or_insert_with(|| json!([]));
        let Some(list) = list.as_array_mut() else { bail!("hooks.{ev} is not an array") };
        let present = list
            .iter()
            .any(|m| m["hooks"].as_array().into_iter().flatten().any(|h| h["command"].as_str().is_some_and(|c| c.contains(MARK))));
        if !present {
            list.push(json!({"hooks":[{"type":"command","command":command,"async":true,"timeout":5}]}));
            added += 1;
        }
    }
    Ok(added)
}

/// Remove every hook whose command is `deskhud hook`, and containers left empty by that.
pub fn strip_hooks(settings: &mut Value) -> usize {
    let mut removed = 0;
    let Some(hooks) = settings.get_mut("hooks").and_then(Value::as_object_mut) else { return 0 };
    for list in hooks.values_mut() {
        let Some(list) = list.as_array_mut() else { continue };
        for m in list.iter_mut() {
            if let Some(hs) = m.get_mut("hooks").and_then(Value::as_array_mut) {
                let before = hs.len();
                hs.retain(|h| !h["command"].as_str().is_some_and(|c| c.contains(MARK)));
                removed += before - hs.len();
            }
        }
        list.retain(|m| m["hooks"].as_array().is_none_or(|h| !h.is_empty()));
    }
    hooks.retain(|_, l| l.as_array().is_none_or(|a| !a.is_empty()));
    if hooks.is_empty() {
        settings.as_object_mut().unwrap().remove("hooks");
    }
    removed
}

fn load_settings(path: &Path) -> Result<Value> {
    match fs::read_to_string(path) {
        Ok(t) if t.trim().is_empty() => Ok(json!({})),
        Ok(t) => serde_json::from_str(&t).with_context(|| format!("parsing {}", path.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(json!({})),
        Err(e) => Err(e).with_context(|| format!("reading {}", path.display())),
    }
}

fn save_settings(path: &Path, v: &Value) -> Result<()> {
    if path.exists() {
        let stamp = jiff::Zoned::now().strftime("%Y%m%d-%H%M%S").to_string();
        let backup = path.with_file_name(format!("settings.json.deskhud-{stamp}.bak"));
        fs::copy(path, &backup).with_context(|| format!("backing up to {}", backup.display()))?;
        eprintln!("backed up {} to {}", path.display(), backup.display());
    }
    write_atomic(path, (serde_json::to_string_pretty(v)? + "\n").as_bytes())
}

fn systemctl(args: &[&str]) -> Result<()> {
    let st = Command::new("systemctl").arg("--user").args(args).status().context("running systemctl")?;
    if !st.success() {
        bail!("systemctl --user {} failed", args.join(" "));
    }
    Ok(())
}

pub fn install(hooks: bool, dry_run: bool) -> Result<()> {
    let exe = exe()?;
    let unit = unit_text(&exe);
    let hook_cmd = format!("{exe} hook");
    let path = unit_path();
    let settings_path = claude_settings_path();
    if dry_run {
        println!("# {}\n{unit}", path.display());
        let mut example = json!({});
        merge_hooks(&mut example, &hook_cmd)?;
        println!("# merged into {} (with --hooks)\n{}", settings_path.display(), serde_json::to_string_pretty(&example)?);
        return Ok(());
    }
    write_atomic(&path, unit.as_bytes())?;
    eprintln!("wrote {}", path.display());
    systemctl(&["daemon-reload"])?;
    systemctl(&["enable", "--now", "deskhud.service"])?;
    eprintln!("deskhud.service enabled and started");
    if hooks {
        let mut s = load_settings(&settings_path)?;
        let added = merge_hooks(&mut s, &hook_cmd)?;
        if added == 0 {
            eprintln!("Claude Code hooks already installed");
        } else {
            save_settings(&settings_path, &s)?;
            eprintln!("added deskhud to {added} Claude Code hook events in {} (new sessions pick them up)", settings_path.display());
        }
    } else if load_settings(&settings_path).is_ok_and(|s| s.to_string().contains(MARK)) {
        eprintln!("Claude Code hooks are installed (left unchanged)");
    } else {
        eprintln!("Claude Code hooks not installed: run `deskhud install --hooks` for exact permission / question states");
    }
    Ok(())
}

pub fn uninstall() -> Result<()> {
    let path = unit_path();
    if path.exists() {
        let _ = systemctl(&["disable", "--now", "deskhud.service"]);
        fs::remove_file(&path)?;
        let _ = systemctl(&["daemon-reload"]);
        eprintln!("removed {}", path.display());
    }
    let settings_path = claude_settings_path();
    let mut s = load_settings(&settings_path)?;
    let n = strip_hooks(&mut s);
    if n > 0 {
        save_settings(&settings_path, &s)?;
        eprintln!("removed {n} deskhud hooks from {}", settings_path.display());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn merge_keeps_existing_and_is_idempotent() {
        let mut s = json!({
            "attribution": {"commit": ""},
            "hooks": {"Stop": [{"hooks": [{"type":"command","command":"say done"}]}]}
        });
        assert_eq!(merge_hooks(&mut s, "/bin/deskhud hook").unwrap(), HOOK_EVENTS.len());
        assert_eq!(merge_hooks(&mut s, "/bin/deskhud hook").unwrap(), 0);
        assert_eq!(s["attribution"]["commit"], "");
        assert_eq!(s["hooks"]["Stop"].as_array().unwrap().len(), 2);
        assert_eq!(s["hooks"]["Stop"][0]["hooks"][0]["command"], "say done");
        assert_eq!(s["hooks"]["PermissionRequest"][0]["hooks"][0]["async"], true);
        // Key order of the user's file is preserved.
        assert_eq!(s.as_object().unwrap().keys().next().unwrap(), "attribution");

        assert_eq!(strip_hooks(&mut s), HOOK_EVENTS.len());
        assert_eq!(s, json!({"attribution": {"commit": ""}, "hooks": {"Stop": [{"hooks": [{"type":"command","command":"say done"}]}]}}));
        let mut t = json!({});
        merge_hooks(&mut t, "x/deskhud hook").unwrap();
        strip_hooks(&mut t);
        assert_eq!(t, json!({}));
    }
}
