//! On-demand views for the display's popups: `fastfetch` (system info, grouped the way the user's
//! fastfetch config groups it, with its icons and logo) and a btop-style process table.

use std::collections::HashMap;
use std::fs;
use std::process::Command;
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use crate::agents::strip_ansi;

/// The terminal's 16 colours, for fastfetch's colour swatches (`\e[3Xm●`).
fn ansi_color(code: u32) -> Option<&'static str> {
    Some(match code {
        30 | 90 => "#6b7280",
        31 | 91 => "#ef4444",
        32 | 92 => "#22c55e",
        33 | 93 => "#eab308",
        34 | 94 => "#3b82f6",
        35 | 95 => "#a855f7",
        36 | 96 => "#06b6d4",
        37 | 97 | 38 | 39 => "#e5e7eb",
        _ => return None,
    })
}

/// Colours of the `●` swatches in a line, in order.
fn swatches(raw: &str) -> Vec<&'static str> {
    let mut out = Vec::new();
    let mut color = None;
    let mut rest = raw;
    while let Some(i) = rest.find(['\x1b', '●']) {
        if rest[i..].starts_with('●') {
            out.push(color.unwrap_or("#e5e7eb"));
            rest = &rest[i + '●'.len_utf8()..];
        } else {
            let seq = &rest[i + 1..];
            let end = seq.find('m').unwrap_or(0);
            color = seq.get(1..end).and_then(|c| c.split(';').next_back()).and_then(|c| c.parse().ok()).and_then(ansi_color);
            rest = &seq[end.min(seq.len())..];
        }
    }
    out
}

/// A label for rows that only have an icon: the usual meaning of fastfetch's Nerd Font icons.
fn icon_label(icon: &str) -> &'static str {
    match icon.chars().next().map(|c| c as u32).unwrap_or(0) {
        0xf4bc | 0xf2db | 0xf0ee0 => "CPU",
        0xe266 | 0xf08ae => "GPU",
        0xf1104 | 0xf108 | 0xf0379 | 0xf1ab => "Display",
        0xf02ca | 0xf0a0 | 0xf0249 => "Disk",
        0xefc5 | 0xf035b | 0xf85a => "Memory",
        0xf04e1 => "Swap",
        0xf052b => "Channel",
        0xf013 | 0xf17c | 0xe712 => "Kernel",
        0xf488 | 0xf2d0 => "WM",
        0xf489 | 0xe795 | 0xf120 => "Terminal",
        0xf03d6 | 0xf187 => "Packages",
        0xf0e0c | 0xf1fc | 0xf03d8 => "Theme",
        0xf109 => "Host",
        0xf199f => "OS Age",
        0xf1ad0 | 0xf017 | 0xf43a => "Uptime",
        0xf0150 | 0xe23c => "Shell",
        0xf031 | 0xe659 => "Font",
        0xf007 => "User",
        0xf1eb | 0xf0928 => "Network",
        0xf242 | 0xf0079 => "Battery",
        0xf028 | 0xf057e => "Sound",
        _ => "",
    }
}

fn is_icon(c: char) -> bool {
    let c = c as u32;
    (0xe000..=0xf8ff).contains(&c) || c >= 0xf0000
}

/// fastfetch's output as sections of rows (icon, key, value), plus its logo.
pub fn fastfetch() -> Value {
    let run = |args: &[&str]| {
        Command::new("fastfetch").args(args).env("COLUMNS", "200").output().ok().map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
    };
    let Some(out) = run(&["--logo", "none", "--pipe", "false"]) else {
        return json!({"error":"fastfetch is not installed"});
    };
    let mut sections: Vec<Value> = Vec::new();
    let mut rows: Vec<Value> = Vec::new();
    let mut title = String::new();
    let flush = |title: &mut String, rows: &mut Vec<Value>, sections: &mut Vec<Value>| {
        if !rows.is_empty() {
            sections.push(json!({"title": std::mem::take(title), "rows": std::mem::take(rows)}));
        }
    };
    for raw in out.lines() {
        let line = strip_ansi(raw);
        let t = line.trim();
        if t.is_empty() {
            continue;
        }
        // Box borders: "┌───Title───┐" opens a section, "└───┘" closes it.
        if t.starts_with('┌') || t.starts_with('╭') {
            flush(&mut title, &mut rows, &mut sections);
            title = t.trim_matches(|c: char| "┌┐╭╮─ ".contains(c)).to_string();
            continue;
        }
        if t.chars().all(|c| "└┘╰╯─ ".contains(c)) {
            flush(&mut title, &mut rows, &mut sections);
            continue;
        }
        // "│ ├<icon>: value", "<icon> Key: value", "Key: value"
        let t = t.trim_start_matches(|c: char| "│├└┃┣┗ ".contains(c));
        let (head, value) = match t.split_once(": ") {
            Some((h, v)) => (h.trim(), v.trim()),
            None => ("", t),
        };
        let mut icon: String = head.chars().filter(|c| is_icon(*c)).collect();
        // Omarchy's own font puts its logo at U+E900, a COBOL mark in standard Nerd Fonts: show
        // the Arch logo it builds on instead.
        if icon == "\u{e900}" {
            icon = "\u{f303}".into();
        }
        let mut key = head.chars().filter(|c| !is_icon(*c)).collect::<String>().trim().to_string();
        if key.is_empty() {
            key = icon_label(&icon).to_string();
        }
        let colors = swatches(raw);
        let value = value.replace('●', "").trim().to_string();
        rows.push(json!({"icon": icon, "key": key, "value": value, "colors": colors}));
    }
    flush(&mut title, &mut rows, &mut sections);
    // The logo: block art, trimmed to its bounding box.
    let logo: Vec<String> = run(&["-s", "none", "--pipe", "true"])
        .map(|l| l.lines().map(|x| strip_ansi(x).trim_end().to_string()).filter(|x| !x.trim().is_empty()).collect())
        .unwrap_or_default();
    let indent = logo.iter().map(|l| l.len() - l.trim_start().len()).min().unwrap_or(0);
    let logo: Vec<String> = logo.iter().map(|l| l.chars().skip(indent).collect()).collect();
    json!({"sections": sections, "logo": logo})
}

