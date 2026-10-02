//! opencode: sessions in SQLite, `$XDG_DATA_HOME/opencode/opencode.db` (`~/.local/share/…`).
//! `session` rows (working directory, title, model), `message` rows (JSON: role, times, tokens,
//! `finish`, `error`) and `part` rows (JSON: `text`, `reasoning`, `tool` calls with their state,
//! step markers). Read whole on each poll, recent sessions only; the database is opened read-only.

use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::time::SystemTime;

use rusqlite::{Connection, OpenFlags};
use serde_json::Value;

use super::{
    Line, Lines, Provider, Session, State, base_name, detail, first_line, full_tool, home, markdown, mtime, prose, short_model,
    strip_markdown, tool_detail, tool_line, unix_ms,
};

/// Sessions read per poll: the most recently active ones.
const MAX_READ: usize = 8;

pub struct OpenCode {
    db: PathBuf,
    config: PathBuf,
    config_mtime: Option<SystemTime>,
    windows: HashMap<String, u64>,
    conn: Option<Connection>,
}

impl Default for OpenCode {
    fn default() -> Self {
        Self::new()
    }
}

fn data_dir() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".local/share")).join("opencode")
}

impl OpenCode {
    pub fn new() -> Self {
        let config = std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".config"));
        OpenCode {
            db: data_dir().join("opencode.db"),
            config: config.join("opencode/opencode.json"),
            config_mtime: None,
            windows: HashMap::new(),
            conn: None,
        }
    }

    fn conn(&mut self) -> Option<&Connection> {
        if self.conn.is_none() && self.db.is_file() {
            let c = Connection::open_with_flags(&self.db, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX).ok()?;
            let _ = c.busy_timeout(std::time::Duration::from_millis(200));
            self.conn = Some(c);
        }
        self.conn.as_ref()
    }
}

/// opencode's tool input under the names the shared helpers know (`filePath` → `file_path`, …).
fn normalize(input: &Value) -> Value {
    let mut v = input.clone();
    if let Some(o) = v.as_object_mut() {
        for (from, to) in [("filePath", "file_path"), ("oldString", "old_string"), ("newString", "new_string")] {
            if let Some(x) = o.get(from).cloned()
                && !o.contains_key(to)
            {
                o.insert(to.to_string(), x);
            }
        }
    }
    v
}

/// `bash` → `Bash`, so edits, writes and shell calls get the same treatment as Claude Code's.
fn title_case(name: &str) -> String {
    let mut c = name.chars();
    c.next().map(|f| f.to_uppercase().chain(c).collect()).unwrap_or_default()
}

/// Tool calls whose output would only repeat files or listings.
fn quiet(tool: &str) -> bool {
    matches!(tool, "read" | "list" | "glob" | "ls" | "todoread" | "todowrite")
}

fn text_parts(parts: &[Value], kind: &str) -> String {
    parts
        .iter()
        .filter(|p| p["type"] == kind && p["synthetic"] != true && p["ignored"] != true)
        .filter_map(|p| p["text"].as_str())
        .collect::<Vec<_>>()
        .join("\n\n")
}

struct Row {
    id: String,
    directory: String,
    title: String,
    updated: i64,
    output: u64,
}

