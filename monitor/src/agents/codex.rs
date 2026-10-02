//! Codex: `$CODEX_HOME/sessions/YYYY/MM/DD/rollout-*.jsonl`, `{timestamp, type, payload}` records.
//!
//! Recent Codex versions no longer write `event_msg/user_message`: the prompt arrives as an
//! `item_completed` UserMessage (and a `response_item` user message), mixed with injected context
//! (`<environment_context>`, AGENTS.md, plugin lists) that must not be taken for a prompt. Thread
//! names live in `session_index.jsonl`.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::Value;

use super::{
    Line, Lines, LogParser, Provider, Session, State, base_name, detail, first_line, home, humanize, markdown, mtime, prose, shell_words,
    strip_markdown, tool_line, ts_ms,
};

/// Day folders searched: a session keeps writing to the folder of the day it started.
const DAYS: i64 = 7;

pub struct Codex {
    root: PathBuf,
    index: PathBuf,
    index_mtime: Option<SystemTime>,
    names: HashMap<String, String>,
}

impl Default for Codex {
    fn default() -> Self {
        Self::new()
    }
}

impl Codex {
    pub fn new() -> Self {
        let root = std::env::var_os("CODEX_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".codex"));
        Self::with_root(root)
    }

    pub fn with_root(root: PathBuf) -> Self {
        Codex { index: root.join("session_index.jsonl"), root: root.join("sessions"), index_mtime: None, names: HashMap::new() }
    }
}

/// Placeholder names Codex gives a thread before it has a real one.
fn generic_name(n: &str) -> bool {
    n.is_empty() || n.starts_with("New ")
}

impl Provider for Codex {
    fn logs(&self, since: SystemTime) -> Vec<PathBuf> {
        let today = jiff::Zoned::now().date();
        let mut out = Vec::new();
        for back in 0..DAYS {
            let Ok(day) = today.checked_sub(jiff::Span::new().days(back)) else { continue };
            let dir = self.root.join(day.strftime("%Y/%m/%d").to_string());
            for f in fs::read_dir(dir).into_iter().flatten().flatten() {
                let p = f.path();
                if p.extension().is_some_and(|e| e == "jsonl") && mtime(&p).is_some_and(|t| t >= since) {
                    out.push(p);
                }
            }
        }
        out
    }

    fn parser(&self, _path: &Path) -> Box<dyn LogParser> {
        Box::new(CodexLog::default())
    }

    fn refresh(&mut self) {
        let m = mtime(&self.index);
        if m == self.index_mtime {
            return;
        }
        self.index_mtime = m;
        self.names.clear();
        for line in fs::read_to_string(&self.index).unwrap_or_default().lines() {
            if let Ok(v) = serde_json::from_str::<Value>(line)
                && let (Some(id), Some(name)) = (v["id"].as_str(), v["thread_name"].as_str())
                && !generic_name(name)
            {
                self.names.insert(id.to_string(), name.to_string());
            }
        }
    }