// ---------------------------------------------------------------- processes

/// btop's process box, sampled on request: CPU since the previous request.
pub struct ProcTop {
    prev: HashMap<u32, u64>,
    prev_total: u64,
    at: Option<Instant>,
    users: HashMap<u32, String>,
    page: u64,
}

impl Default for ProcTop {
    fn default() -> Self {
        Self::new()
    }
}

fn total_jiffies() -> u64 {
    fs::read_to_string("/proc/stat")
        .ok()
        .and_then(|s| s.lines().next().map(|l| l.split_whitespace().skip(1).filter_map(|n| n.parse::<u64>().ok()).sum()))
        .unwrap_or(0)
}

/// Command line for a screen: argv[0] as its file name, arguments after.
fn cmdline(pid: u32) -> String {
    let raw = fs::read(format!("/proc/{pid}/cmdline")).unwrap_or_default();
    let mut args = raw.split(|b| *b == 0).filter(|a| !a.is_empty()).map(|a| String::from_utf8_lossy(a).into_owned());
    let Some(first) = args.next() else { return String::new() };
    let first = first.rsplit('/').next().unwrap_or(&first).to_string();
    let mut s = std::iter::once(first).chain(args).collect::<Vec<_>>().join(" ");
    if s.len() > 160 {
        let mut cut = 160;
        while !s.is_char_boundary(cut) {
            cut -= 1;
        }
        s.truncate(cut);
    }
    s
}

impl ProcTop {
    pub fn new() -> Self {
        let users = fs::read_to_string("/etc/passwd")
            .unwrap_or_default()
            .lines()
            .filter_map(|l| {
                let f: Vec<&str> = l.split(':').collect();
                Some((f.get(2)?.parse().ok()?, f.first()?.to_string()))
            })
            .collect();
        ProcTop { prev: HashMap::new(), prev_total: 0, at: None, users, page: rustix::param::page_size() as u64 }
    }

    fn read(&self) -> (HashMap<u32, (String, u64, u32, u64, u32)>, u64) {
        let mut now = HashMap::new();
        for e in fs::read_dir("/proc").into_iter().flatten().flatten() {
            let Some(pid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
            let Ok(stat) = fs::read_to_string(e.path().join("stat")) else { continue };
            let (Some(l), Some(r)) = (stat.find('('), stat.rfind(')')) else { continue };
            let f: Vec<&str> = stat[r + 1..].split_whitespace().collect();
            if f.len() < 22 {
                continue;
            }
            let ticks = f[11].parse::<u64>().unwrap_or(0) + f[12].parse::<u64>().unwrap_or(0);
            let uid = fs::read_to_string(e.path().join("status"))
                .ok()
                .and_then(|s| s.lines().find(|l| l.starts_with("Uid:")).and_then(|l| l.split_whitespace().nth(1)?.parse().ok()))
                .unwrap_or(0);
            now.insert(pid, (stat[l + 1..r].to_string(), ticks, f[17].parse().unwrap_or(0), f[21].parse::<u64>().unwrap_or(0) * self.page, uid));
        }
        (now, total_jiffies())
    }

    /// The top `n` processes by CPU (then memory), and how many there are.
    pub fn sample(&mut self, n: usize) -> Value {
        // First call (or a stale baseline): measure over a short interval.
        if self.at.is_none_or(|t| t.elapsed() > Duration::from_secs(30)) {
            let (now, total) = self.read();
            self.prev = now.iter().map(|(p, v)| (*p, v.1)).collect();
            self.prev_total = total;
            std::thread::sleep(Duration::from_millis(400));
        }
        let (now, total) = self.read();
        let dt = total.saturating_sub(self.prev_total).max(1) as f64;
        let mut all: Vec<(u32, f64, &(String, u64, u32, u64, u32))> = now
            .iter()
            .map(|(pid, v)| {
                let d = self.prev.get(pid).map(|p| v.1.saturating_sub(*p)).unwrap_or(0);
                (*pid, 100.0 * d as f64 / dt, v)
            })
            .filter(|(_, cpu, v)| *cpu > 0.0 || v.3 > 0)
            .collect();
        all.sort_by(|a, b| b.1.total_cmp(&a.1).then(b.2.3.cmp(&a.2.3)));
        let count = now.len();
        let procs: Vec<Value> = all
            .iter()
            .take(n)
            .map(|(pid, cpu, (name, _, threads, mem, uid))| {
                json!({
                    "pid": pid, "name": name, "cmd": cmdline(*pid), "threads": threads, "mem": mem,
                    "cpu": (cpu * 10.0).round() / 10.0,
                    "user": self.users.get(uid).cloned().unwrap_or_else(|| uid.to_string()),
                })
            })
            .collect();
        self.prev = now.iter().map(|(p, v)| (*p, v.1)).collect();
        self.prev_total = total;
        self.at = Some(Instant::now());
        json!({"count": count, "procs": procs})
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn swatch_colors() {
        assert_eq!(swatches("Tokyoled \x1b[31m●\x1b[32m●"), vec!["#ef4444", "#22c55e"]);
    }
}
