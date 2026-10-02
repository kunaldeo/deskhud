//! pi and oh-my-pi (`omp`, a pi fork with the same log format):
//! `~/.pi/agent/sessions/<cwd-slug>/<timestamp>_<id>.jsonl` (`~/.omp/agent/…` for omp). A `session`
//! header with the working directory, then `message` entries (`user` / `assistant` /
//! `toolResult`) whose `stopReason` tells how an assistant turn ended.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::Value;

use super::{
    Line, Lines, LogParser, Provider, Session, State, base_name, first_line, full_tool, home, markdown, mtime, prose, result_text,
    strip_markdown, tool_detail, tool_line,
};

pub struct Pi {
    agent: &'static str,
    root: PathBuf,
    models: Vec<PathBuf>,
    models_mtime: Vec<Option<SystemTime>>,
    windows: HashMap<String, u64>,
}

impl Default for Pi {
    fn default() -> Self {
        Self::new()
    }
}

impl Pi {
    pub fn new() -> Self {
        let dir = std::env::var_os("PI_CODING_AGENT_DIR").map(PathBuf::from).unwrap_or_else(|| home().join(".pi/agent"));
        Self::with_root(dir)
    }

    /// oh-my-pi: the same logs under `~/.omp/agent`.
    pub fn omp() -> Self {
        Pi { agent: "omp", ..Self::with_root(home().join(".omp/agent")) }
    }

    pub fn with_root(dir: PathBuf) -> Self {
        Pi {
            agent: "pi",
            root: dir.join("sessions"),
            models: vec![dir.join("models.json"), dir.join("models-store.json")],
            models_mtime: Vec::new(),
            windows: HashMap::new(),
        }
    }
}

/// Every `{id, contextWindow}` object anywhere in a models file.
fn collect_windows(v: &Value, out: &mut HashMap<String, u64>) {
    match v {
        Value::Object(o) => {
            if let (Some(id), Some(w)) = (o.get("id").and_then(Value::as_str), o.get("contextWindow").and_then(Value::as_u64)) {
                out.insert(id.to_string(), w);
            }
            o.values().for_each(|c| collect_windows(c, out));
        }
        Value::Array(a) => a.iter().for_each(|c| collect_windows(c, out)),
        _ => {}
    }
}

impl Provider for Pi {
    fn logs(&self, since: SystemTime) -> Vec<PathBuf> {
        let mut out = Vec::new();
        for dir in fs::read_dir(&self.root).into_iter().flatten().flatten() {
            for f in fs::read_dir(dir.path()).into_iter().flatten().flatten() {
                let p = f.path();
                if p.extension().is_some_and(|e| e == "jsonl") && mtime(&p).is_some_and(|t| t >= since) {
                    out.push(p);
                }
            }
        }
        out
    }

    fn parser(&self, _path: &Path) -> Box<dyn LogParser> {
        Box::new(PiLog { agent: self.agent, ..Default::default() })
    }

    fn refresh(&mut self) {
        let m: Vec<_> = self.models.iter().map(|p| mtime(p)).collect();
        if m == self.models_mtime {
            return;
        }
        self.models_mtime = m;
        self.windows.clear();
        for p in &self.models {
            if let Some(v) = fs::read_to_string(p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) {
                collect_windows(&v, &mut self.windows);
            }
        }
    }

    fn decorate(&self, s: &mut Session) {
        if s.ctx_max == 0 {
            s.ctx_max = self.windows.get(&s.model).copied().unwrap_or(0);
        }
        // Unknown models named for their window ("k3-256k"): take it from the name.
        if s.ctx_max == 0
            && let Some(k) = s.model.rsplit('-').next().and_then(|t| t.strip_suffix('k')).and_then(|n| n.parse::<u64>().ok())
        {
            s.ctx_max = k * 1000;
        }
    }
}

#[derive(Default)]
struct PiLog {
    agent: &'static str,
    seen: bool,
    /// omp recorded the session ending: late tool results don't make it "working" again.
    exited: bool,
    id: String,
    project: String,
    title: String,
    model: String,
    state: Option<State>,
    turn_start: Option<i64>,
    lines: Lines,
    activity: String,
    summary: String,
    ask: String,
    cwd: String,
    log_time: i64,
    ctx: u64,
    out: u64,
}

/// A message line's id for the display: `m:` + the log entry's id.
fn msg_id(d: &Value) -> String {
    d["id"].as_str().map(|u| format!("m:{u}")).unwrap_or_default()
}