    fn decorate(&self, s: &mut Session) {
        if let Some(name) = self.names.get(&s.id) {
            s.title = name.clone();
        }
    }
}

/// The person's words from a user message, or `None` for injected context.
fn user_prompt(text: &str) -> Option<String> {
    let t = text.trim();
    if let Some(rest) = t.strip_prefix("<realtime_delegation>") {
        // Voice sessions hand over a transcript; the end-of-session flush is not a request.
        if rest.contains("<source>transcript_tail_flush</source>") {
            return None;
        }
        let input = rest.split_once("<input>")?.1;
        let input = input.split_once("</input>").map(|(i, _)| i).unwrap_or(input);
        return Some(input.trim().to_string()).filter(|s| !s.is_empty());
    }
    if t.is_empty() || t.starts_with('<') || t.starts_with("# AGENTS.md") || t.starts_with("# Context from my IDE") {
        return None;
    }
    Some(t.to_string())
}

/// Drop Codex UI directives (`::codex-realtime-inline{}`, `visualize{"path":…}`) from agent text.
pub fn strip_directives(text: &str) -> String {
    let directive = |l: &str| {
        let l = l.trim();
        if let Some(rest) = l.strip_prefix("::") {
            return rest.contains('{') && l.ends_with('}');
        }
        match l.split_once('{') {
            Some((name, _)) => {
                !name.is_empty()
                    && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
                    && l.ends_with('}')
                    && l.contains("\":")
            }
            None => false,
        }
    };
    text.lines().filter(|l| !directive(l)).collect::<Vec<_>>().join("\n").trim().to_string()
}

/// Shell command out of a code-mode `exec` call: `tools.exec_command({cmd:"cargo test",…})`.
fn exec_cmd(js: &str) -> Option<String> {
    let start = js.find("cmd:\"").map(|i| i + 5).or_else(|| js.find("\"cmd\":\"").map(|i| i + 7))?;
    let mut out = String::new();
    let mut chars = js[start..].chars();
    while let Some(c) = chars.next() {
        match c {
            '\\' => match chars.next() {
                Some('n') => out.push('\n'),
                Some(o) => out.push(o),
                None => break,
            },
            '"' => break,
            c => out.push(c),
        }
    }
    Some(first_line(&out))
}

/// One-line description of a `response_item` tool call (the turn's current activity).
fn call_line(p: &Value) -> String {
    let name = p["name"].as_str().unwrap_or("tool");
    match p["type"].as_str() {
        Some("custom_tool_call") => {
            let input = p["input"].as_str().unwrap_or_default();
            if input.contains("apply_patch") {
                "edit".into()
            } else if let Some(cmd) = exec_cmd(input) {
                format!("shell · {cmd}")
            } else {
                name.to_string()
            }
        }
        Some("local_shell_call") => tool_line("shell", &p["action"]),
        _ => {
            let args: Value = p["arguments"].as_str().and_then(|a| serde_json::from_str(a).ok()).unwrap_or(Value::Null);
            let name = if matches!(name, "exec_command" | "shell_command") { "shell" } else { name };
            tool_line(name, &args)
        }
    }
}

fn item_cmd(item: &Value) -> String {
    item["parsed_cmd"][0]["cmd"].as_str().map(first_line).or_else(|| item["command"].as_array().map(|a| shell_words(a))).unwrap_or_default()
}

/// A file change as text: a new file's content, or an update's diff (`+` / `-` lines; with
/// `changed_only`, without the unchanged context).
fn change_text(c: &Value, changed_only: bool) -> String {
    c["unified_diff"]
        .as_str()
        .or(c["diff"].as_str())
        .map(|d| {
            d.lines()
                .filter(|l| !l.starts_with("@@") && !l.starts_with("+++") && !l.starts_with("---"))
                .filter(|l| !changed_only || l.starts_with('+') || l.starts_with('-'))
                .collect::<Vec<_>>()
                .join("\n")
        })
        .or_else(|| c["content"].as_str().map(str::to_string))
        .unwrap_or_default()
}

/// A finished item in full, for the display's popup: the command and its output, or every
/// file change.
fn item_full(item: &Value) -> Option<String> {
    match item["type"].as_str()? {
        "CommandExecution" => {
            let cmd = item["command"].as_array().map(|a| shell_words(a)).or(item["command"].as_str().map(str::to_string)).unwrap_or_default();
            Some(format!("$ {cmd}\n\n{}", item["aggregated_output"].as_str().unwrap_or_default().trim_end()))
        }
        "FileChange" => Some(
            item["changes"]
                .as_object()?
                .iter()
                .map(|(path, c)| format!("{}\n{}", base_name(path), change_text(c, false)))
                .collect::<Vec<_>>()
                .join("\n\n"),
        ),
        "AgentMessage" => Some(markdown(&strip_directives(&item_text(item)))),
        "UserMessage" => Some(item_text(item).trim().to_string()),
        _ => None,
    }
}

/// Full text for the display's popup, from the session's rollout: an item by its id (`m:` for
/// messages), or with an empty `tool` the last reply.
pub fn full_text(id: &str, tool: &str) -> Option<String> {
    let root = Codex::new().root;
    let suffix = format!("{id}.jsonl");
    // rollouts live in YYYY/MM/DD folders
    let mut stack = vec![root];
    let mut found = None;
    while let Some(d) = stack.pop() {
        for e in fs::read_dir(&d).into_iter().flatten().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.to_string_lossy().ends_with(&suffix) {
                found = Some(p);
            }
        }
        if found.is_some() {
            break;
        }
    }
    let log = fs::read_to_string(found?).ok()?;
    let item_id = tool.strip_prefix("m:").unwrap_or(tool);
    let items = log.lines().rev().filter(|l| l.contains("item_completed")).filter_map(|l| serde_json::from_str::<Value>(l).ok());
    let mut items = items.map(|d| d["payload"]["item"].clone());
    if tool.is_empty() {
        return items.find(|i| i["type"] == "AgentMessage").and_then(|i| item_full(&i));
    }
    items.find(|i| i["id"] == item_id).and_then(|i| item_full(&i))
}