impl OpenCode {
    /// One session: its messages and parts, in order.
    fn session(&self, c: &Connection, r: &Row) -> Option<Session> {
        let mut parts: HashMap<String, Vec<Value>> = HashMap::new();
        let mut st = c.prepare_cached("SELECT message_id, data FROM part WHERE session_id = ?1 ORDER BY id").ok()?;
        let rows = st.query_map([&r.id], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))).ok()?;
        for (mid, data) in rows.flatten() {
            if let Ok(v) = serde_json::from_str::<Value>(&data) {
                parts.entry(mid).or_default().push(v);
            }
        }
        let mut st = c.prepare_cached("SELECT id, data FROM message WHERE session_id = ?1 ORDER BY time_created, id").ok()?;
        let msgs: Vec<(String, Value)> = st
            .query_map([&r.id], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)))
            .ok()?
            .flatten()
            .filter_map(|(id, d)| serde_json::from_str(&d).ok().map(|v| (id, v)))
            .collect();

        let mut s = Session::new("opencode");
        s.id = r.id.clone();
        s.cwd = r.directory.clone();
        s.project = base_name(&r.directory);
        s.out = r.output;
        let mut lines = Lines::default();
        let mut first_prompt = String::new();
        let mut state = State::Idle;
        let none = Vec::new();
        for (mid, m) in &msgs {
            let ps = parts.get(mid).unwrap_or(&none);
            let created = m["time"]["created"].as_i64().unwrap_or(0);
            s.log_time = s.log_time.max(m["time"]["completed"].as_i64().unwrap_or(created));
            match m["role"].as_str() {
                Some("user") => {
                    let text = text_parts(ps, "text");
                    let text = text.trim();
                    // Slash commands (`/exit`, `/init`) are not prompts.
                    if text.is_empty() || (text.starts_with('/') && !text.contains(' ')) {
                        continue;
                    }
                    if first_prompt.is_empty() {
                        first_prompt = first_line(text);
                    }
                    lines.push(Line::user(prose(text)).with_tool_id(&format!("m:{mid}")));
                    s.ask = text.to_string();
                    s.turn_start = Some(created / 1000);
                    s.activity.clear();
                    state = State::Working;
                }
                Some("assistant") => {
                    if let Some(model) = m["modelID"].as_str() {
                        s.model = model.to_string();
                    }
                    let t = &m["tokens"];
                    let n = |v: &Value| v.as_u64().unwrap_or(0);
                    let ctx = n(&t["total"]).max(n(&t["input"]) + n(&t["cache"]["read"]) + n(&t["cache"]["write"]) + n(&t["output"]));
                    if ctx > 0 {
                        s.ctx = ctx;
                    }
                    let mut last_text = String::new();
                    for p in ps {
                        match p["type"].as_str() {
                            Some("text") if p["synthetic"] != true => {
                                let t = p["text"].as_str().unwrap_or_default().trim();
                                if !t.is_empty() {
                                    lines.push(Line::text(markdown(t)).with_tool_id(&format!("m:{mid}")));
                                    last_text = t.to_string();
                                }
                            }
                            Some("reasoning") if s.activity.is_empty() => s.activity = "Thinking".into(),
                            Some("tool") => {
                                let tool = p["tool"].as_str().unwrap_or("tool");
                                let ts = &p["state"];
                                let input = normalize(&ts["input"]);
                                let name = title_case(tool);
                                let line = tool_line(&name, &input);
                                let mut d = tool_detail(&name, &input);
                                if d.is_empty() && !quiet(tool) {
                                    d = detail(ts["output"].as_str().or(ts["error"].as_str()).unwrap_or_default());
                                }
                                lines.push(Line::tool(line.clone()).with_tool_id(p["callID"].as_str().unwrap_or_default()).with_detail(d).with_file(&input));
                                match ts["status"].as_str() {
                                    Some("pending" | "running") => {
                                        s.activity = line;
                                        // The question tool waits for an answer in the terminal.
                                        if tool == "question" {
                                            s.prompt = input["questions"][0]["question"].as_str().unwrap_or("Question").to_string();
                                            state = State::Question;
                                        } else {
                                            state = State::Working;
                                        }
                                    }
                                    _ => {}
                                }
                            }
                            _ => {}
                        }
                    }
                    if !m["error"].is_null() {
                        s.summary = m["error"]["data"]["message"].as_str().or(m["error"]["name"].as_str()).unwrap_or("Error").to_string();
                        s.activity.clear();
                        state = if m["error"]["name"] == "MessageAbortedError" { State::Idle } else { State::Error };
                    } else if m["time"]["completed"].is_null() {
                        if state != State::Question {
                            state = State::Working;
                        }
                    } else if m["finish"] == "tool-calls" {
                        state = State::Working;
                    } else {
                        if !last_text.is_empty() {
                            s.summary = strip_markdown(&last_text);
                        }
                        s.activity.clear();
                        state = State::Done;
                    }
                }
                _ => {}
            }
        }
        if first_prompt.is_empty() && lines.to_vec().is_empty() {
            return None;
        }
        // opencode names sessions "New session - <date>" until it titles them.
        s.title = if r.title.starts_with("New session") || r.title.is_empty() { first_prompt } else { r.title.clone() };
        s.state = state;
        s.lines = lines.to_vec();
        s.ctx_max = self.windows.get(&s.model).copied().unwrap_or(0);
        s.model = short_model(s.model.rsplit('/').next().unwrap_or(&s.model));
        let _ = r.updated;
        Some(s)
    }
}