/// pi's tool arguments under the names the shared helpers know (`path` → `file_path`, …).
fn normalize_args(a: &Value) -> Value {
    let mut v = a.clone();
    if let Some(o) = v.as_object_mut() {
        // omp: `i` is the call's one-line intent, like Claude Code's `description`.
        for (from, to) in [
            ("path", "file_path"),
            ("oldText", "old_string"),
            ("newText", "new_string"),
            ("old_text", "old_string"),
            ("new_text", "new_string"),
            ("i", "description"),
        ] {
            if let Some(x) = o.get(from).cloned()
                && !o.contains_key(to)
            {
                o.insert(to.to_string(), x);
            }
        }
    }
    v
}

/// omp's hashline edits: `input` is `[path#hash]` then `PUT n:` / `+` / `-` lines. The file and the
/// changed lines.
fn omp_patch(args: &Value) -> Option<(String, String)> {
    let input = args["input"].as_str()?;
    let first = input.lines().next()?;
    let path = first.strip_prefix('[')?.split(['#', ']']).next()?.to_string();
    let body = input.lines().skip(1).filter(|l| !l.trim().is_empty()).collect::<Vec<_>>().join("\n");
    Some((path, body))
}

/// What a call changed, for its line: omp's patch, else the shared edit / write detail.
fn call_detail(name: &str, args: &Value) -> (String, Value) {
    if let Some((path, body)) = omp_patch(args) {
        return (super::detail(&body), serde_json::json!({ "path": path }));
    }
    // Only file tools name a file (a glob's `path` is a folder, its pattern no file).
    let file = if matches!(name, "Read" | "Edit" | "Write" | "NotebookEdit") { args.clone() } else { Value::Null };
    (tool_detail(name, args), file)
}

/// `edit` → `Edit`, so the shared helpers treat the call like Claude Code's.
fn title_case(name: &str) -> String {
    let mut c = name.chars();
    c.next().map(|f| f.to_uppercase().chain(c).collect()).unwrap_or_default()
}

/// Full text for the display's popup, from the session's log under `root`: a tool call (by id)
/// with its output, a message (`m:<entry id>`), or with an empty `tool` the last reply.
pub fn full_text(root: &Path, id: &str, tool: &str) -> Option<String> {
    let suffix = format!("_{id}.jsonl");
    let log = fs::read_dir(root.join("sessions"))
        .ok()?
        .flatten()
        .flat_map(|d| fs::read_dir(d.path()).into_iter().flatten().flatten())
        .map(|f| f.path())
        .find(|p| p.to_string_lossy().ends_with(&suffix))?;
    let log = fs::read_to_string(log).ok()?;
    let text_of = |m: &Value| -> String {
        match &m["content"] {
            Value::String(s) => s.clone(),
            c => c.as_array().into_iter().flatten().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n\n"),
        }
    };
    if tool.is_empty() {
        return log.lines().rev().filter_map(|l| serde_json::from_str::<Value>(l).ok()).find_map(|d| {
            let m = &d["message"];
            (m["role"] == "assistant").then(|| markdown(&text_of(m))).filter(|t| !t.is_empty())
        });
    }
    if let Some(eid) = tool.strip_prefix("m:") {
        let d: Value = log.lines().filter(|l| l.contains(eid)).find_map(|l| serde_json::from_str::<Value>(l).ok().filter(|d| d["id"] == eid))?;
        return Some(markdown(&text_of(&d["message"])));
    }
    let (mut name, mut args, mut output) = (String::new(), Value::Null, String::new());
    for l in log.lines().filter(|l| l.contains(tool)) {
        let Ok(d) = serde_json::from_str::<Value>(l) else { continue };
        let m = &d["message"];
        if m["role"] == "toolResult" && m["toolCallId"] == tool {
            output = result_text(&m["content"]);
        }
        for b in m["content"].as_array().into_iter().flatten() {
            if b["type"] == "toolCall" && b["id"] == tool {
                name = title_case(b["name"].as_str().unwrap_or_default());
                args = normalize_args(&b["arguments"]);
            }
        }
    }
    if let Some((path, body)) = omp_patch(&args) {
        return Some(format!("{}\n{body}", base_name(&path)));
    }
    (!name.is_empty()).then(|| full_tool(&name, &args, &output))
}

fn msg_time(d: &Value) -> Option<i64> {
    d["message"]["timestamp"].as_i64().or_else(|| super::ts_ms(&d["timestamp"]))
}