/// One-line description of a finished item, with what it printed or changed.
fn item_line(item: &Value) -> Option<Line> {
    let id = item["id"].as_str().unwrap_or_default();
    match item["type"].as_str()? {
        "CommandExecution" => {
            let out = detail(item["aggregated_output"].as_str().unwrap_or_default());
            Some(Line::tool(format!("shell · {}", humanize(&item_cmd(item)))).with_tool_id(id).with_detail(out))
        }
        "FileChange" => {
            let changes = item["changes"].as_object()?;
            let files: Vec<String> = changes.keys().map(|k| base_name(k)).collect();
            // All new files: shown as code; otherwise the first change as a diff.
            let adds = changes.values().all(|c| c["type"] == "add");
            let first = changes.iter().next();
            let mut l = Line::tool(format!("{} · {}", if adds { "write" } else { "edit" }, files.join(", "))).with_tool_id(id);
            if let Some((path, c)) = first {
                l = l.with_detail(detail(&change_text(c, true))).with_file(&serde_json::json!({ "path": path }));
            }
            Some(l)
        }
        "McpToolCall" => {
            Some(Line::tool(format!("{} · {}", item["server"].as_str().unwrap_or("mcp"), item["tool"].as_str().unwrap_or("tool"))))
        }
        "ImageView" => Some(Line::tool(format!("view · {}", base_name(item["path"].as_str().unwrap_or_default())))),
        "WebSearch" => Some(Line::tool(format!("search · {}", item["query"].as_str().unwrap_or_default()))),
        "AgentMessage" => {
            let text = item_text(item);
            let text = strip_directives(&text);
            (!text.is_empty()).then(|| Line::text(markdown(&text)).with_tool_id(&format!("m:{}", item["id"].as_str().unwrap_or_default())))
        }
        _ => None,
    }
}

fn item_text(item: &Value) -> String {
    item["content"].as_array().into_iter().flatten().filter_map(|c| c["text"].as_str()).collect::<Vec<_>>().join("\n")
}

#[derive(Default)]
struct CodexLog {
    seen: bool,
    id: String,
    project: String,
    first_prompt: String,
    ask: String,
    cwd: String,
    model: String,
    state: Option<State>,
    turn_start: Option<i64>,
    lines: Lines,
    activity: String,
    prompt: String,
    summary: String,
    log_time: i64,
    ctx: u64,
    ctx_max: u64,
    out: u64,
}

impl CodexLog {
    fn mark(&mut self, d: &Value, state: State) {
        self.state = Some(state);
        if let Some(t) = ts_ms(&d["timestamp"]) {
            self.log_time = self.log_time.max(t);
        }
    }

    /// Something the person typed; `id` is its item id when known (for the full text).
    fn user(&mut self, text: &str, id: &str) {
        let Some(p) = user_prompt(text) else { return };
        if self.first_prompt.is_empty() {
            self.first_prompt = first_line(&p);
        }
        // user_message and the matching item_completed both carry the prompt: show it once.
        if self.ask != p {
            let line = Line::user(prose(&p));
            self.lines.push(if id.is_empty() { line } else { line.with_tool_id(&format!("m:{id}")) });
            self.ask = p;
        }
    }
}

