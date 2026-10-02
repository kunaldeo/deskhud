//! The connection to the display: discovery, hello / pairing, role, and pacing what is sent.
//!
//! One thread owns the WebSocket. Everything else talks to it through [`Shared`]: the stats and
//! agents threads publish their latest output there, and CLI requests arrive as [`Command`]s.

use std::collections::VecDeque;
use std::net::TcpStream;
use std::path::PathBuf;
use std::sync::mpsc::{Receiver, Sender, TryRecvError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, anyhow, bail};
use serde::Serialize;
use serde_json::{Map, Value, json};
use tracing::{debug, info, warn};
use tungstenite::{Message, WebSocket};

use crate::config::{State, is_static_host};
use crate::{discover, http};

pub const PROTO: u64 = 1;
const STATS_EVERY: Duration = Duration::from_secs(1);
/// Agents are re-sent at least this often while active, and at most once per `AGENTS_MIN`.
const AGENTS_EVERY: Duration = Duration::from_secs(5);
const AGENTS_MIN: Duration = Duration::from_millis(500);
const PING_STANDBY: Duration = Duration::from_secs(2);
const PING_ACTIVE: Duration = Duration::from_secs(5);
/// The display answers pings; this much silence means the connection is dead.
const DEAD_AFTER: Duration = Duration::from_secs(15);
const REPLY_TIMEOUT: Duration = Duration::from_secs(4);
const POLL: Duration = Duration::from_millis(50);
const BACKOFF_MAX: Duration = Duration::from_secs(15);
/// After the user denies pairing on the display, do not ask again for this long.
const DENIED_BACKOFF: Duration = Duration::from_secs(120);

#[derive(Clone, Debug, Default, Serialize)]
pub struct Status {
    /// discovering | connecting | pairing | connected | denied | offline
    pub state: String,
    pub device_id: String,
    pub addr: String,
    pub device: Option<Value>,
    pub settings: Option<Value>,
    pub active: Option<bool>,
    pub active_host: String,
    pub pair_code: String,
    pub last_error: String,
    pub connected_since: Option<u64>,
}

/// A message to send to the display, optionally waiting for replies of the given types.
pub struct Command {
    pub msg: Value,
    pub wait: Vec<String>,
    /// On timeout, reply with what arrived instead of an error (e.g. `role` is only sent on change).
    pub soft: bool,
    pub reply: Option<Sender<Result<Value, String>>>,
}

#[derive(Default)]
pub struct Shared {
    pub stats: Mutex<Option<Value>>,
    /// (version, sessions JSON array)
    pub agents: Mutex<(u64, Value)>,
    pub status: Mutex<Status>,
    /// New `device` target set by `deskhud use`.
    pub retarget: Mutex<Option<String>>,
    /// `deskhud demo on`: unix time it was switched on (0 = off).
    pub demo_since: std::sync::atomic::AtomicI64,
}

impl Shared {
    pub fn is_active(&self) -> bool {
        self.status.lock().unwrap().active == Some(true)
    }

    fn update(&self, f: impl FnOnce(&mut Status)) {
        f(&mut self.status.lock().unwrap());
    }
}

pub struct LinkConfig {
    pub pc_id: String,
    pub host: String,
    pub os: String,
    /// Device id, static `host[:port]`, or empty for "the only display around".
    pub device: String,
    pub state_dir: PathBuf,
}

struct Pending {
    waiting: Vec<String>,
    got: Map<String, Value>,
    soft: bool,
    reply: Sender<Result<Value, String>>,
    deadline: Instant,
}

enum End {
    /// Connection lost or failed: retry soon.
    Retry(anyhow::Error),
    Denied,
    Retarget,
}

