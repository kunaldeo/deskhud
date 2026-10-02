//! Claude Code: `~/.claude/projects/<project>/<session>.jsonl`, one entry per message block.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use serde_json::Value;

use super::{
    Line, Lines, LogParser, markdown, Provider, Session, State, base_name, first_line, home, mtime, prose, question_text, short_model, strip_markdown,
    full_tool, result_text, tool_detail, tool_line, ts_ms,
};

pub struct ClaudeCode {
    root: PathBuf,
}

impl Default for ClaudeCode {
    fn default() -> Self {
        Self::new()
    }
}

impl ClaudeCode {
    pub fn new() -> Self {
        let dir = std::env::var_os("CLAUDE_CONFIG_DIR").map(PathBuf::from).unwrap_or_else(|| home().join(".claude"));
        Self::with_root(dir.join("projects"))
    }

    pub fn with_root(root: PathBuf) -> Self {
        Self { root }
    }
}

impl Provider for ClaudeCode {
    fn logs(&self, since: SystemTime) -> Vec<PathBuf> {
        let mut out = Vec::new();
        // Appending to a log does not touch its directory's mtime, so every project is listed.
        // Subagent transcripts live in subdirectories and are not sessions of their own.
        for project in fs::read_dir(&self.root).into_iter().flatten().flatten() {
            for f in fs::read_dir(project.path()).into_iter().flatten().flatten() {
                let p = f.path();
                if p.extension().is_some_and(|e| e == "jsonl") && mtime(&p).is_some_and(|t| t >= since) {
                    out.push(p);
                }
            }
        }
        out
    }

    fn parser(&self, _path: &Path) -> Box<dyn LogParser> {
        Box::new(ClaudeLog::default())
    }
}

/// A message line's id for the display: `m:` + its log entry's uuid ("" without one).
fn msg_id(d: &Value) -> String {
    d["uuid"].as_str().map(|u| format!("m:{u}")).unwrap_or_default()
}

/// The log of session `id` (its file name), in any project.
fn log_path(id: &str) -> Option<PathBuf> {
    let root = ClaudeCode::new().root;
    fs::read_dir(&root).ok()?.flatten().map(|p| p.path().join(format!("{id}.jsonl"))).find(|p| p.is_file())
}

/// Full text for the display's popup: a tool call (by tool_use id) with its output, or with an
/// empty `tool` the last reply. Only lines that can match are parsed, so big logs stay quick.
pub fn full_text(id: &str, tool: &str) -> Option<String> {
    let log = fs::read_to_string(log_path(id)?).ok()?;
    if tool.is_empty() {
        return log.lines().rev().filter(|l| l.contains("\"assistant\"") && l.contains("\"text\"")).find_map(|l| {
            let d: Value = serde_json::from_str(l).ok()?;
            let text: Vec<&str> = d["message"]["content"].as_array()?.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect();
            (!text.is_empty()).then(|| markdown(&text.join("\n\n")))
        });
    }
    // A message (`m:<uuid>`): the person's text, or every text block of the agent's reply.
    if let Some(uuid) = tool.strip_prefix("m:") {
        let d: Value = log.lines().filter(|l| l.contains(uuid)).find_map(|l| serde_json::from_str::<Value>(l).ok().filter(|d| d["uuid"] == uuid))?;
        if let Some(p) = d["attachment"]["prompt"].as_str() {
            return Some(p.trim().to_string());
        }
        if d["type"] == "user" {
            return typed_prompt(&d);
        }
        let text: Vec<&str> = d["message"]["content"].as_array()?.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect();
        return Some(markdown(&text.join("\n\n")));
    }
    let (mut name, mut input, mut output) = (String::new(), Value::Null, String::new());
    for l in log.lines().filter(|l| l.contains(tool)) {
        let Ok(d) = serde_json::from_str::<Value>(l) else { continue };
        for b in d["message"]["content"].as_array().into_iter().flatten() {
            if b["type"] == "tool_use" && b["id"] == tool {
                name = b["name"].as_str().unwrap_or_default().to_string();
                input = b["input"].clone();
            } else if b["type"] == "tool_result" && b["tool_use_id"] == tool {
                output = result_text(&b["content"]);
            }
        }
    }
    (!name.is_empty()).then(|| full_tool(&name, &input, &output))
}