impl LogParser for CodexLog {
    fn feed(&mut self, d: &Value) {
        let p = &d["payload"];
        match (d["type"].as_str(), p["type"].as_str()) {
            (Some("session_meta"), _) => {
                if let Some(id) = p["id"].as_str() {
                    self.id = id.to_string();
                }
                if let Some(cwd) = p["cwd"].as_str() {
                    self.project = base_name(cwd);
                    self.cwd = cwd.to_string();
                }
                if let Some(w) = p["context_window"].as_u64() {
                    self.ctx_max = w;
                }
            }
            (Some("turn_context"), _) => {
                if let Some(cwd) = p["cwd"].as_str() {
                    self.project = base_name(cwd);
                }
                if let Some(m) = p["model"].as_str() {
                    self.model = m.to_string();
                }
            }
            (Some("event_msg"), Some(kind)) => match kind {
                "task_started" => {
                    self.seen = true;
                    self.turn_start = p["started_at"].as_i64().or_else(|| ts_ms(&d["timestamp"]).map(|t| t / 1000));
                    if let Some(w) = p["model_context_window"].as_u64() {
                        self.ctx_max = w;
                    }
                    // History is kept across turns: the display scrolls through the conversation.
                    self.activity.clear();
                    self.mark(d, State::Working);
                }
                "user_message" => {
                    self.seen = true;
                    self.user(p["message"].as_str().unwrap_or_default(), "");
                }
                "task_complete" => {
                    self.seen = true;
                    if let Some(m) = p["last_agent_message"].as_str() {
                        self.summary = strip_markdown(&strip_directives(m));
                    }
                    self.activity.clear();
                    self.mark(d, State::Done);
                }
                "turn_aborted" => {
                    self.activity.clear();
                    self.mark(d, State::Idle);
                }
                "error" => {
                    self.summary = p["message"].as_str().unwrap_or("Codex error").to_string();
                    self.mark(d, State::Error);
                }
                "exec_approval_request" | "apply_patch_approval_request" => {
                    self.prompt = match p["command"].as_array() {
                        Some(cmd) => format!("shell · {}", shell_words(cmd)),
                        None => p["reason"].as_str().unwrap_or("Approval needed").to_string(),
                    };
                    self.mark(d, State::Permission);
                }
                "request_user_input" | "elicitation_request" => {
                    self.prompt = p["message"].as_str().or(p["questions"][0]["question"].as_str()).unwrap_or("Input needed").to_string();
                    self.mark(d, State::Question);
                }
                "token_count" => {
                    let info = &p["info"];
                    if !info.is_null() {
                        self.ctx = info["last_token_usage"]["total_tokens"].as_u64().unwrap_or(self.ctx);
                        self.out = info["total_token_usage"]["output_tokens"].as_u64().unwrap_or(self.out);
                        self.ctx_max = info["model_context_window"].as_u64().unwrap_or(self.ctx_max);
                    }
                }
                "item_completed" => {
                    let item = &p["item"];
                    if item["type"] == "UserMessage" {
                        self.seen = true;
                        self.user(&item_text(item), item["id"].as_str().unwrap_or_default());
                        return;
                    }
                    if let Some(line) = item_line(item) {
                        self.lines.push(line);
                    }
                    if self.state.is_some_and(|s| s.waiting()) {
                        self.mark(d, State::Working);
                    }
                }
                _ => {}
            },
            (Some("response_item"), Some("message")) if p["role"] == "user" => {
                let text = item_text(p);
                self.user(&text, "");
            }
            (Some("response_item"), Some("function_call" | "custom_tool_call" | "local_shell_call")) => {
                self.seen = true;
                self.activity = call_line(p);
                if !self.state.is_some_and(|s| s.waiting()) {
                    self.mark(d, State::Working);
                }
            }
            (Some("response_item"), Some("function_call_output" | "custom_tool_call_output"))
                if self.state.is_some_and(|s| s.waiting()) =>
            {
                self.mark(d, State::Working);
            }
            _ => {}
        }
    }