pub fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Run forever: find the display, stay connected, reconnect with backoff.
pub fn run(mut cfg: LinkConfig, shared: Arc<Shared>, cmds: Receiver<Command>) {
    let mut backoff = Duration::from_secs(1);
    loop {
        if let Some(d) = shared.retarget.lock().unwrap().take() {
            cfg.device = d;
        }
        shared.update(|s| {
            s.state = "discovering".into();
            s.active = None;
        });
        let end = match resolve(&cfg) {
            Ok((id, addr)) => {
                shared.update(|s| {
                    s.state = "connecting".into();
                    s.device_id = id.clone();
                    s.addr = addr.clone();
                });
                session(&cfg, &shared, &cmds, &id, &addr)
            }
            Err(e) => End::Retry(e),
        };
        fail_pending_cmds(&cmds, "not connected to a display");
        let wait = match end {
            End::Retarget => Duration::ZERO,
            End::Denied => {
                warn!("pairing was denied on the display; asking again in {}s", DENIED_BACKOFF.as_secs());
                shared.update(|s| s.state = "denied".into());
                DENIED_BACKOFF
            }
            End::Retry(e) => {
                let msg = format!("{e:#}");
                let was_connected = {
                    let st = shared.status.lock().unwrap();
                    if st.last_error != msg {
                        warn!("{msg}");
                    }
                    st.connected_since.is_some()
                };
                shared.update(|s| {
                    s.state = "offline".into();
                    s.active = None;
                    s.connected_since = None;
                    s.last_error = msg;
                });
                // A connection that worked and dropped is retried at once; repeated failures back off.
                if was_connected {
                    backoff = Duration::from_secs(1);
                }
                let w = backoff;
                backoff = (backoff * 2).min(BACKOFF_MAX);
                w
            }
        };
        sleep_unless_retarget(&shared, &cmds, wait);
    }
}