impl PiLog {
    fn mark(&mut self, d: &Value, state: State) {
        self.state = Some(state);
        if let Some(t) = msg_time(d) {
            self.log_time = self.log_time.max(t);
        }
    }
}

impl LogParser for PiLog {
    fn feed(&mut self, d: &Value) {
        match d["type"].as_str() {
            Some("session") => {
                if let Some(cwd) = d["cwd"].as_str() {
                    self.project = base_name(cwd);
                    self.cwd = cwd.to_string();
                }
                if let Some(id) = d["id"].as_str() {
                    self.id = id.to_string();
                }
            }
            Some("model_change") => {
                if let Some(m) = d["modelId"].as_str() {
                    self.model = m.to_string();
                }
            }
            // omp: auto titles, and a record of the session ending (so it stops "working").
            Some("title" | "title_change") => {
                if let Some(t) = d["title"].as_str().filter(|t| !t.is_empty()) {
                    self.title = t.to_string();
                }
            }
            Some("custom") if d["customType"] == "session_exit" => {
                self.exited = true;
                self.activity.clear();
                if self.state == Some(State::Working) {
                    self.state = Some(State::Idle);
                }
            }
            Some("session_info") => {
                if let Some(n) = d["name"].as_str().filter(|n| !n.is_empty()) {
                    self.title = n.to_string();
                }
            }
            Some("message") => {
                let m = &d["message"];
                match m["role"].as_str() {
                    Some("user") => {
                        let text: String = match &m["content"] {
                            Value::String(s) => s.clone(),
                            c => c.as_array().into_iter().flatten().filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n"),
                        };
                        if text.trim().is_empty() {
                            return;
                        }
                        self.seen = true;
                        self.exited = false;  // resumed
                        if self.title.is_empty() {
                            self.title = first_line(&text);
                        }
                        self.turn_start = msg_time(d).map(|t| t / 1000);
                        // History is kept across turns: the display scrolls through the conversation.
                        self.activity.clear();
                        self.lines.push(Line::user(prose(&text)).with_tool_id(&msg_id(d)));
                        self.ask = text.trim().to_string();
                        self.mark(d, State::Working);
                    }
                    Some("assistant") => {
                        self.seen = true;
                        if let Some(model) = m["model"].as_str() {
                            self.model = model.to_string();
                        }
                        if let Some(u) = m["usage"].as_object() {
                            let n = |k: &str| u.get(k).and_then(Value::as_u64).unwrap_or(0);
                            self.ctx = n("totalTokens").max(n("input") + n("cacheRead") + n("cacheWrite") + n("output"));
                            self.out += n("output");
                        }
                        let mut last_text = String::new();
                        for b in m["content"].as_array().into_iter().flatten() {
                            match b["type"].as_str() {
                                Some("text") => {
                                    let t = b["text"].as_str().unwrap_or_default().trim();
                                    if !t.is_empty() {
                                        self.lines.push(Line::text(markdown(t)).with_tool_id(&msg_id(d)));
                                        last_text = t.to_string();
                                    }
                                }
                                Some("toolCall") => {
                                    let name = title_case(b["name"].as_str().unwrap_or("tool"));
                                    let args = normalize_args(&b["arguments"]);
                                    let line = tool_line(&name, &args);
                                    let id = b["id"].as_str().unwrap_or_default();
                                    let (d, file) = call_detail(&name, &args);
                                    self.lines.push(Line::tool(line.clone()).with_tool_id(id).with_detail(d).with_file(&file));
                                    self.activity = line;
                                }
                                Some("thinking") if self.activity.is_empty() => self.activity = "Thinking".into(),
                                _ => {}
                            }
                        }
                        match m["stopReason"].as_str() {
                            Some("stop" | "length") => {
                                if !last_text.is_empty() {
                                    self.summary = strip_markdown(&last_text);
                                }
                                self.activity.clear();
                                self.mark(d, State::Done);
                            }
                            Some("error") => {
                                self.summary = m["errorMessage"].as_str().unwrap_or("Error").to_string();
                                self.mark(d, State::Error);
                            }
                            Some("aborted") => {
                                self.activity.clear();
                                self.mark(d, State::Idle);
                            }
                            _ => self.mark(d, State::Working),
                        }
                    }
                    Some("toolResult") => {
                        // Output under its call; file reads would only repeat the file.
                        if !matches!(m["toolName"].as_str(), Some("read" | "ls" | "find")) {
                            self.lines.set_output(m["toolCallId"].as_str().unwrap_or_default(), &result_text(&m["content"]));
                        }
                        if !self.exited {
                            self.mark(d, State::Working);
                        }
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }

    fn session(&self) -> Option<Session> {
        if !self.seen {
            return None;
        }
        let mut s = Session::new(if self.agent.is_empty() { "pi" } else { self.agent });
        s.id = self.id.clone();
        s.project = self.project.clone();
        s.cwd = self.cwd.clone();
        s.title = self.title.clone();
        s.state = self.state.unwrap_or(State::Idle);
        s.activity = self.activity.clone();
        s.summary = self.summary.clone();
        s.ask = self.ask.clone();
        s.model = self.model.clone();
        s.ctx = self.ctx;
        s.out = self.out;
        s.turn_start = self.turn_start;
        s.lines = self.lines.to_vec();
        s.log_time = self.log_time;
        Some(s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn turns() {
        let mut p = PiLog::default();
        let log = [
            json!({"type":"session","version":3,"id":"p1","timestamp":"2026-09-27T06:23:47.313Z","cwd":"/home/u/snake"}),
            json!({"type":"model_change","modelId":"k3-256k"}),
            json!({"type":"message","message":{"role":"user","content":[{"type":"text","text":"decrypt the firmware"}],"timestamp":1790526800000i64}}),
            json!({"type":"message","message":{"role":"assistant","model":"k3-256k","usage":{"input":230,"output":349,"cacheRead":90368,"cacheWrite":0,"totalTokens":90947},"stopReason":"toolUse",
                "content":[{"type":"thinking","thinking":"…"},{"type":"toolCall","name":"bash","arguments":{"command":"ls -la ~/Downloads/"}}],"timestamp":1790526828409i64}}),
            json!({"type":"message","message":{"role":"toolResult","content":[{"type":"text","text":"…"}],"timestamp":1790526829000i64}}),
        ];
        for e in &log {
            p.feed(e);
        }
        let s = p.session().unwrap();
        assert_eq!((s.state, s.project.as_str(), s.title.as_str(), s.id.as_str()), (State::Working, "snake", "decrypt the firmware", "p1"));
        assert_eq!((s.activity.as_str(), s.ctx, s.out, s.turn_start), ("Bash · ls -la ~/Downloads/", 90947, 349, Some(1790526800)));
        p.feed(&json!({"type":"message","message":{"role":"assistant","usage":{"output":10,"totalTokens":91000},"stopReason":"stop","content":[{"type":"text","text":"Decrypted."}],"timestamp":1790526840249i64}}));
        let s = p.session().unwrap();
        assert_eq!((s.state, s.summary.as_str(), s.out, s.log_time), (State::Done, "Decrypted.", 359, 1790526840249));
        p.feed(&json!({"type":"message","message":{"role":"user","content":"more"}}));
        p.feed(&json!({"type":"message","message":{"role":"assistant","stopReason":"aborted","content":[]}}));
        assert_eq!(p.session().unwrap().state, State::Idle);
    }

    #[test]
    fn context_windows() {
        let mut w = HashMap::new();
        collect_windows(&json!({"providers":{"x":{"models":[{"id":"k3-256k","contextWindow":262144}]}}}), &mut w);
        assert_eq!(w.get("k3-256k"), Some(&262144));
    }

    #[test]
    fn tool_output_and_omp() {
        let mut p = PiLog { agent: "omp", ..Default::default() };
        for e in [
            json!({"type":"session","id":"o1","cwd":"/home/u/app"}),
            json!({"type":"message","id":"e1","message":{"role":"user","content":"fix it"}}),
            json!({"type":"message","id":"e2","message":{"role":"assistant","content":[{"type":"toolCall","id":"c1","name":"bash","arguments":{"command":"make"}},{"type":"toolCall","id":"c2","name":"edit","arguments":{"path":"/home/u/app/m.c","oldText":"a","newText":"b"}}],"stopReason":"toolUse"}}),
            json!({"type":"message","id":"e3","message":{"role":"toolResult","toolCallId":"c1","toolName":"bash","content":[{"type":"text","text":"ok"}]}}),
        ] {
            p.feed(&e);
        }
        let s = p.session().unwrap();
        assert_eq!(s.agent, "omp");
        let tools: Vec<_> = s.lines.iter().filter(|l| l.k == "tool").map(|l| (l.d.as_str(), l.f.as_str(), l.tool_id.as_str())).collect();
        assert_eq!(tools, vec![("ok", "", "c1"), ("- a\n+ b", "m.c", "c2")]);
    }
}
