//! `deskhud daemon`: stats and agent threads, the display link, and the local control socket.
//!
//! Control socket protocol: one JSON request line, one JSON response line.
//! `{"op":"status"}`, `{"op":"agents"}`, `{"op":"retarget","device":"…"}`,
//! `{"op":"send","msg":{…},"wait":["settings"],"soft":false}`, and `{"op":"hook","event":{…}}`
//! (no response: hook commands must never wait).

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};
use tracing::{info, warn};

use crate::agents::Monitor;
use crate::config::{self, Config};
use crate::link::{self, Command, LinkConfig, Shared};
use crate::stats::Sampler;

pub fn run() -> Result<()> {
    let cfg = Config::load()?;
    let sock = config::socket_path();
    if UnixStream::connect(&sock).is_ok() {
        bail!("deskhud daemon is already running ({})", sock.display());
    }
    let _ = std::fs::remove_file(&sock);
    let listener = UnixListener::bind(&sock).with_context(|| format!("binding {}", sock.display()))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&sock, std::fs::Permissions::from_mode(0o600));
    }

    let shared = Arc::new(Shared::default());
    let (hook_tx, hook_rx) = mpsc::channel::<Value>();
    let (cmd_tx, cmd_rx) = mpsc::channel::<Command>();

    spawn("deskhud-stats", {
        let shared = shared.clone();
        move || stats_loop(&shared)
    });
    spawn("deskhud-agents", {
        let shared = shared.clone();
        move || agents_loop(&shared, hook_rx)
    });
    spawn("deskhud-ipc", {
        let shared = shared.clone();
        let cmd_tx = Mutex::new(cmd_tx);
        move || {
            for conn in listener.incoming().flatten() {
                let (shared, hook_tx, cmd_tx) = (shared.clone(), hook_tx.clone(), cmd_tx.lock().unwrap().clone());
                thread::spawn(move || {
                    if let Err(e) = serve(conn, &shared, &hook_tx, &cmd_tx) {
                        tracing::debug!("control request: {e:#}");
                    }
                });
            }
        }
    });

    info!("deskhud {} pc_id {} as {:?}", env!("CARGO_PKG_VERSION"), cfg.pc_id, cfg.host_name());
    let link_cfg = LinkConfig {
        pc_id: cfg.pc_id.clone(),
        host: cfg.host_name(),
        os: link::default_os(),
        device: cfg.device.clone(),
        state_dir: config::state_dir(),
    };
    link::run(link_cfg, shared, cmd_rx);
    Ok(())
}

fn spawn(name: &str, f: impl FnOnce() + Send + 'static) {
    thread::Builder::new().name(name.into()).spawn(f).expect("spawn thread");
}

/// Sample once a second while this PC is the active one (nobody looks at standby stats).
fn stats_loop(shared: &Shared) {
    let mut sampler = Sampler::new();
    let mut primed = false;
    loop {
        if shared.is_active() {
            if !primed {
                sampler.sample();
                primed = true;
                thread::sleep(Duration::from_millis(250));
            }
            let s = sampler.sample();
            *shared.stats.lock().unwrap() = serde_json::to_value(&s).ok();
        } else {
            primed = false;
        }
        thread::sleep(Duration::from_secs(1));
    }
}