/// Sleep, but wake early for `deskhud use`, failing CLI requests meanwhile.
fn sleep_unless_retarget(shared: &Shared, cmds: &Receiver<Command>, d: Duration) {
    let end = Instant::now() + d;
    while Instant::now() < end {
        if shared.retarget.lock().unwrap().is_some() {
            return;
        }
        fail_pending_cmds(cmds, "not connected to a display");
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn fail_pending_cmds(cmds: &Receiver<Command>, why: &str) {
    while let Ok(c) = cmds.try_recv() {
        if let Some(r) = c.reply {
            let _ = r.send(Err(why.to_string()));
        }
    }
}

/// Which display, at which address.
fn resolve(cfg: &LinkConfig) -> Result<(String, String)> {
    if is_static_host(&cfg.device) {
        let addr = http::with_port(&cfg.device);
        let info = http::get_json(&addr, "/api/info", None).with_context(|| format!("display at {addr} not reachable"))?;
        let id = info["id"].as_str().ok_or_else(|| anyhow!("{addr} did not report a device id"))?;
        return Ok((id.to_string(), addr));
    }
    let want = (!cfg.device.is_empty()).then_some(cfg.device.as_str());
    let state = State::load(&cfg.state_dir);
    // The last known address answers instantly; mDNS is only needed when it moved. Without a
    // configured display, any display this PC is paired with qualifies.
    let candidates: Vec<&String> = match want {
        Some(id) => state.last_addr.keys().filter(|k| k.as_str() == id).collect(),
        None => state.last_addr.keys().filter(|k| state.tokens.contains_key(*k)).collect(),
    };
    for id in candidates {
        let addr = &state.last_addr[id];
        if http::get_json(addr, "/api/info", None).is_ok_and(|i| i["id"] == id.as_str()) {
            return Ok((id.clone(), addr.clone()));
        }
    }
    let found = discover::browse(Duration::from_secs(4), want)?;
    let pick = match want {
        Some(id) => found.into_iter().find(|f| f.id == id).ok_or_else(|| anyhow!("display {id} not found on the network"))?,
        None => match found.len() {
            0 => bail!("no display found on the network (mDNS _deskhud._tcp)"),
            1 => found.into_iter().next().unwrap(),
            n => {
                // Prefer one this PC is already paired with.
                let f = found.iter().find(|f| state.tokens.contains_key(&f.id)).unwrap_or(&found[0]).clone();
                info!("{n} displays found; using {} ({}). Choose with `deskhud use <id>`", f.name, f.id);
                f
            }
        },
    };
    Ok((pick.id, pick.addr))
}

fn os_name() -> String {
    std::fs::read_to_string("/etc/os-release")
        .ok()
        .and_then(|t| {
            let get = |k: &str| t.lines().find_map(|l| l.strip_prefix(k).map(|v| v.trim_matches('"').to_string()));
            get("PRETTY_NAME=").or_else(|| get("NAME="))
        })
        .unwrap_or_else(|| "Linux".into())
}

/// What the display shows under the PC's name: the OS and, like fastfetch's summary, the desktop
/// ("Omarchy · Hyprland").
pub fn default_os() -> String {
    let os = os_name();
    match std::env::var("XDG_CURRENT_DESKTOP").ok().and_then(|d| d.split(':').next().map(str::to_string)).filter(|d| !d.is_empty()) {
        Some(de) if !os.contains(&de) => format!("{os} · {de}"),
        _ => os,
    }
}

fn is_timeout(e: &tungstenite::Error) -> bool {
    matches!(e, tungstenite::Error::Io(io) if matches!(io.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut))
}

fn send(ws: &mut WebSocket<TcpStream>, v: &Value) -> Result<()> {
    ws.send(Message::text(v.to_string())).context("sending to display")
}

fn session(cfg: &LinkConfig, shared: &Shared, cmds: &Receiver<Command>, id: &str, addr: &str) -> End {
    let stream = match http::connect(addr, Duration::from_secs(4)) {
        Ok(s) => s,
        Err(e) => return End::Retry(e),
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let _ = stream.set_write_timeout(Some(Duration::from_secs(5)));
    let mut ws = match tungstenite::client(format!("ws://{addr}/ws"), stream) {
        Ok((ws, _)) => ws,
        Err(e) => return End::Retry(anyhow!("WebSocket handshake with {addr}: {e}")),
    };
    let _ = ws.get_ref().set_read_timeout(Some(POLL));
    let mut state = State::load(&cfg.state_dir);
    if state.last_addr.get(id).map(String::as_str) != Some(addr) {
        state.last_addr.insert(id.to_string(), addr.to_string());
        let _ = state.save(&cfg.state_dir);
    }
    let token = state.tokens.get(id).cloned().unwrap_or_default();
    let hello =
        json!({"t":"hello","proto":PROTO,"pc_id":cfg.pc_id,"host":cfg.host,"os":cfg.os,"agent":env!("CARGO_PKG_VERSION"),"token":token,"tz":crate::stats::local_tz()});
    if let Err(e) = send(&mut ws, &hello) {
        return End::Retry(e);
    }
    info!("connected to display {id} at {addr}");
    match converse(cfg, shared, cmds, id, &mut ws) {
        Ok(end) => end,
        Err(e) => End::Retry(e),
    }
}

fn converse(cfg: &LinkConfig, shared: &Shared, cmds: &Receiver<Command>, id: &str, ws: &mut WebSocket<TcpStream>) -> Result<End> {
    let far_past = Instant::now() - Duration::from_secs(3600);
    let mut last_rx = Instant::now();
    let (mut last_stats, mut last_agents, mut last_ping) = (far_past, far_past, Instant::now());
    let mut sent_agents_version = u64::MAX;
    let mut pending: VecDeque<Pending> = VecDeque::new();
    let mut top = crate::sysview::ProcTop::new();

    loop {
        // Incoming.
        loop {
            match ws.read() {
                Ok(Message::Text(t)) => {
                    last_rx = Instant::now();
                    let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else {
                        debug!("ignoring non-JSON frame");
                        continue;
                    };
                    let kind = v["t"].as_str().unwrap_or_default().to_string();
                    debug!("<- {kind}");
                    match kind.as_str() {
                        "welcome" => shared.update(|s| {
                            s.state = "connected".into();
                            s.device = Some(v["device"].clone());
                            if v["settings"].is_object() {
                                s.settings = Some(v["settings"].clone());
                            }
                            s.pair_code.clear();
                            s.last_error.clear();
                            s.connected_since.get_or_insert(now_secs());
                        }),
                        "pair_pending" => {
                            let code = v["code"].as_str().unwrap_or_default().to_string();
                            info!("pairing: tap Allow on the display");
                            // One notification per pairing attempt, not per reconnect while it waits.
                            if !NOTIFIED_PAIRING.swap(true, std::sync::atomic::Ordering::Relaxed) {
                                notify_desktop(&format!("Tap Allow on the DeskHUD display to connect {}", cfg.host));
                            }
                            shared.update(|s| {
                                s.state = "pairing".into();
                                s.pair_code = code;
                            });
                        }
                        "pair_result" => {
                            if v["ok"] != true {
                                let _ = ws.close(None);
                                return Ok(End::Denied);
                            }
                            NOTIFIED_PAIRING.store(false, std::sync::atomic::Ordering::Relaxed);
                    if let Some(tok) = v["token"].as_str() {
                                let mut st = State::load(&cfg.state_dir);
                                st.tokens.insert(id.to_string(), tok.to_string());
                                st.save(&cfg.state_dir)?;
                                info!("paired with display {id}");
                            }
                            shared.update(|s| {
                                s.state = "connected".into();
                                s.pair_code.clear();
                                s.connected_since.get_or_insert(now_secs());
                            });
                            // Device info and settings, in case no welcome follows.
                            send(ws, &json!({"t":"get"}))?;
                        }
                        "role" => {
                            let active = v["active"] == true;
                            let was = shared.status.lock().unwrap().active;
                            shared.update(|s| {
                                s.active = Some(active);
                                s.active_host = v["active_host"].as_str().unwrap_or_default().to_string();
                                if s.state != "connected" {
                                    s.state = "connected".into();
                                    s.connected_since.get_or_insert(now_secs());
                                }
                            });
                            if was != Some(active) {
                                info!("{} on the display", if active { "active" } else { "standby" });
                            }
                            if active && was != Some(true) {
                                // Taking over: show this PC's data at once.
                                last_stats = far_past;
                                last_agents = far_past;
                                sent_agents_version = u64::MAX;
                            }
                        }
                        // The display's popup wants the whole text behind a shortened line.
                        "full" => {
                            let text = crate::agents::full_text(v["session"].as_str().unwrap_or_default(), v["tool"].as_str().unwrap_or_default());
                            send(ws, &json!({"t":"full","req":v["req"],"text":text.unwrap_or_default()}))?;
                        }
                        // Popups on the display: system info (fastfetch) and the process table.
                        "sysinfo" => {
                            let mut r = crate::sysview::fastfetch();
                            r["t"] = json!("sysinfo");
                            r["req"] = v["req"].clone();
                            send(ws, &r)?;
                        }
                        "procs" => {
                            // Screenshot mode: a sample table, not this PC's processes.
                            let demo = shared.demo_since.load(std::sync::atomic::Ordering::Relaxed) > 0;
                            let mut r = if demo { crate::demo::procs() } else { top.sample(v["n"].as_u64().unwrap_or(40).min(60) as usize) };
                            r["t"] = json!("procs");
                            r["req"] = v["req"].clone();
                            send(ws, &r)?;
                        }
                        "settings" => shared.update(|s| s.settings = Some(v["settings"].clone())),
                        "device" => shared.update(|s| s.device = Some(v.get("device").cloned().unwrap_or_else(|| strip_t(&v)))),
                        "error" => {
                            let msg = v["msg"].as_str().unwrap_or("error").to_string();
                            warn!("display: {msg}");
                            if let Some(p) = pending.pop_front() {
                                let _ = p.reply.send(Err(msg.clone()));
                            }
                            shared.update(|s| s.last_error = msg);
                        }
                        _ => {}
                    }
                    resolve_pending(&mut pending, &kind, &v);
                }
                Ok(Message::Close(_)) => return Ok(End::Retry(anyhow!("display closed the connection"))),
                Ok(_) => last_rx = Instant::now(),
                Err(e) if is_timeout(&e) => break,
                Err(e) => return Ok(End::Retry(anyhow!("connection lost: {e}"))),
            }
        }

        if shared.retarget.lock().unwrap().is_some() {
            let _ = ws.close(None);
            return Ok(End::Retarget);
        }

        // CLI requests.
        loop {
            match cmds.try_recv() {
                Ok(c) => {
                    send(ws, &c.msg)?;
                    if let Some(reply) = c.reply {
                        if c.wait.is_empty() {
                            let _ = reply.send(Ok(Value::Null));
                        } else {
                            pending.push_back(Pending {
                                waiting: c.wait,
                                got: Map::new(),
                                soft: c.soft,
                                reply,
                                deadline: Instant::now() + REPLY_TIMEOUT,
                            });
                        }
                    }
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => return Ok(End::Retry(anyhow!("shutting down"))),
            }
        }
        let now = Instant::now();
        while pending.front().is_some_and(|p| p.deadline <= now) {
            let p = pending.pop_front().unwrap();
            let _ = p.reply.send(if p.soft {
                Ok(Value::Object(p.got))
            } else {
                Err(format!("display did not answer ({} missing)", p.waiting.join(", ")))
            });
        }

        // Outgoing data.
        let (connected, active) = {
            let s = shared.status.lock().unwrap();
            (s.state == "connected", s.active == Some(true))
        };
        if connected && active {
            if now.duration_since(last_stats) >= STATS_EVERY
                && let Some(stats) = shared.stats.lock().unwrap().clone()
            {
                let mut msg = stats;
                msg["t"] = json!("stats");
                send(ws, &msg)?;
                last_stats = now;
            }
            let (version, sessions) = shared.agents.lock().unwrap().clone();
            let since = now.duration_since(last_agents);
            if (version != sent_agents_version && since >= AGENTS_MIN) || since >= AGENTS_EVERY {
                send(ws, &json!({"t":"agents","sessions":sessions}))?;
                last_agents = now;
                sent_agents_version = version;
            }
        }
        let ping_every = if active { PING_ACTIVE } else { PING_STANDBY };
        if now.duration_since(last_ping) >= ping_every {
            send(ws, &json!({"t":"ping"}))?;
            last_ping = now;
        }
        if now.duration_since(last_rx) > DEAD_AFTER {
            return Ok(End::Retry(anyhow!("display stopped responding")));
        }
    }
}

fn strip_t(v: &Value) -> Value {
    let mut v = v.clone();
    if let Some(o) = v.as_object_mut() {
        o.remove("t");
    }
    v
}

fn resolve_pending(pending: &mut VecDeque<Pending>, kind: &str, v: &Value) {
    let Some(i) = pending.iter().position(|p| p.waiting.iter().any(|w| w == kind)) else { return };
    let p = &mut pending[i];
    p.waiting.retain(|w| w != kind);
    let body = match kind {
        "settings" => v["settings"].clone(),
        "device" => v.get("device").cloned().unwrap_or_else(|| strip_t(v)),
        _ => strip_t(v),
    };
    p.got.insert(kind.to_string(), body);
    if p.waiting.is_empty() {
        let p = pending.remove(i).unwrap();
        let _ = p.reply.send(Ok(Value::Object(p.got)));
    }
}

/// Desktop notification; best effort, never blocks the link.
/// Set once the "tap Allow" notification went out; cleared when pairing ends.
static NOTIFIED_PAIRING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn notify_desktop(body: &str) {
    let body = body.to_string();
    std::thread::spawn(move || {
        let _ = std::process::Command::new("notify-send")
            .args(["--app-name=DeskHUD", "-i", "video-display", "DeskHUD pairing", &body])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    });
}