/// Context window: not logged. The 5-series models and `[1m]` variants run with 1M, and a
/// context seen above 200k settles it for any model.
fn window(model: &str, peak: u64) -> u64 {
    let long = ["opus-5", "sonnet-5", "fable-5", "[1m]"].iter().any(|m| model.contains(m));
    if long || peak > 200_000 { 1_000_000 } else { 200_000 }
}

/// Text the person typed (not a tool result, command wrapper or injected skill text).
fn typed_prompt(d: &Value) -> Option<String> {
    if d["isMeta"] == true || d.get("toolUseResult").is_some() || d["isCompactSummary"] == true {
        return None;
    }
    let text: String = match &d["message"]["content"] {
        Value::String(s) => s.clone(),
        Value::Array(blocks) => {
            blocks.iter().filter(|b| b["type"] == "text").filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n")
        }
        _ => return None,
    };
    let t = text.trim();
    (!t.is_empty() && !t.starts_with('<') && !t.starts_with("Base directory for this skill") && !t.starts_with("Caveat:"))
        .then(|| t.to_string())
}

#[derive(Default)]
struct ClaudeLog {
    seen: bool,
    project: String,
    ai_title: String,
    first_prompt: String,
    ask: String,
    cwd: String,
    state: Option<State>,
    turn_start: Option<i64>,
    lines: Lines,
    /// tool_use ids whose output isn't worth showing (file reads, listings).
    reads: std::collections::HashSet<String>,
    activity: String,
    activity_id: String,
    prompt: String,
    summary: String,
    log_time: i64,
    // usage
    last_message: String,
    model: String,
    context: u64,
    peak: u64,
    output: u64,
}

impl ClaudeLog {
    /// Something the person typed; `d` is its log entry, whose uuid lets the display fetch it in full.
    fn say(&mut self, text: &str, d: &Value) {
        let text = text.trim();
        if text.is_empty() || text.starts_with('<') {
            return;
        }
        self.ask = text.to_string();
        self.lines.push(Line::user(prose(text)).with_tool_id(&msg_id(d)));
    }
}

impl ClaudeLog {
    fn usage(&mut self, d: &Value) {
        if d["subtype"] == "compact_boundary" {
            let m = &d["compactMetadata"];
            self.peak = self.peak.max(m["preTokens"].as_u64().unwrap_or(0));
            self.context = m["postTokens"].as_u64().unwrap_or(self.context);
            return;
        }
        let msg = &d["message"];
        if msg["usage"].is_null() {
            return;
        }
        let n = |k: &str| msg["usage"][k].as_u64().unwrap_or(0);
        // Every block of one message repeats the message's usage: count it once.
        let id = msg["id"].as_str().unwrap_or_default();
        if id.is_empty() || id != self.last_message {
            self.output += n("output_tokens");
            self.last_message = id.to_string();
        }
        self.context = n("input_tokens") + n("cache_creation_input_tokens") + n("cache_read_input_tokens") + n("output_tokens");
        self.peak = self.peak.max(self.context);
        if let Some(m) = msg["model"].as_str().filter(|m| m.starts_with("claude")) {
            self.model = m.to_string();
        }
    }

    fn mark(&mut self, d: &Value, state: State) {
        self.state = Some(state);
        if let Some(t) = ts_ms(&d["timestamp"]) {
            self.log_time = self.log_time.max(t);
        }
    }
}