    fn session(&self) -> Option<Session> {
        if !self.seen {
            return None;
        }
        let mut s = Session::new("codex");
        s.id = self.id.clone();
        s.project = self.project.clone();
        s.cwd = self.cwd.clone();
        s.title = self.first_prompt.clone();
        s.ask = self.ask.clone();
        s.state = self.state.unwrap_or(State::Idle);
        s.activity = self.activity.clone();
        s.prompt = self.prompt.clone();
        s.summary = self.summary.clone();
        s.model = self.model.clone();
        s.ctx = self.ctx;
        s.ctx_max = self.ctx_max;
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

    fn run(entries: &[Value]) -> Session {
        let mut p = CodexLog::default();
        for e in entries {
            p.feed(e);
        }
        p.session().unwrap()
    }

    #[test]
    fn current_format() {
        let mut log = vec![
            json!({"type":"session_meta","payload":{"id":"t1","cwd":"/home/u/gbt","context_window":258400}}),
            json!({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"<environment_context>\n<cwd>/home/u/gbt</cwd>"}]}}),
            json!({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md instructions for /home/u/gbt"}]}}),
            json!({"type":"turn_context","payload":{"cwd":"/home/u/gbt","model":"gpt-6-astra"}}),
            json!({"type":"event_msg","timestamp":"2026-09-27T12:00:00Z","payload":{"type":"task_started","started_at":1790510400,"model_context_window":258400}}),
            json!({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"UserMessage","content":[{"type":"text","text":"add tests\nfor the parser"}]}}}),
            json!({"type":"response_item","payload":{"type":"custom_tool_call","name":"exec","input":"text(await tools.exec_command({cmd:\"cargo test\\n--all\",max_output_tokens:1000}));"}}),
        ];
        let s = run(&log);
        assert_eq!((s.state, s.project.as_str(), s.title.as_str(), s.id.as_str()), (State::Working, "gbt", "add tests", "t1"));
        assert_eq!((s.activity.as_str(), s.model.as_str(), s.ctx_max), ("shell · cargo test", "gpt-6-astra", 258400));
        assert_eq!(s.turn_start, Some(1790510400));

        log.push(json!({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"CommandExecution","command":["/usr/bin/bash","-lc","cargo test"],"parsed_cmd":[{"type":"unknown","cmd":"cargo test"}]}}}));
        log.push(json!({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"FileChange","changes":{"/home/u/gbt/src/a.rs":{}}}}}));
        log.push(json!({"type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"total_tokens":5000},"total_token_usage":{"output_tokens":700},"model_context_window":258400}}}));
        log.push(json!({"type":"event_msg","payload":{"type":"task_complete","last_agent_message":"::codex-realtime-inline{}\nAdded 3 tests.\nvisualize{\"path\":\"/x\"}"}}));
        let s = run(&log);
        assert_eq!((s.state, s.summary.as_str(), s.ctx, s.out), (State::Done, "Added 3 tests.", 5000, 700));
        assert_eq!(s.lines, vec![Line::user("add tests for the parser"), Line::tool("shell · cargo test"), Line::tool("edit · a.rs")]);
        assert_eq!(s.ask, "add tests\nfor the parser");
        assert_eq!(s.activity, "");

        log.push(json!({"type":"event_msg","payload":{"type":"task_started"}}));
        log.push(json!({"type":"event_msg","payload":{"type":"exec_approval_request","command":["bash","-lc","rm -rf target"]}}));
        let s = run(&log);
        assert_eq!((s.state, s.prompt.as_str()), (State::Permission, "shell · rm -rf target"));
        log.push(json!({"type":"event_msg","payload":{"type":"turn_aborted"}}));
        assert_eq!(run(&log).state, State::Idle);
    }

    #[test]
    fn realtime_prompts() {
        assert_eq!(
            user_prompt("<realtime_delegation>\n  <input>Make it better</input>\n</realtime_delegation>").as_deref(),
            Some("Make it better")
        );
        assert_eq!(user_prompt("<realtime_delegation><source>transcript_tail_flush</source><input>bye</input>"), None);
        assert_eq!(user_prompt("<recommended_plugins>…"), None);
        assert_eq!(user_prompt("fix it").as_deref(), Some("fix it"));
    }

    #[test]
    fn legacy_user_message() {
        let s = run(&[
            json!({"type":"event_msg","payload":{"type":"task_started"}}),
            json!({"type":"event_msg","payload":{"type":"user_message","message":"old style"}}),
            json!({"type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"ls\"]}"}}),
        ]);
        assert_eq!((s.title.as_str(), s.activity.as_str()), ("old style", "shell · ls"));
    }

    #[test]
    fn thread_names_from_index() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("session_index.jsonl"),
            "{\"id\":\"t1\",\"thread_name\":\"New chat\"}\n{\"id\":\"t1\",\"thread_name\":\"Parser tests\"}\n{\"id\":\"t2\",\"thread_name\":\"New voice chat\"}\n",
        )
        .unwrap();
        let mut c = Codex::with_root(dir.path().to_path_buf());
        c.refresh();
        let mut s = Session { id: "t1".into(), title: "add tests".into(), ..Session::new("codex") };
        c.decorate(&mut s);
        assert_eq!(s.title, "Parser tests");
        let mut s = Session { id: "t2".into(), title: "first prompt".into(), ..Session::new("codex") };
        c.decorate(&mut s);
        assert_eq!(s.title, "first prompt");
    }

    #[test]
    fn tool_details() {
        let item = json!({"type":"CommandExecution","id":"exec-1","command":["bash","-lc","cargo test"],"parsed_cmd":[{"cmd":"cargo test"}],"aggregated_output":"running 2 tests\ntest result: ok"});
        let l = item_line(&item).unwrap();
        assert_eq!((l.d.as_str(), l.tool_id.as_str()), ("running 2 tests\ntest result: ok", "exec-1"));
        assert_eq!(item_full(&item).unwrap(), "$ cargo test\n\nrunning 2 tests\ntest result: ok");
        let item = json!({"type":"FileChange","id":"exec-2","changes":{"/p/src/a.rs":{"type":"update","unified_diff":"@@ -1,2 +1,2 @@\n fn a() {}\n-let x = 1;\n+let x = 2;"}}});
        let l = item_line(&item).unwrap();
        assert_eq!((l.s.as_str(), l.d.as_str(), l.f.as_str()), ("edit · a.rs", "-let x = 1;\n+let x = 2;", "a.rs"));
        let item = json!({"type":"FileChange","id":"exec-3","changes":{"/p/new.py":{"type":"add","content":"print(1)\n"}}});
        assert_eq!(item_line(&item).unwrap().s, "write · new.py");
    }
}