/// Poll session logs once a second, and right away when a hook event arrives.
fn agents_loop(shared: &Shared, hooks: Receiver<Value>) {
    let mut monitor = Monitor::new(crate::agents::providers()).with_liveness();
    let mut last = Value::Null;
    loop {
        match hooks.recv_timeout(Duration::from_secs(1)) {
            Ok(ev) => {
                monitor.hook(&ev);
                // Hooks come in bursts (PostToolUse + PreToolUse): take them all, then poll once.
                thread::sleep(Duration::from_millis(30));
                while let Ok(ev) = hooks.try_recv() {
                    monitor.hook(&ev);
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
        }
        let demo = shared.demo_since.load(std::sync::atomic::Ordering::Relaxed);
        let sessions = if demo > 0 {
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
            crate::demo::sessions(demo, now)
        } else {
            serde_json::to_value(monitor.poll()).unwrap_or(Value::Array(vec![]))
        };
        if sessions != last {
            let mut a = shared.agents.lock().unwrap();
            a.0 += 1;
            a.1 = sessions.clone();
            last = sessions;
        }
    }
}

fn serve(conn: UnixStream, shared: &Shared, hook_tx: &Sender<Value>, cmd_tx: &Sender<Command>) -> Result<()> {
    conn.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut line = String::new();
    BufReader::new(&conn).read_line(&mut line)?;
    let req: Value = serde_json::from_str(&line)?;
    let resp = match req["op"].as_str().unwrap_or_default() {
        "hook" => {
            let _ = hook_tx.send(req["event"].clone());
            return Ok(());
        }
        "status" => {
            let st = serde_json::to_value(&*shared.status.lock().unwrap())?;
            let sessions = shared.agents.lock().unwrap().1.clone();
            json!({"ok":true,"status":st,"sessions":sessions})
        }
        "agents" => json!({"ok":true,"sessions":shared.agents.lock().unwrap().1.clone()}),
        "demo" => {
            let on = req["on"] == true;
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
            shared.demo_since.store(if on { now } else { 0 }, std::sync::atomic::Ordering::Relaxed);
            json!({"ok":true,"demo":on})
        }
        "retarget" => {
            *shared.retarget.lock().unwrap() = Some(req["device"].as_str().unwrap_or_default().to_string());
            json!({"ok":true})
        }
        "send" => {
            let (tx, rx) = mpsc::channel();
            let wait: Vec<String> = req["wait"].as_array().into_iter().flatten().filter_map(|w| w.as_str().map(String::from)).collect();
            cmd_tx.send(Command { msg: req["msg"].clone(), wait, soft: req["soft"] == true, reply: Some(tx) })?;
            match rx.recv_timeout(Duration::from_secs(8)) {
                Ok(Ok(v)) => json!({"ok":true,"result":v}),
                Ok(Err(e)) => json!({"ok":false,"error":e}),
                Err(_) => json!({"ok":false,"error":"timed out"}),
            }
        }
        op => json!({"ok":false,"error":format!("unknown op {op:?}")}),
    };
    let mut w = &conn;
    w.write_all(format!("{resp}\n").as_bytes())?;
    Ok(())
}

/// Send one request to the running daemon and read its answer.
pub fn request(req: &Value) -> Result<Value> {
    request_at(&config::socket_path(), req)
}

pub fn request_at(sock: &Path, req: &Value) -> Result<Value> {
    let conn = UnixStream::connect(sock).context("deskhud daemon is not running (start it: systemctl --user start deskhud)")?;
    conn.set_read_timeout(Some(Duration::from_secs(12)))?;
    let mut w = &conn;
    w.write_all(format!("{req}\n").as_bytes())?;
    let mut line = String::new();
    BufReader::new(&conn).read_line(&mut line)?;
    let v: Value = serde_json::from_str(&line).context("bad answer from daemon")?;
    if v["ok"] != true {
        bail!("{}", v["error"].as_str().unwrap_or("request failed"));
    }
    Ok(v)
}

/// `deskhud hook`: forward a Claude Code hook event. Never fails, never blocks for long, never
/// prints: Claude Code reads hook stdout.
pub fn forward_hook() {
    use std::io::Read;
    let mut input = Vec::new();
    let _ = std::io::stdin().take(4 << 20).read_to_end(&mut input);
    let Ok(event) = serde_json::from_slice::<Value>(&input) else { return };
    // Large tool inputs (file writes) are not needed for state: keep the message small.
    let event = slim_event(event);
    if let Ok(conn) = UnixStream::connect(config::socket_path()) {
        let _ = conn.set_write_timeout(Some(Duration::from_millis(500)));
        let mut w = &conn;
        if w.write_all(format!("{}\n", json!({"op":"hook","event":event})).as_bytes()).is_err() {
            warn!("hook: daemon did not accept the event");
        }
    }
}

fn slim_event(mut ev: Value) -> Value {
    if let Some(o) = ev.as_object_mut() {
        o.remove("tool_response");
        if let Some(input) = o.get_mut("tool_input").and_then(Value::as_object_mut) {
            for v in input.values_mut() {
                if let Some(s) = v.as_str()
                    && s.len() > 2000
                {
                    *v = Value::String(s.chars().take(500).collect());
                }
            }
        }
    }
    ev
}
