//! The link against an in-process mock display: pairing, roles, pacing, reconnect, CLI commands.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use deskhud::config::State;
use deskhud::link::{self, Command, LinkConfig, Shared};
use serde_json::{Value, json};
use tungstenite::{Message, WebSocket};

type Ws = WebSocket<TcpStream>;

/// Accept the next WebSocket connection, answering `/api/info` probes on the way.
fn accept_ws(listener: &TcpListener) -> Ws {
    loop {
        let (mut s, _) = listener.accept().unwrap();
        let mut buf = [0u8; 64];
        let n = s.peek(&mut buf).unwrap();
        if buf[..n].starts_with(b"GET /api/info") {
            let mut req = [0u8; 1024];
            let _ = s.read(&mut req);
            let body = json!({"id":"mock01","name":"Mock","fw":"0.0.1","uptime":5}).to_string();
            let _ = write!(
                s,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                body.len()
            );
            continue;
        }
        let ws = tungstenite::accept(s).unwrap();
        ws.get_ref().set_read_timeout(Some(Duration::from_millis(50))).unwrap();
        return ws;
    }
}

/// Next JSON frame within `timeout`.
fn recv(ws: &mut Ws, timeout: Duration) -> Option<Value> {
    let end = Instant::now() + timeout;
    while Instant::now() < end {
        match ws.read() {
            Ok(Message::Text(t)) => return serde_json::from_str(t.as_str()).ok(),
            Ok(_) => {}
            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
            Err(_) => return None,
        }
    }
    None
}

/// Frames received during `d`, by type.
fn collect(ws: &mut Ws, d: Duration) -> Vec<String> {
    let end = Instant::now() + d;
    let mut out = Vec::new();
    while let Some(left) = end.checked_duration_since(Instant::now()) {
        if let Some(v) = recv(ws, left) {
            out.push(v["t"].as_str().unwrap_or_default().to_string());
        }
    }
    out
}

fn send(ws: &mut Ws, v: Value) {
    ws.send(Message::text(v.to_string())).unwrap();
}

/// Wait for a frame of type `t`, skipping others.
fn expect(ws: &mut Ws, t: &str, timeout: Duration) -> Value {
    let end = Instant::now() + timeout;
    while let Some(left) = end.checked_duration_since(Instant::now()) {
        if let Some(v) = recv(ws, left)
            && v["t"] == t
        {
            return v;
        }
    }
    panic!("no {t} frame within {timeout:?}");
}

