//! System metrics from /proc, /sys, NVML (NVIDIA) and amdgpu sysfs. No subprocesses.
//!
//! Rates (CPU load, disk IO, network, per-process CPU) are deltas between two samples, so the
//! first sample after start reports them as zero.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::net::UdpSocket;
use std::path::{Path, PathBuf};
use std::time::Instant;

use nvml_wrapper::Nvml;
use nvml_wrapper::enum_wrappers::device::{Clock, TemperatureSensor};
use serde::Serialize;

#[derive(Clone, Debug, Default, Serialize)]
pub struct Stats {
    pub cpu: Cpu,
    pub mem: Mem,
    pub gpus: Vec<Gpu>,
    pub disks: Vec<Disk>,
    pub io: Io,
    pub net: Net,
    pub load: [f64; 3],
    pub uptime: u64,
    pub procs: Vec<Proc>,
    /// POSIX time zone of this PC (footer of /etc/localtime), so the display follows local time.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tz: Option<String>,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Cpu {
    pub name: String,
    pub load: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub temp: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mhz: Option<u32>,
    pub cores: Vec<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power: Option<f64>,
    /// Extra named sensors (AMD: one per CCD).
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub temps: Vec<NamedTemp>,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct NamedTemp {
    pub name: String,
    pub temp: f64,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Mem {
    pub used: u64,
    pub total: u64,
    /// MemAvailable: what programs can still get without swapping.
    pub available: u64,
    /// Page cache plus reclaimable slab (Cached + SReclaimable), like `free`'s buff/cache minus buffers.
    pub cached: u64,
    /// MemFree: completely unused.
    pub free: u64,
    pub swap_used: u64,
    pub swap_total: u64,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Gpu {
    pub name: String,
    pub load: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub temp: Option<f64>,
    pub vram_used: u64,
    pub vram_total: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power_max: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub fan: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mhz: Option<u32>,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Disk {
    pub mount: String,
    pub used: u64,
    pub total: u64,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Io {
    pub read: f64,
    pub write: f64,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Net {
    pub iface: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub ip: String,
    pub down: f64,
    pub up: f64,
    /// Bytes received / sent on `iface` since boot.
    pub rx_total: u64,
    pub tx_total: u64,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Proc {
    pub pid: u32,
    pub name: String,
    /// Percent of the whole machine (all cores).
    pub cpu: f64,
    /// Resident memory, bytes.
    pub mem: u64,
    pub threads: u32,
}

const CPU_SENSORS: &[&str] = &["k10temp", "coretemp", "zenpower", "cpu_thermal"];
const REAL_FS: &[&str] = &["ext4", "ext3", "xfs", "btrfs", "f2fs", "vfat", "exfat", "ntfs3", "ntfs", "zfs", "bcachefs", "fuseblk"];
const MAX_DISKS: usize = 6;
const TOP_PROCS: usize = 16;

fn read(path: impl AsRef<Path>) -> Option<String> {
    fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

fn read_num<T: std::str::FromStr>(path: impl AsRef<Path>) -> Option<T> {
    read(path)?.parse().ok()
}

enum GpuSource {
    Nvidia(Box<Nvml>),
    Amd(Vec<PathBuf>),
}

/// CPU time counters: (total, idle) jiffies per line of /proc/stat.
type CpuTimes = Vec<(u64, u64)>;

pub struct Sampler {
    cpu_name: String,
    cpu_temp: Option<PathBuf>,
    ccd_temps: Vec<(String, PathBuf)>,
    tz: Option<String>,
    rapl: Option<PathBuf>,
    gpus: Vec<GpuSource>,
    page: u64,
    prev_at: Option<Instant>,
    prev_cpu: CpuTimes,
    prev_energy: Option<u64>,
    prev_io: Option<(u64, u64)>,
    prev_net: Option<(String, u64, u64)>,
    prev_procs: HashMap<u32, u64>,
    prev_total: u64,
}

impl Default for Sampler {
    fn default() -> Self {
        Self::new()
    }
}

impl Sampler {
    pub fn new() -> Self {
        let cpu_name = read("/proc/cpuinfo")
            .and_then(|t| {
                t.lines().find(|l| l.starts_with("model name")).and_then(|l| l.split_once(':')).map(|(_, v)| v.trim().to_string())
            })
            .map(|n| tidy_cpu_name(&n))
            .unwrap_or_default();
        let rapl = ["/sys/class/powercap/intel-rapl:0/energy_uj", "/sys/class/powercap/amd-rapl:0/energy_uj"]
            .iter()
            .map(PathBuf::from)
            .find(|p| fs::read_to_string(p).is_ok());
        Sampler {
            cpu_name,
            cpu_temp: find_cpu_temp(),
            ccd_temps: find_ccd_temps(),
            tz: local_tz(),
            rapl,
            gpus: find_gpus(),
            page: rustix::param::page_size() as u64,
            prev_at: None,
            prev_cpu: Vec::new(),
            prev_energy: None,
            prev_io: None,
            prev_net: None,
            prev_procs: HashMap::new(),
            prev_total: 0,
        }
    }

    pub fn sample(&mut self) -> Stats {
        let now = Instant::now();
        let dt = self.prev_at.map(|t| now.duration_since(t).as_secs_f64()).unwrap_or(0.0);
        self.prev_at = Some(now);
        let mut s = Stats { cpu: self.cpu(dt), mem: mem(), gpus: self.gpu_stats(), disks: disks(), ..Default::default() };
        s.io = self.io(dt);
        s.net = self.net(dt);
        s.procs = self.procs();
        if let Some(t) = read("/proc/loadavg") {
            for (i, v) in t.split_whitespace().take(3).enumerate() {
                s.load[i] = v.parse().unwrap_or(0.0);
            }
        }
        s.tz = self.tz.clone();
        s.uptime = read("/proc/uptime").and_then(|t| t.split_whitespace().next()?.parse::<f64>().ok()).unwrap_or(0.0) as u64;
        s
    }

    fn cpu(&mut self, dt: f64) -> Cpu {
        let mut c = Cpu { name: self.cpu_name.clone(), ..Default::default() };
        let times: CpuTimes = read("/proc/stat")
            .unwrap_or_default()
            .lines()
            .take_while(|l| l.starts_with("cpu"))
            .map(|l| {
                let v: Vec<u64> = l.split_whitespace().skip(1).filter_map(|x| x.parse().ok()).collect();
                let total: u64 = v.iter().take(8).sum();
                let idle = v.get(3).copied().unwrap_or(0) + v.get(4).copied().unwrap_or(0);
                (total, idle)
            })
            .collect();
        let load = |(t, i): (u64, u64), (pt, pi): (u64, u64)| {
            let dt = t.saturating_sub(pt) as f64;
            let di = i.saturating_sub(pi) as f64;
            if dt > 0.0 { (100.0 * (dt - di) / dt).clamp(0.0, 100.0) } else { 0.0 }
        };
        if self.prev_cpu.len() == times.len() && !times.is_empty() {
            c.load = round1(load(times[0], self.prev_cpu[0]));
            c.cores = times[1..].iter().zip(&self.prev_cpu[1..]).map(|(a, b)| round1(load(*a, *b))).collect();
        }
        self.prev_total = times.first().map(|t| t.0).unwrap_or(0).saturating_sub(self.prev_cpu.first().map(|t| t.0).unwrap_or(0));
        self.prev_cpu = times;
        c.temp = self.cpu_temp.as_ref().and_then(read_num::<f64>).map(|m| round1(m / 1000.0));
        c.temps = self
            .ccd_temps
            .iter()
            .filter_map(|(name, p)| read_num::<f64>(p).map(|m| NamedTemp { name: name.clone(), temp: round1(m / 1000.0) }))
            .collect();
        c.mhz = fs::read_dir("/sys/devices/system/cpu")
            .ok()
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|e| read_num::<u32>(e.path().join("cpufreq/scaling_cur_freq")))
            .max()
            .map(|khz| khz / 1000);
        if let Some(p) = &self.rapl
            && let Some(e) = read_num::<u64>(p)
        {
            if let Some(prev) = self.prev_energy
                && dt > 0.0
                && e >= prev
            {
                c.power = Some(round1((e - prev) as f64 / 1e6 / dt));
            }
            self.prev_energy = Some(e);
        }
        c
    }

    fn gpu_stats(&self) -> Vec<Gpu> {
        let mut out = Vec::new();
        for src in &self.gpus {
            match src {
                GpuSource::Nvidia(nvml) => {
                    for i in 0..nvml.device_count().unwrap_or(0) {
                        let Ok(d) = nvml.device_by_index(i) else { continue };
                        let mem = d.memory_info().ok();
                        out.push(Gpu {
                            name: d.name().unwrap_or_default().replace("NVIDIA ", "").replace("GeForce ", ""),
                            load: d.utilization_rates().map(|u| u.gpu as f64).unwrap_or(0.0),
                            temp: d.temperature(TemperatureSensor::Gpu).ok().map(|t| t as f64),
                            vram_used: mem.as_ref().map(|m| m.used).unwrap_or(0),
                            vram_total: mem.as_ref().map(|m| m.total).unwrap_or(0),
                            power: d.power_usage().ok().map(|mw| round1(mw as f64 / 1000.0)),
                            power_max: d.enforced_power_limit().ok().map(|mw| round1(mw as f64 / 1000.0)),
                            fan: d.fan_speed(0).ok().map(|f| f as f64),
                            mhz: d.clock_info(Clock::Graphics).ok(),
                        });
                    }
                }
                GpuSource::Amd(cards) => {
                    for card in cards {
                        out.push(amd_gpu(card));
                    }
                }
            }
        }
        out
    }

    fn io(&mut self, dt: f64) -> Io {
        let (mut r, mut w) = (0u64, 0u64);
        for line in read("/proc/diskstats").unwrap_or_default().lines() {
            let f: Vec<&str> = line.split_whitespace().collect();
            // Whole physical disks only: partitions and virtual devices would double count.
            if f.len() < 10 || !Path::new("/sys/block").join(f[2]).join("device").exists() {
                continue;
            }
            r += f[5].parse::<u64>().unwrap_or(0) * 512;
            w += f[9].parse::<u64>().unwrap_or(0) * 512;
        }
        let mut io = Io::default();
        if let Some((pr, pw)) = self.prev_io
            && dt > 0.0
        {
            io.read = (r.saturating_sub(pr) as f64 / dt).round();
            io.write = (w.saturating_sub(pw) as f64 / dt).round();
        }
        self.prev_io = Some((r, w));
        io
    }

    fn net(&mut self, dt: f64) -> Net {
        let iface = default_iface().unwrap_or_default();
        let mut n = Net { iface: iface.clone(), ip: local_ip().unwrap_or_default(), ..Default::default() };
        let counters = read("/proc/net/dev").unwrap_or_default().lines().skip(2).find_map(|line| {
            let (name, rest) = line.split_once(':')?;
            if name.trim() != iface {
                return None;
            }
            let cols: Vec<u64> = rest.split_whitespace().filter_map(|c| c.parse().ok()).collect();
            (cols.len() > 8).then(|| (cols[0], cols[8]))
        });
        if let Some((rx, tx)) = counters {
            (n.rx_total, n.tx_total) = (rx, tx);
            if let Some((pi, prx, ptx)) = &self.prev_net
                && *pi == iface
                && dt > 0.0
            {
                n.down = (rx.saturating_sub(*prx) as f64 / dt).round();
                n.up = (tx.saturating_sub(*ptx) as f64 / dt).round();
            }
            self.prev_net = Some((iface, rx, tx));
        }
        n
    }

    /// Top processes by CPU since the previous sample (then by memory), one entry per process.
    fn procs(&mut self) -> Vec<Proc> {
        let mut now = HashMap::new();
        let mut all: Vec<(Proc, u64)> = Vec::new();
        for e in fs::read_dir("/proc").into_iter().flatten().flatten() {
            let Some(pid) = e.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else { continue };
            let Some(stat) = read(e.path().join("stat")) else { continue };
            let Some((name, ticks, threads, rss_pages)) = parse_stat(&stat) else { continue };
            now.insert(pid, ticks);
            let delta = self.prev_procs.get(&pid).map(|p| ticks.saturating_sub(*p)).unwrap_or(0);
            all.push((Proc { pid, name, cpu: 0.0, mem: rss_pages * self.page, threads }, delta));
        }
        self.prev_procs = now;
        let total = self.prev_total as f64;
        for (p, d) in &mut all {
            if total > 0.0 {
                p.cpu = round1(100.0 * *d as f64 / total);
            }
        }
        let mut v: Vec<Proc> = all.into_iter().map(|(p, _)| p).filter(|p| p.mem > 0 || p.cpu > 0.0).collect();
        v.sort_by(|a, b| b.cpu.total_cmp(&a.cpu).then(b.mem.cmp(&a.mem)));
        v.truncate(TOP_PROCS);
        v
    }
}

/// `/proc/<pid>/stat` → (comm, utime+stime ticks, threads, rss pages).
fn parse_stat(stat: &str) -> Option<(String, u64, u32, u64)> {
    // comm is parenthesised and may contain spaces or parentheses.
    let (l, r) = (stat.find('(')?, stat.rfind(')')?);
    let name = stat[l + 1..r].to_string();
    let f: Vec<&str> = stat[r + 1..].split_whitespace().collect();
    if f.len() < 22 {
        return None;
    }
    let ticks = f[11].parse::<u64>().unwrap_or(0) + f[12].parse::<u64>().unwrap_or(0);
    Some((name, ticks, f[17].parse().unwrap_or(0), f[21].parse().unwrap_or(0)))
}

/// The PC's POSIX time zone: the footer line of a TZif v2+ `/etc/localtime`, else `$TZ` when it is
/// already a POSIX string. `None` when neither says anything usable.
pub fn local_tz() -> Option<String> {
    if let Ok(bytes) = fs::read("/etc/localtime")
        && let Some(tz) = tzif_footer(&bytes)
    {
        return Some(tz);
    }
    std::env::var("TZ").ok().filter(|t| is_posix_tz(t))
}

fn is_posix_tz(t: &str) -> bool {
    // A name (letters, or <+04> style) followed by an offset: "IST-5:30", "UTC0", "<+04>-4".
    !t.starts_with(':')
        && !t.contains('/')
        && t.is_ascii()
        && (t.starts_with('<') || t.starts_with(|c: char| c.is_ascii_alphabetic()))
        && t.chars().any(|c| c.is_ascii_digit())
}

fn tzif_footer(bytes: &[u8]) -> Option<String> {
    if bytes.len() < 6 || &bytes[..4] != b"TZif" || bytes[4] < b'2' {
        return None;
    }
    let body = bytes.strip_suffix(b"\n")?;
    let start = body.iter().rposition(|&b| b == b'\n')? + 1;
    let tz = std::str::from_utf8(&body[start..]).ok()?.trim();
    is_posix_tz(tz).then(|| tz.to_string())
}

fn round1(x: f64) -> f64 {
    (x * 10.0).round() / 10.0
}

/// `AMD Ryzen 9 7950X 16-Core Processor` → `Ryzen 9 7950X`.
fn tidy_cpu_name(n: &str) -> String {
    let mut s = n.replace("(R)", "").replace("(TM)", "").replace("(tm)", "");
    for junk in [" Processor", " CPU"] {
        if let Some(i) = s.find(junk) {
            s.truncate(i);
        }
    }
    let words: Vec<&str> = s.split_whitespace().filter(|w| !w.ends_with("-Core") && *w != "AMD" && *w != "Intel").collect();
    let s = words.join(" ");
    s.split(" @ ").next().unwrap_or(&s).to_string()
}

fn mem() -> Mem {
    parse_meminfo(&read("/proc/meminfo").unwrap_or_default())
}

fn parse_meminfo(text: &str) -> Mem {
    let mut kv = HashMap::new();
    for line in text.lines() {
        let mut it = line.split_whitespace();
        if let (Some(k), Some(v)) = (it.next(), it.next().and_then(|v| v.parse::<u64>().ok())) {
            kv.insert(k.trim_end_matches(':').to_string(), v * 1024);
        }
    }
    let g = |k: &str| kv.get(k).copied().unwrap_or(0);
    Mem {
        total: g("MemTotal"),
        used: g("MemTotal").saturating_sub(g("MemAvailable")),
        available: g("MemAvailable"),
        cached: g("Cached") + g("SReclaimable"),
        free: g("MemFree"),
        swap_total: g("SwapTotal"),
        swap_used: g("SwapTotal").saturating_sub(g("SwapFree")),
    }
}

fn unescape_mount(s: &str) -> String {
    s.replace("\\040", " ").replace("\\011", "\t").replace("\\134", "\\")
}

fn disks() -> Vec<Disk> {
    let mut seen = HashSet::new();
    let mut out = Vec::new();
    for line in read("/proc/mounts").unwrap_or_default().lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() < 3 || !REAL_FS.contains(&f[2]) || !f[0].starts_with("/dev/") {
            continue;
        }
        let mount = unescape_mount(f[1]);
        if mount.starts_with("/snap") || mount.starts_with("/var/lib/") || mount.starts_with("/run/") {
            continue;
        }
        // btrfs subvolumes share a device: report it once, under its shortest mount point.
        if !seen.insert(f[0].to_string()) {
            continue;
        }
        let Ok(st) = rustix::fs::statvfs(mount.as_str()) else { continue };
        let total = st.f_blocks * st.f_frsize;
        if total == 0 {
            continue;
        }
        let free = st.f_bavail * st.f_frsize;
        out.push(Disk { mount, used: total.saturating_sub(free), total });
    }
    out.sort_by_key(|d| (d.mount != "/", d.mount.len()));
    out.truncate(MAX_DISKS);
    out
}

/// Interface of the default IPv4 route with the lowest metric.
fn default_iface() -> Option<String> {
    read("/proc/net/route")?
        .lines()
        .skip(1)
        .filter_map(|l| {
            let f: Vec<&str> = l.split_whitespace().collect();
            (f.len() > 6 && f[1] == "00000000").then(|| (f[6].parse::<u32>().unwrap_or(u32::MAX), f[0].to_string()))
        })
        .min()
        .map(|(_, i)| i)
}

/// Source address the kernel would use for outbound traffic. Connecting a UDP socket sends nothing.
fn local_ip() -> Option<String> {
    let s = UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect("192.0.2.1:9").ok()?;
    Some(s.local_addr().ok()?.ip().to_string())
}

fn find_cpu_temp() -> Option<PathBuf> {
    let mut hwmons: Vec<_> = fs::read_dir("/sys/class/hwmon").ok()?.flatten().map(|e| e.path()).collect();
    hwmons.sort();
    for hw in hwmons {
        if !read(hw.join("name")).is_some_and(|n| CPU_SENSORS.contains(&n.as_str())) {
            continue;
        }
        let mut inputs: Vec<_> = fs::read_dir(&hw)
            .ok()?
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("temp") && n.ends_with("_input")))
            .collect();
        inputs.sort();
        let preferred = inputs.iter().find(|p| {
            let label = p.to_string_lossy().replace("_input", "_label");
            read(label).is_some_and(|l| matches!(l.as_str(), "Tctl" | "Tdie" | "Package id 0"))
        });
        if let Some(p) = preferred.or(inputs.first()) {
            return Some(p.clone());
        }
    }
    None
}

/// AMD k10temp per-CCD sensors (`Tccd1` …) as ("CCD1", path).
fn find_ccd_temps() -> Vec<(String, PathBuf)> {
    let mut out = Vec::new();
    for hw in fs::read_dir("/sys/class/hwmon").into_iter().flatten().flatten().map(|e| e.path()) {
        if !read(hw.join("name")).is_some_and(|n| n == "k10temp" || n == "zenpower") {
            continue;
        }
        for e in fs::read_dir(&hw).into_iter().flatten().flatten() {
            let p = e.path();
            let Some(f) = p.file_name().and_then(|n| n.to_str()).filter(|n| n.ends_with("_label")) else { continue };
            if let Some(label) = read(&p)
                && let Some(n) = label.strip_prefix("Tccd")
            {
                out.push((format!("CCD{n}"), hw.join(f.replace("_label", "_input"))));
            }
        }
    }
    out.sort();
    out
}

fn find_gpus() -> Vec<GpuSource> {
    let mut out = Vec::new();
    if let Ok(nvml) = Nvml::init()
        && nvml.device_count().is_ok_and(|n| n > 0)
    {
        out.push(GpuSource::Nvidia(Box::new(nvml)));
    }
    let mut amd: Vec<PathBuf> = fs::read_dir("/sys/class/drm")
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_name().to_str().is_some_and(|n| n.starts_with("card") && !n.contains('-')))
        .map(|e| e.path().join("device"))
        // Discrete cards only: skip small iGPU carve-outs.
        .filter(|d| d.join("gpu_busy_percent").exists() && read_num::<u64>(d.join("mem_info_vram_total")).unwrap_or(0) > 2 << 30)
        .collect();
    amd.sort();
    if !amd.is_empty() {
        out.push(GpuSource::Amd(amd));
    }
    out
}

fn amd_gpu(card: &Path) -> Gpu {
    let hwmon = fs::read_dir(card.join("hwmon")).ok().and_then(|mut d| d.next()).and_then(|e| e.ok()).map(|e| e.path());
    let hw = |f: &str| hwmon.as_ref().and_then(|h| read_num::<f64>(h.join(f)));
    let fan = match (hw("pwm1"), hw("pwm1_max")) {
        (Some(p), Some(m)) if m > 0.0 => Some(round1(100.0 * p / m)),
        _ => None,
    };
    let mhz = read(card.join("pp_dpm_sclk")).and_then(|t| {
        let line = t.lines().find(|l| l.ends_with('*'))?;
        line.split_whitespace().nth(1)?.trim_end_matches("Mhz").parse().ok()
    });
    let name = read(card.join("product_name")).filter(|n| !n.is_empty()).unwrap_or_else(|| "Radeon".into());
    Gpu {
        name,
        load: read_num(card.join("gpu_busy_percent")).unwrap_or(0.0),
        temp: hw("temp1_input").map(|m| round1(m / 1000.0)),
        vram_used: read_num(card.join("mem_info_vram_used")).unwrap_or(0),
        vram_total: read_num(card.join("mem_info_vram_total")).unwrap_or(0),
        power: hw("power1_average").or(hw("power1_input")).map(|uw| round1(uw / 1e6)),
        power_max: hw("power1_cap").map(|uw| round1(uw / 1e6)),
        fan,
        mhz,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_names() {
        assert_eq!(tidy_cpu_name("AMD Ryzen 9 7950X 16-Core Processor"), "Ryzen 9 7950X");
        assert_eq!(tidy_cpu_name("Intel(R) Core(TM) i9-14900K"), "Core i9-14900K");
        assert_eq!(tidy_cpu_name("Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz"), "Core i7-8700");
    }

    #[test]
    fn tz_footer() {
        // TZif v2 header … v2 data … "\n<POSIX TZ>\n"
        let mut f = b"TZif2".to_vec();
        f.extend_from_slice(&[0u8; 40]);
        f.extend_from_slice(b"\nIST-5:30\n");
        assert_eq!(tzif_footer(&f).as_deref(), Some("IST-5:30"));
        let mut ny = b"TZif3".to_vec();
        ny.extend_from_slice(b"\x00\x01\nEST5EDT,M3.2.0,M11.1.0\n");
        assert_eq!(tzif_footer(&ny).as_deref(), Some("EST5EDT,M3.2.0,M11.1.0"));
        assert_eq!(tzif_footer(b"TZif\0no footer"), None); // v1 has no footer
        assert_eq!(tzif_footer(b"TZif2\n\n"), None); // empty footer
        assert!(is_posix_tz("<+04>-4") && !is_posix_tz(":/etc/localtime") && !is_posix_tz("Asia/Kolkata"));
    }

    #[test]
    fn meminfo() {
        let m = parse_meminfo(
            "MemTotal: 1000 kB\nMemFree: 200 kB\nMemAvailable: 600 kB\nCached: 300 kB\nSReclaimable: 50 kB\nSwapTotal: 100 kB\nSwapFree: 40 kB\n",
        );
        assert_eq!((m.total, m.used, m.available, m.cached, m.free), (1_024_000, 409_600, 614_400, 358_400, 204_800));
        assert_eq!((m.swap_total, m.swap_used), (102_400, 61_440));
    }

    #[test]
    fn proc_stat() {
        let stat = "2298 (tt (lcd) d) S 1 2298 2298 0 -1 4194560 100 0 0 0 150 50 0 0 20 0 6 0 1000 1000000 14080 18446744073709551615";
        assert_eq!(parse_stat(stat), Some(("tt (lcd) d".to_string(), 200, 6, 14080)));
    }

    #[test]
    fn samples_this_machine() {
        let mut s = Sampler::new();
        s.sample();
        std::thread::sleep(std::time::Duration::from_millis(300));
        let st = s.sample();
        assert!(st.mem.total > 0);
        assert!(!st.cpu.cores.is_empty());
        assert!(st.uptime > 0);
        assert!(st.disks.iter().any(|d| d.total > 0));
    }
}