impl LogParser for ClaudeLog {
    fn feed(&mut self, d: &Value) {
        // Entries carry the shell's current directory, which follows `cd`: the session's project
        // is where it started.
        if self.project.is_empty()
            && let Some(cwd) = d["cwd"].as_str()
        {
            self.project = base_name(cwd);
            self.cwd = cwd.to_string();
        }
        if d["isSidechain"] == true {
            return;
        }
        match d["type"].as_str() {
            Some("ai-title") => {
                if let Some(t) = d["aiTitle"].as_str().filter(|t| !t.is_empty()) {
                    self.ai_title = t.to_string();
                }
            }
            Some("system") => self.usage(d),
            // A message typed while the agent was working, folded into the running turn.
            Some("attachment")
                if d["attachment"]["type"] == "queued_command" && d["attachment"]["origin"]["kind"].as_str().is_none_or(|k| k == "human") =>
            {
                if let Some(text) = d["attachment"]["prompt"].as_str() {
                    self.say(text, d);
                }
            }
            Some("user") => {
                if let Some(prompt) = typed_prompt(d) {
                    self.seen = true;
                    if prompt.starts_with("[Request interrupted") {
                        self.mark(d, State::Idle);
                        self.activity.clear();
                        return;
                    }
                    if self.first_prompt.is_empty() {
                        self.first_prompt = first_line(&prompt);
                    }
                    self.mark(d, State::Working);
                    self.turn_start = ts_ms(&d["timestamp"]).map(|t| t / 1000);
                    // History is kept across turns: the display scrolls through the conversation.
                    self.activity.clear();
                    self.say(&prompt, d);
                } else if d.get("toolUseResult").is_some() || d["message"]["content"][0]["type"] == "tool_result" {
                    // Shell and search output goes under its call (file reads would only repeat code).
                    for b in d["message"]["content"].as_array().into_iter().flatten() {
                        if b["type"] == "tool_result" {
                            let id = b["tool_use_id"].as_str().unwrap_or_default();
                            if !self.reads.contains(id) {
                                self.lines.set_output(id, &result_text(&b["content"]));
                            }
                        }
                    }
                    // The answer to a question (or any tool result) means the turn goes on.
                    if self.state.is_some_and(|s| s.waiting()) {
                        self.mark(d, State::Working);
                    }
                }
            }
            Some("assistant") => {
                self.seen = true;
                self.usage(d);
                let msg = &d["message"];
                if d["isApiErrorMessage"] == true {
                    let text = msg["content"][0]["text"].as_str().unwrap_or("API error");
                    self.summary = text.to_string();
                    self.mark(d, State::Error);
                    return;
                }
                let end = matches!(msg["stop_reason"].as_str(), Some("end_turn" | "stop_sequence" | "refusal"));
                for b in msg["content"].as_array().into_iter().flatten() {
                    match b["type"].as_str() {
                        Some("thinking" | "redacted_thinking") => {
                            if self.activity.is_empty() {
                                self.activity = "Thinking".into();
                            }
                            self.mark(d, State::Working);
                        }
                        Some("text") => {
                            let text = b["text"].as_str().unwrap_or_default().trim();
                            if text.is_empty() {
                                continue;
                            }
                            self.lines.push(Line::text(markdown(text)).with_tool_id(&msg_id(d)));
                            if end {
                                self.summary = strip_markdown(text);
                                self.activity.clear();
                                self.mark(d, State::Done);
                            } else {
                                self.mark(d, State::Working);
                            }
                        }
                        Some("tool_use") => {
                            let name = b["name"].as_str().unwrap_or("tool");
                            let line = tool_line(name, &b["input"]);
                            let id = b["id"].as_str().unwrap_or_default();
                            self.lines.push(
                                Line::tool(line.clone()).with_tool_id(id).with_detail(tool_detail(name, &b["input"])).with_file(&b["input"]),
                            );
                            if matches!(name, "Read" | "Glob" | "LS" | "TodoWrite" | "AskUserQuestion" | "ExitPlanMode") {
                                self.reads.insert(id.to_string());
                            }
                            self.activity = line;
                            self.activity_id = id.to_string();
                            if matches!(name, "AskUserQuestion" | "ExitPlanMode") {
                                self.prompt = question_text(&b["input"]);
                                self.mark(d, State::Question);
                            } else {
                                self.mark(d, State::Working);
                            }
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }

    fn session(&self) -> Option<Session> {
        if !self.seen {
            return None;
        }
        let mut s = Session::new("claude");
        s.project = self.project.clone();
        s.cwd = self.cwd.clone();
        s.title = if self.ai_title.is_empty() { self.first_prompt.clone() } else { self.ai_title.clone() };
        s.state = self.state.unwrap_or(State::Idle);
        s.activity = self.activity.clone();
        s.activity_id = self.activity_id.clone();
        s.prompt = self.prompt.clone();
        s.ask = self.ask.clone();
        s.summary = self.summary.clone();
        s.model = short_model(&self.model);
        s.ctx = self.context;
        s.ctx_max = window(&self.model, self.peak);
        s.out = self.output;
        s.turn_start = self.turn_start;
        s.lines = self.lines.to_vec();
        s.log_time = self.log_time;
        Some(s)
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn full_text_from_log() {
        let dir = tempfile::tempdir().unwrap();
        let proj = dir.path().join("projects").join("-home-u-proj");
        std::fs::create_dir_all(&proj).unwrap();
        let log = [
            json!({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_9","name":"Bash","input":{"command":"cargo test"}}]}}),
            json!({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_9","content":"line 1\nline 2\nline 3"}]}}),
            json!({"type":"assistant","message":{"content":[{"type":"text","text":"All **done**.\n\nSecond paragraph."}]}}),
        ];
        std::fs::write(proj.join("sess1.jsonl"), log.iter().map(|v| v.to_string()).collect::<Vec<_>>().join("\n")).unwrap();
        // SAFETY: tests in this module don't read CLAUDE_CONFIG_DIR concurrently.
        unsafe { std::env::set_var("CLAUDE_CONFIG_DIR", dir.path()) };
        assert_eq!(full_text("sess1", "toolu_9").unwrap(), "$ cargo test\n\nline 1\nline 2\nline 3");
        assert_eq!(full_text("sess1", "").unwrap(), "All **done**.\n\nSecond paragraph.");  // markdown: the display renders it
        assert!(full_text("sess1", "toolu_missing").is_none());
        unsafe { std::env::remove_var("CLAUDE_CONFIG_DIR") };
    }

    use super::*;
    use serde_json::json;

    fn run(entries: &[Value]) -> Session {
        let mut p = ClaudeLog::default();
        for e in entries {
            p.feed(e);
        }
        p.session().unwrap()
    }

    #[test]
    fn turn_states() {
        let mut log = vec![
            json!({"type":"user","cwd":"/home/u/proj","timestamp":"2026-09-26T03:00:00Z","message":{"role":"user","content":"fix the build"}}),
            json!({"type":"assistant","message":{"id":"m1","stop_reason":"tool_use","content":[{"type":"thinking","thinking":"…"}]}}),
            json!({"type":"assistant","cwd":"/home/u/proj/sub","message":{"id":"m1","stop_reason":"tool_use","content":[{"type":"text","text":"Looking."}]}}),
            json!({"type":"assistant","message":{"id":"m1","stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{"command":"cargo build\n--release"}}]}}),
        ];
        let s = run(&log);
        assert_eq!((s.state, s.project.as_str(), s.title.as_str()), (State::Working, "proj", "fix the build"));
        assert_eq!(s.activity, "Bash · cargo build");
        assert_eq!(s.lines, vec![Line::user("fix the build"), Line::text("Looking."), Line::tool("Bash · cargo build")]);
        assert_eq!(s.ask, "fix the build");
        assert_eq!(s.turn_start, Some(1790391600));

        log.push(json!({"type":"user","toolUseResult":{},"message":{"content":[{"type":"tool_result","content":"ok"}]}}));
        log.push(json!({"type":"assistant","message":{"id":"m2","stop_reason":"end_turn","content":[{"type":"text","text":"Build fixed.\nDetails…"}]}}));
        log.push(json!({"type":"ai-title","aiTitle":"Fix the build"}));
        let s = run(&log);
        // A single newline is a soft break in markdown: same paragraph.
        assert_eq!((s.state, s.summary.as_str(), s.title.as_str()), (State::Done, "Build fixed. Details…", "Fix the build"));

        log.push(json!({"type":"user","message":{"content":"<command-name>/model</command-name>"}}));
        assert_eq!(run(&log).state, State::Done, "slash-command wrappers are not prompts");

        log.push(json!({"type":"user","message":{"content":"next"}}));
        log.push(json!({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"AskUserQuestion","input":{"questions":[{"question":"Which one?"}]}}]}}));
        let s = run(&log);
        // The new turn appends to the conversation; the first turn's entries stay.
        assert_eq!((s.state, s.prompt.as_str()), (State::Question, "Which one?"));
        assert_eq!(s.lines[0], Line::user("fix the build"));
        assert_eq!(s.lines[s.lines.len() - 2], Line::user("next"));

        // A message typed while the agent works is folded into the turn as an attachment.
        log.push(json!({"type":"attachment","attachment":{"type":"queued_command","prompt":"also check the tests","origin":{"kind":"human"}}}));
        let s = run(&log);
        assert_eq!((s.ask.as_str(), s.lines.last()), ("also check the tests", Some(&Line::user("also check the tests"))));

        log.push(json!({"type":"user","toolUseResult":{"answers":{}},"message":{"content":[{"type":"tool_result","content":"A"}]}}));
        assert_eq!(run(&log).state, State::Working);

        log.push(json!({"type":"user","message":{"content":[{"type":"text","text":"[Request interrupted by user]"}]}}));
        assert_eq!(run(&log).state, State::Idle);

        log.push(json!({"type":"user","message":{"content":"again"}}));
        log.push(
            json!({"type":"assistant","isApiErrorMessage":true,"message":{"content":[{"type":"text","text":"API Error: overloaded"}]}}),
        );
        let s = run(&log);
        assert_eq!((s.state, s.summary.as_str()), (State::Error, "API Error: overloaded"));
    }

    #[test]
    fn sidechain_and_meta_ignored() {
        let s = run(&[
            json!({"type":"user","cwd":"/p/x","message":{"content":"real"}}),
            json!({"type":"user","isMeta":true,"message":{"content":"injected"}}),
            json!({"type":"assistant","isSidechain":true,"message":{"stop_reason":"end_turn","content":[{"type":"text","text":"sub done"}]}}),
        ]);
        assert_eq!((s.state, s.title.as_str(), s.summary.as_str()), (State::Working, "real", ""));
    }

    #[test]
    fn usage() {
        let mut p = ClaudeLog::default();
        let usage = json!({"input_tokens":2,"cache_creation_input_tokens":100,"cache_read_input_tokens":250_000,"output_tokens":40});
        for _ in 0..2 {
            p.feed(&json!({"type":"assistant","message":{"id":"m1","model":"claude-haiku-4-5","usage":usage,"content":[]}}));
        }
        let s = p.session().unwrap();
        assert_eq!((s.out, s.ctx, s.ctx_max, s.model.as_str()), (40, 250_142, 1_000_000, "haiku-4.5"));
        p.feed(&json!({"type":"system","subtype":"compact_boundary","compactMetadata":{"preTokens":900_000,"postTokens":15_000}}));
        assert_eq!(p.session().unwrap().ctx, 15_000);
        let mut q = ClaudeLog::default();
        q.feed(&json!({"type":"assistant","message":{"id":"m1","model":"claude-opus-5-5","usage":{"input_tokens":10},"content":[]}}));
        assert_eq!(q.session().unwrap().ctx_max, 1_000_000);
        let mut r = ClaudeLog::default();
        r.feed(&json!({"type":"assistant","message":{"id":"m1","model":"claude-haiku-4-5","usage":{"input_tokens":10},"content":[]}}));
        assert_eq!(r.session().unwrap().ctx_max, 200_000);
    }
}