#[test]
fn pair_roles_reconnect_and_commands() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let state_dir = tempfile::tempdir().unwrap();

    let shared = Arc::new(Shared::default());
    *shared.stats.lock().unwrap() = Some(json!({"cpu":{"load":12.5}}));
    *shared.agents.lock().unwrap() = (1, json!([{"id":"claude:s1","state":"working"}]));
    let (cmd_tx, cmd_rx) = mpsc::channel::<Command>();
    let cfg = LinkConfig {
        pc_id: "pc-1".into(),
        host: "testbox".into(),
        os: "Test OS".into(),
        device: format!("127.0.0.1:{port}"),
        state_dir: state_dir.path().to_path_buf(),
    };
    {
        let shared = shared.clone();
        thread::spawn(move || link::run(cfg, shared, cmd_rx));
    }

    // First connection: unknown PC, paired on the display.
    let mut ws = accept_ws(&listener);
    let hello = expect(&mut ws, "hello", Duration::from_secs(3));
    assert_eq!(
        (hello["pc_id"].as_str(), hello["host"].as_str(), hello["token"].as_str(), hello["proto"].as_u64()),
        (Some("pc-1"), Some("testbox"), Some(""), Some(1))
    );
    send(&mut ws, json!({"t":"pair_pending","code":"482913"}));
    thread::sleep(Duration::from_millis(200));
    {
        let st = shared.status.lock().unwrap();
        assert_eq!((st.state.as_str(), st.pair_code.as_str()), ("pairing", "482913"));
    }
    // Nothing but pings while pairing.
    assert!(collect(&mut ws, Duration::from_millis(500)).iter().all(|t| t == "ping"));
    send(&mut ws, json!({"t":"pair_result","ok":true,"token":"tok123"}));
    expect(&mut ws, "get", Duration::from_secs(2));
    send(&mut ws, json!({"t":"role","active":true,"active_host":"testbox"}));
    // Active: stats at once, agents at once.
    let got = collect(&mut ws, Duration::from_millis(2500));
    assert!(got.iter().filter(|t| *t == "stats").count() >= 2, "{got:?}");
    assert!(got.iter().any(|t| t == "agents"), "{got:?}");
    assert_eq!(State::load(state_dir.path()).tokens.get("mock01").map(String::as_str), Some("tok123"));

    // Agents change: sent again promptly.
    *shared.agents.lock().unwrap() = (2, json!([{"id":"claude:s1","state":"permission"}]));
    let a = expect(&mut ws, "agents", Duration::from_secs(1));
    assert_eq!(a["sessions"][0]["state"], "permission");

    // The display drops the connection: the link comes back with its token.
    drop(ws);
    let mut ws = accept_ws(&listener);
    let hello = expect(&mut ws, "hello", Duration::from_secs(5));
    assert_eq!(hello["token"], "tok123");
    send(&mut ws, json!({"t":"welcome","device":{"id":"mock01","name":"Mock","fw":"0.0.1"},"settings":{"brightness":80}}));
    send(&mut ws, json!({"t":"role","active":false,"active_host":"otherpc"}));
    // Standby: heartbeat only.
    let got = collect(&mut ws, Duration::from_millis(2500));
    assert!(got.iter().any(|t| t == "ping"), "{got:?}");
    assert!(got.iter().all(|t| t == "ping"), "standby must not stream: {got:?}");
    assert_eq!(shared.status.lock().unwrap().active_host, "otherpc");

    // Failover to this PC: data flows immediately.
    let t0 = Instant::now();
    send(&mut ws, json!({"t":"role","active":true,"active_host":"testbox"}));
    expect(&mut ws, "stats", Duration::from_secs(1));
    assert!(t0.elapsed() < Duration::from_millis(500));

    // A CLI command round trip.
    let (tx, rx) = mpsc::channel();
    cmd_tx
        .send(Command { msg: json!({"t":"set","settings":{"brightness":40}}), wait: vec!["settings".into()], soft: false, reply: Some(tx) })
        .unwrap();
    let set = expect(&mut ws, "set", Duration::from_secs(1));
    assert_eq!(set["settings"]["brightness"], 40);
    send(&mut ws, json!({"t":"settings","settings":{"brightness":40}}));
    let r = rx.recv_timeout(Duration::from_secs(2)).unwrap().unwrap();
    assert_eq!(r["settings"]["brightness"], 40);

    // Soft wait (role is only sent on change): times out to Ok.
    let (tx, rx) = mpsc::channel();
    cmd_tx.send(Command { msg: json!({"t":"cmd","cmd":"activate"}), wait: vec!["role".into()], soft: true, reply: Some(tx) }).unwrap();
    expect(&mut ws, "cmd", Duration::from_secs(1));
    assert!(rx.recv_timeout(Duration::from_secs(6)).unwrap().is_ok());
}

#[test]
fn denied_pairing_backs_off() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let state_dir = tempfile::tempdir().unwrap();
    let shared = Arc::new(Shared::default());
    let (_cmd_tx, cmd_rx) = mpsc::channel::<Command>();
    let cfg = LinkConfig {
        pc_id: "pc-2".into(),
        host: "h".into(),
        os: "o".into(),
        device: format!("127.0.0.1:{port}"),
        state_dir: state_dir.path().to_path_buf(),
    };
    {
        let shared = shared.clone();
        thread::spawn(move || link::run(cfg, shared, cmd_rx));
    }
    let mut ws = accept_ws(&listener);
    expect(&mut ws, "hello", Duration::from_secs(3));
    send(&mut ws, json!({"t":"pair_result","ok":false}));
    thread::sleep(Duration::from_millis(300));
    assert_eq!(shared.status.lock().unwrap().state, "denied");
    // No immediate retry that would pop the pairing dialog again.
    listener.set_nonblocking(true).unwrap();
    thread::sleep(Duration::from_secs(2));
    assert!(listener.accept().is_err(), "reconnected right after a denial");
}
