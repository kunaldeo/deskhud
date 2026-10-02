//! Just enough HTTP/1.1 for the display's small API: one request per connection.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};

pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}

impl Response {
    pub fn json(&self) -> Result<serde_json::Value> {
        serde_json::from_slice(&self.body).context("display sent invalid JSON")
    }

    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).trim().to_string()
    }
}

/// `host` or `host:port` → `host:port` (port 80 by default).
pub fn with_port(addr: &str) -> String {
    if addr.rsplit_once(':').is_some_and(|(_, p)| p.parse::<u16>().is_ok()) { addr.to_string() } else { format!("{addr}:80") }
}

pub fn connect(addr: &str, timeout: Duration) -> Result<TcpStream> {
    let addr = with_port(addr);
    let sa = addr.to_socket_addrs()?.next().ok_or_else(|| anyhow!("cannot resolve {addr}"))?;
    let s = TcpStream::connect_timeout(&sa, timeout).with_context(|| format!("connecting to {addr}"))?;
    s.set_nodelay(true)?;
    Ok(s)
}

/// Send a request. `body` is written in chunks, reporting `(sent, total)` to `progress`.
pub fn request(
    addr: &str,
    method: &str,
    path: &str,
    token: Option<&str>,
    body: Option<(&[u8], &str)>,
    timeout: Duration,
    mut progress: impl FnMut(usize, usize),
) -> Result<Response> {
    let mut s = connect(addr, Duration::from_secs(4))?;
    s.set_read_timeout(Some(timeout))?;
    s.set_write_timeout(Some(timeout))?;
    let mut head = format!(
        "{method} {path} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\nUser-Agent: deskhud/{}\r\n",
        with_port(addr),
        env!("CARGO_PKG_VERSION")
    );
    if let Some(t) = token {
        head += &format!("Authorization: Bearer {t}\r\n");
    }
    let data = body.map(|(b, _)| b).unwrap_or_default();
    if let Some((b, ct)) = body {
        head += &format!("Content-Type: {ct}\r\nContent-Length: {}\r\n", b.len());
    } else if method != "GET" {
        head += "Content-Length: 0\r\n";
    }
    head += "\r\n";
    s.write_all(head.as_bytes())?;
    for (i, chunk) in data.chunks(8192).enumerate() {
        if let Err(e) = s.write_all(chunk) {
            // The display may answer early (e.g. 401) and close; prefer its answer to EPIPE.
            return read_response(&mut s).map_err(|_| anyhow!(e));
        }
        progress((i * 8192 + chunk.len()).min(data.len()), data.len());
    }
    read_response(&mut s)
}

fn read_response(s: &mut TcpStream) -> Result<Response> {
    let mut r = BufReader::new(s);
    let mut line = String::new();
    r.read_line(&mut line)?;
    let status: u16 =
        line.split_whitespace().nth(1).and_then(|c| c.parse().ok()).ok_or_else(|| anyhow!("bad HTTP status line {line:?}"))?;
    let (mut len, mut chunked) = (None, false);
    loop {
        line.clear();
        if r.read_line(&mut line)? == 0 || line.trim().is_empty() {
            break;
        }
        if let Some((k, v)) = line.split_once(':') {
            match k.trim().to_ascii_lowercase().as_str() {
                "content-length" => len = v.trim().parse::<usize>().ok(),
                "transfer-encoding" => chunked = v.to_ascii_lowercase().contains("chunked"),
                _ => {}
            }
        }
    }
    let mut body = Vec::new();
    if chunked {
        loop {
            line.clear();
            r.read_line(&mut line)?;
            let n = usize::from_str_radix(line.trim().split(';').next().unwrap_or("0"), 16).unwrap_or(0);
            if n == 0 {
                break;
            }
            let start = body.len();
            body.resize(start + n, 0);
            r.read_exact(&mut body[start..])?;
            line.clear();
            r.read_line(&mut line)?;
        }
    } else if let Some(n) = len {
        body.resize(n, 0);
        r.read_exact(&mut body)?;
    } else {
        r.read_to_end(&mut body)?;
    }
    Ok(Response { status, body })
}

pub fn get_json(addr: &str, path: &str, token: Option<&str>) -> Result<serde_json::Value> {
    let r = request(addr, "GET", path, token, None, Duration::from_secs(4), |_, _| {})?;
    if r.status != 200 {
        bail!("GET {path}: HTTP {} {}", r.status, r.text());
    }
    r.json()
}