impl Provider for OpenCode {
    fn logs(&self, _since: SystemTime) -> Vec<PathBuf> {
        Vec::new()
    }

    fn parser(&self, _path: &std::path::Path) -> Box<dyn super::LogParser> {
        unreachable!("opencode sessions are read from its database")
    }

    fn refresh(&mut self) {
        // Context windows: `provider.<id>.models.<model>.limit.context` in opencode.json.
        let m = mtime(&self.config);
        if m == self.config_mtime {
            return;
        }
        self.config_mtime = m;
        self.windows.clear();
        if let Some(v) = fs::read_to_string(&self.config).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) {
            for (_, p) in v["provider"].as_object().into_iter().flatten() {
                for (id, model) in p["models"].as_object().into_iter().flatten() {
                    if let Some(w) = model["limit"]["context"].as_u64() {
                        self.windows.insert(id.clone(), w);
                    }
                }
            }
        }
    }

    fn sessions(&mut self, since: SystemTime) -> Vec<(Session, i64)> {
        let since_ms = unix_ms(since);
        let Some(c) = self.conn() else { return Vec::new() };
        let rows: Vec<Row> = (|| {
            let mut st = c
                .prepare_cached(
                    "SELECT id, directory, title, time_updated, tokens_output FROM session \
                     WHERE time_updated >= ?1 AND parent_id IS NULL AND time_archived IS NULL \
                     ORDER BY time_updated DESC LIMIT ?2",
                )
                .ok()?;
            let rows = st
                .query_map(rusqlite::params![since_ms, MAX_READ as i64], |r| {
                    Ok(Row { id: r.get(0)?, directory: r.get(1)?, title: r.get(2)?, updated: r.get(3)?, output: r.get::<_, i64>(4)? as u64 })
                })
                .ok()?;
            Some(rows.flatten().collect())
        })()
        .unwrap_or_default();
        // The connection borrow ends above; sessions are built with a fresh one per poll.
        let Some(c) = self.conn.take() else { return Vec::new() };
        let out = rows.iter().filter_map(|r| self.session(&c, r).map(|s| (s, r.updated))).collect();
        self.conn = Some(c);
        out
    }
}

/// Full text for the display's popup: a tool call (by callID) with its output, a message
/// (`m:<message id>`), or with an empty `tool` the last reply.
pub fn full_text(id: &str, tool: &str) -> Option<String> {
    let c = Connection::open_with_flags(data_dir().join("opencode.db"), OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    let parts = |sql: &str, arg: &str| -> Vec<Value> {
        let Ok(mut st) = c.prepare(sql) else { return Vec::new() };
        st.query_map([id, arg], |r| r.get::<_, String>(0))
            .map(|rows| rows.flatten().filter_map(|d| serde_json::from_str(&d).ok()).collect())
            .unwrap_or_default()
    };
    if let Some(mid) = tool.strip_prefix("m:") {
        let ps = parts("SELECT data FROM part WHERE session_id = ?1 AND message_id = ?2 ORDER BY id", mid);
        return Some(markdown(&text_parts(&ps, "text")));
    }
    if tool.is_empty() {
        let ps = parts(
            "SELECT p.data FROM part p JOIN message m ON m.id = p.message_id WHERE p.session_id = ?1 AND ?2 = '' \
             AND json_extract(m.data, '$.role') = 'assistant' AND json_extract(p.data, '$.type') = 'text' ORDER BY p.id DESC LIMIT 1",
            "",
        );
        return ps.first().and_then(|p| p["text"].as_str()).map(markdown);
    }
    let ps = parts("SELECT data FROM part WHERE session_id = ?1 AND json_extract(data, '$.callID') = ?2", tool);
    let p = ps.first()?;
    let st = &p["state"];
    let out = st["output"].as_str().or(st["error"].as_str()).unwrap_or_default();
    Some(full_tool(&title_case(p["tool"].as_str().unwrap_or_default()), &normalize(&st["input"]), out))
}
