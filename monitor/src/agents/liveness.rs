//! Which agent sessions are still open (agent processes, not logs), so closed ones leave the display right away instead of
//! lingering until the activity timeout.
//!
//! - Claude Code registers every running session in `~/.claude/sessions/<pid>.json` (session id,
//!   pid, process start time). A session is open while that pid is alive with the same start time.
//! - Codex, pi and omp: a session is open while a process of that agent has its log open, or runs
//!   in the session's working directory. opencode (sessions in a database): a process running in
//!   the session's working directory.
//!
//! When nothing can be said (no registry, `/proc` unreadable) the answer is "unknown" and the
//! time-based rules apply as before.

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Default)]
pub struct Live {
    /// Claude sessions with a live process; `None` when this Claude Code keeps no registry.
    claude: Option<HashSet<String>>,
    /// Session logs a codex / pi process has open.
    open_files: HashSet<PathBuf>,
    /// Working directories of running codex / pi processes.
    codex_cwds: HashSet<PathBuf>,
    pi_cwds: HashSet<PathBuf>,
    omp_cwds: HashSet<PathBuf>,
    opencode_cwds: HashSet<PathBuf>,
    scanned: bool,
}

/// Field 22 of /proc/<pid>/stat: start time in clock ticks since boot.
fn start_time(pid: u32) -> Option<String> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    // The command name (field 2) may contain spaces; fields after it are space separated.
    let rest = &stat[stat.rfind(')')? + 2..];
    rest.split(' ').nth(19).map(str::to_string)
}

impl Live {
    pub fn scan(claude_root: &Path) -> Self {
        let mut l = Live { scanned: fs::read_dir("/proc").is_ok(), ..Default::default() };
        let registry = claude_root.join("sessions");
        if let Ok(dir) = fs::read_dir(&registry) {
            let mut set = HashSet::new();
            for e in dir.flatten() {
                let p = e.path();
                if p.extension().is_none_or(|x| x != "json") {
                    continue;
                }
                let Ok(v) = fs::read_to_string(&p).map(|t| serde_json::from_str::<serde_json::Value>(&t).unwrap_or_default()) else {
                    continue;
                };
                let (Some(pid), Some(sid)) = (v["pid"].as_u64(), v["sessionId"].as_str()) else { continue };
                let alive = match (start_time(pid as u32), v["procStart"].as_str()) {
                    (Some(now), Some(then)) => now == then,
                    (Some(_), None) => true,
                    (None, _) => false,
                };
                if alive {
                    set.insert(sid.to_string());
                }
            }
            l.claude = Some(set);
        }
        for e in fs::read_dir("/proc").into_iter().flatten().flatten() {
            let name = e.file_name();
            let Some(pid) = name.to_str().and_then(|n| n.parse::<u32>().ok()) else { continue };
            let comm = fs::read_to_string(format!("/proc/{pid}/comm")).unwrap_or_default();
            let cwds = match comm.trim() {
                "codex" => &mut l.codex_cwds,
                "pi" => &mut l.pi_cwds,
                "omp" => &mut l.omp_cwds,
                "opencode" | ".opencode" => &mut l.opencode_cwds,
                _ => continue,
            };
            if let Ok(cwd) = fs::read_link(format!("/proc/{pid}/cwd")) {
                cwds.insert(cwd);
            }
            for fd in fs::read_dir(format!("/proc/{pid}/fd")).into_iter().flatten().flatten() {
                if let Ok(target) = fs::read_link(fd.path())
                    && target.extension().is_some_and(|x| x == "jsonl")
                {
                    l.open_files.insert(target);
                }
            }
        }
        l
    }

    /// `Some(true)` open, `Some(false)` closed, `None` unknown.
    pub fn open(&self, agent: &str, log: &Path, session_id: &str, cwd: &str) -> Option<bool> {
        if !self.scanned {
            return None;
        }
        match agent {
            "claude" => self.claude.as_ref().map(|s| s.contains(session_id)),
            "codex" | "pi" | "omp" => {
                let cwds = match agent {
                    "codex" => &self.codex_cwds,
                    "pi" => &self.pi_cwds,
                    _ => &self.omp_cwds,
                };
                Some(self.open_files.contains(log) || (!cwd.is_empty() && cwds.contains(Path::new(cwd))))
            }
            // opencode keeps no log open (a database): a process in the session's folder.
            "opencode" => Some(!cwd.is_empty() && self.opencode_cwds.contains(Path::new(cwd))),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claude_registry_and_processes() {
        let dir = std::env::temp_dir().join(format!("deskhud-live-{}", std::process::id()));
        fs::create_dir_all(dir.join("sessions")).unwrap();
        let me = std::process::id();
        let start = start_time(me).unwrap();
        fs::write(dir.join("sessions/1.json"), format!(r#"{{"pid":{me},"sessionId":"alive","procStart":"{start}"}}"#)).unwrap();
        fs::write(dir.join("sessions/2.json"), format!(r#"{{"pid":{me},"sessionId":"reused-pid","procStart":"1"}}"#)).unwrap();
        fs::write(dir.join("sessions/3.json"), r#"{"pid":4000000000,"sessionId":"gone"}"#).unwrap();
        let l = Live::scan(&dir);
        assert_eq!(l.open("claude", Path::new("x"), "alive", ""), Some(true));
        assert_eq!(l.open("claude", Path::new("x"), "reused-pid", ""), Some(false));
        assert_eq!(l.open("claude", Path::new("x"), "gone", ""), Some(false));
        // No codex / pi process runs in the test: their sessions are closed.
        assert_eq!(l.open("codex", Path::new("/nope.jsonl"), "c", "/tmp"), Some(false));
        assert_eq!(Live::scan(&dir.join("missing")).open("claude", Path::new("x"), "alive", ""), None);
        fs::remove_dir_all(dir).unwrap();
    }
}
