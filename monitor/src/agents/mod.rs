//! Coding-agent activity: what Claude Code, Codex and pi sessions are doing right now.
//!
//! Every recently written session log is followed incrementally from its first byte, so facts at
//! the head of a long log (working directory, first prompt) are never lost and token usage covers
//! the whole session. Claude Code hook events, when installed, override the log-derived state:
//! permission prompts and idle questions never reach the log, hooks see them as they happen.

mod claude;
mod codex;
mod liveness;
mod opencode;
mod pi;

use std::collections::{HashMap, VecDeque};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::Serialize;
use serde_json::Value;

pub use claude::ClaudeCode;
pub use codex::Codex;
pub use pi::Pi;

/// Sessions with no activity for this long are not shown...
pub const RECENT: Duration = Duration::from_secs(30 * 60);
/// ...except ones waiting on the user, which stay up this long.
pub const WAITING_RECENT: Duration = Duration::from_secs(2 * 3600);
/// A working session with no activity for this long was most likely killed or closed.
pub const STALLED: Duration = Duration::from_secs(10 * 60);
/// Sessions sent to the display.
pub const MAX_SESSIONS: usize = 8;
/// Tool descriptions remembered per Claude session (only recent lines are shown anyway).
const DESCRIPTIONS_KEPT: usize = 64;
/// A session whose agent process is gone disappears once its log has been quiet this long.
pub const CLOSED_GRACE: Duration = Duration::from_secs(10);

// Protocol string limits (characters).
const TITLE_MAX: usize = 120;
const ACTIVITY_MAX: usize = 160;
const PROMPT_MAX: usize = 400;
const SUMMARY_MAX: usize = 600;
const ASK_MAX: usize = 300;
const LINE_MAX: usize = 400;
/// Conversation entries kept per session, across turns (the display scrolls through them).
const LINES_MAX: usize = 40;
/// A tool line's detail: the edit, the file written, or the output.
const DETAIL_MAX: usize = 400;
const DETAIL_LINES: usize = 12;
/// What less relevant sessions keep when the frame gets tight.
const LINES_SHORT: usize = 6;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum State {
    Working,
    Permission,
    Question,
    Done,
    Error,
    Idle,
}

impl State {
    fn waiting(self) -> bool {
        matches!(self, State::Permission | State::Question)
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct Line {
    pub k: &'static str,
    pub s: String,
    /// Tool lines: what the call changed or printed (a few lines), shown under the call.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub d: String,
    /// Tool lines: the file the call reads or writes (its name only), so the display can
    /// highlight it in the right language.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub f: String,
    /// Claude tool_use id: a hook's description can replace the argument, and the display asks
    /// for the call's full output by it.
    #[serde(rename = "i", skip_serializing_if = "String::is_empty")]
    pub tool_id: String,
}

impl PartialEq for Line {
    fn eq(&self, o: &Self) -> bool {
        self.k == o.k && self.s == o.s
    }
}
impl Eq for Line {}

impl Line {
    pub fn tool(s: impl Into<String>) -> Self {
        Line { k: "tool", s: s.into(), d: String::new(), f: String::new(), tool_id: String::new() }
    }
    pub fn text(s: impl Into<String>) -> Self {
        Line { k: "text", s: s.into(), d: String::new(), f: String::new(), tool_id: String::new() }
    }
    /// Something the person typed: the turn's prompt or a message sent while the agent worked.
    pub fn user(s: impl Into<String>) -> Self {
        Line { k: "user", s: s.into(), d: String::new(), f: String::new(), tool_id: String::new() }
    }
    pub fn with_tool_id(mut self, id: &str) -> Self {
        self.tool_id = id.to_string();
        self
    }
    pub fn with_detail(mut self, d: String) -> Self {
        self.d = d;
        self
    }
    /// The file a tool call works on, from its input (`file_path`, `notebook_path`, `path`).
    pub fn with_file(mut self, input: &Value) -> Self {
        if let Some(p) = ["file_path", "notebook_path", "path"].iter().find_map(|k| input[*k].as_str()) {
            self.f = base_name(p);
            clip(&mut self.f, 38);
        }
        self
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Session {
    pub id: String,
    pub agent: &'static str,
    pub project: String,
    pub title: String,
    pub state: State,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub activity: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub prompt: String,
    /// The person's latest message to the agent (the current turn's prompt or a later follow-up).
    #[serde(skip_serializing_if = "String::is_empty")]
    pub ask: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub summary: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub model: String,
    pub ctx: u64,
    pub ctx_max: u64,
    pub out: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub turn_start: Option<i64>,
    /// Last activity, unix seconds.
    pub updated: i64,
    /// Working directory the session started in (liveness checks; not sent).
    #[serde(skip)]
    pub cwd: String,
    /// tool_use id of the call in `activity` (Claude), for hook descriptions; not sent.
    #[serde(skip)]
    pub activity_id: String,
    pub lines: Vec<Line>,
    /// Timestamp (unix ms) of the newest log entry that changed the state; hook events newer than
    /// this override the log.
    #[serde(skip)]
    pub log_time: i64,
}

impl Session {
    pub fn new(agent: &'static str) -> Self {
        Session {
            id: String::new(),
            agent,
            project: String::new(),
            title: String::new(),
            state: State::Idle,
            activity: String::new(),
            prompt: String::new(),
            ask: String::new(),
            summary: String::new(),
            model: String::new(),
            ctx: 0,
            ctx_max: 0,
            out: 0,
            turn_start: None,
            updated: 0,
            cwd: String::new(),
            activity_id: String::new(),
            lines: Vec::new(),
            log_time: 0,
        }
    }

    /// Clamp every string to the protocol limits.
    fn truncate(&mut self) {
        clip(&mut self.title, TITLE_MAX);
        clip(&mut self.activity, ACTIVITY_MAX);
        clip(&mut self.prompt, PROMPT_MAX);
        clip(&mut self.ask, ASK_MAX);
        clip(&mut self.summary, SUMMARY_MAX);
        if self.lines.len() > LINES_MAX {
            self.lines.drain(..self.lines.len() - LINES_MAX);
        }
        for l in &mut self.lines {
            clip(&mut l.s, LINE_MAX);
            clip(&mut l.d, DETAIL_MAX);
        }
    }
}

/// Truncate to `max` characters (never inside a UTF-8 sequence), ending with an ellipsis.
pub fn clip(s: &mut String, max: usize) {
    if let Some((i, _)) = s.char_indices().nth(max) {
        let cut = s[..i].char_indices().nth(max - 1).map(|(j, _)| j).unwrap_or(i);
        s.truncate(cut);
        s.push('…');
    }
}

/// Bounded list of the newest stream lines of a turn.
#[derive(Clone, Debug, Default)]
pub struct Lines(VecDeque<Line>);

impl Lines {
    pub fn push(&mut self, l: Line) {
        if self.0.back() == Some(&l) {
            return;
        }
        if self.0.len() == LINES_MAX {
            self.0.pop_front();
        }
        self.0.push_back(l);
    }
    pub fn clear(&mut self) {
        self.0.clear();
    }
    /// Attach a tool call's output to its line, unless the call already shows what it changed.
    pub fn set_output(&mut self, tool_id: &str, out: &str) {
        if tool_id.is_empty() {
            return;
        }
        if let Some(l) = self.0.iter_mut().rev().find(|l| l.tool_id == tool_id)
            && l.d.is_empty()
        {
            l.d = detail(out);
        }
    }
    pub fn to_vec(&self) -> Vec<Line> {
        self.0.iter().cloned().collect()
    }
}

// ---------------------------------------------------------------- helpers shared by providers

pub fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default()
}

pub fn mtime(p: &Path) -> Option<SystemTime> {
    fs::metadata(p).and_then(|m| m.modified()).ok()
}

pub fn unix_ms(t: SystemTime) -> i64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

/// RFC 3339 string → unix ms.
pub fn ts_ms(v: &Value) -> Option<i64> {
    v.as_str()?.parse::<jiff::Timestamp>().ok().map(|t| t.as_millisecond())
}

pub fn base_name(path: &str) -> String {
    Path::new(path.trim_end_matches('/')).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| path.to_string())
}

pub fn first_line(s: &str) -> String {
    s.lines().map(str::trim).find(|l| !l.is_empty()).unwrap_or_default().to_string()
}

// ---------------------------------------------------------------- text for a screen

/// Make a shell command or path readable on a small screen: `$HOME` → `~`, Claude scratchpad
/// paths → `scratchpad`, and leading `cd dir &&` / `VAR=value;` noise dropped.
pub fn humanize(cmd: &str) -> String {
    let mut s = cmd.trim().to_string();
    // Leading `cd <dir> &&`, `cd <dir>;`, `VAR=value;`, `export VAR=value &&`, repeatedly.
    while let Some((head, rest)) = split_first_command(&s) {
        let h = head.trim();
        let h = h.strip_prefix("export ").unwrap_or(h);
        let is_cd = h == "cd" || h.starts_with("cd ");
        let is_assign = h.split_once('=').is_some_and(|(k, v)| {
            !k.is_empty()
                && k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
                && !k.starts_with(|c: char| c.is_ascii_digit())
                && (!v.contains(char::is_whitespace) || v.starts_with('"') || v.starts_with('\''))
        });
        if !(is_cd || is_assign) || rest.trim().is_empty() {
            break;
        }
        s = rest.trim().to_string();
    }
    // Claude scratchpad: /tmp/claude-<uid>/<project>/<session>/scratchpad[/...] → scratchpad[/...]
    while let Some(i) = s.find("/tmp/claude-") {
        let tail = &s[i..];
        let end = tail.find(char::is_whitespace).unwrap_or(tail.len());
        match tail[..end].find("/scratchpad") {
            Some(j) => s.replace_range(i..i + j + "/scratchpad".len(), "scratchpad"),
            None => break,
        }
    }
    if let Some(home) = std::env::var_os("HOME").map(|h| h.to_string_lossy().into_owned()).filter(|h| h.len() > 1) {
        s = s.replace(&format!("{home}/"), "~/");
        if s.ends_with(&home) {
            let n = s.len() - home.len();
            s.replace_range(n.., "~");
        }
    }
    s
}

/// `head ; rest` or `head && rest` at the first top-level separator (quotes respected).
fn split_first_command(s: &str) -> Option<(&str, &str)> {
    let (mut quote, b) = (None::<u8>, s.as_bytes());
    for i in 0..b.len() {
        match (quote, b[i]) {
            (Some(q), c) if c == q => quote = None,
            (Some(_), _) => {}
            (None, b'\'' | b'"') => quote = Some(b[i]),
            (None, b';') => return Some((&s[..i], &s[i + 1..])),
            (None, b'&') if b.get(i + 1) == Some(&b'&') => return Some((&s[..i], &s[i + 2..])),
            (None, b'|' | b'\n') => return None,
            _ => {}
        }
    }
    None
}

/// Markdown → plain text, paragraphs kept (joined by `\n`): `**b**`, `*e*`, `` `c` ``,
/// `[t](u)`, heading / quote / list markers and fenced code are removed.
pub fn strip_markdown(text: &str) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut fenced = false;
    for para in text.split("\n\n") {
        let mut lines = Vec::new();
        for raw in para.lines() {
            let l = raw.trim();
            if l.starts_with("```") {
                fenced = !fenced;
                continue;
            }
            if fenced || l.is_empty() || l.chars().all(|c| matches!(c, '-' | '*' | '_' | '=')) {
                continue;
            }
            let mut l = l.trim_start_matches('#').trim_start_matches('>').trim_start();
            if let Some(r) = l.strip_prefix("- ").or_else(|| l.strip_prefix("* ")).or_else(|| l.strip_prefix("+ ")) {
                l = r;
            } else if let Some((n, r)) = l.split_once(". ")
                && !n.is_empty()
                && n.chars().all(|c| c.is_ascii_digit())
            {
                l = r;
            }
            lines.push(inline_md(l));
        }
        let p = lines.join(" ").split_whitespace().collect::<Vec<_>>().join(" ");
        if !p.is_empty() {
            out.push(p);
        }
    }
    out.join("\n")
}

fn inline_md(l: &str) -> String {
    let mut s = l.replace("**", "").replace("__", "").replace('`', "");
    // [text](url) → text
    while let Some(a) = s.find('[') {
        let Some(b) = s[a..].find("](").map(|b| a + b) else { break };
        let Some(c) = s[b..].find(')').map(|c| b + c) else { break };
        let text = s[a + 1..b].to_string();
        s.replace_range(a..=c, &text);
    }
    // *emphasis* → emphasis (a lone `*`, e.g. in "5 * 3", stays)
    let parts: Vec<&str> = s.split('*').collect();
    if parts.len() >= 3 && parts.len() % 2 == 1 { parts.concat() } else { s }
}

/// Agent prose as markdown for the display, which renders it: trimmed, with runs of blank lines
/// collapsed to one.
pub fn markdown(text: &str) -> String {
    let mut out = String::new();
    let mut blank = false;
    for l in text.trim().lines() {
        let l = l.trim_end();
        if l.trim().is_empty() {
            blank = true;
            continue;
        }
        if !out.is_empty() {
            out.push_str(if blank { "\n\n" } else { "\n" });
        }
        out.push_str(l);
        blank = false;
    }
    out
}

/// Agent prose as plain text, one paragraph per line. A leading heading is kept as a prefix.
pub fn prose(text: &str) -> String {
    // Every paragraph, one per line (the display wraps them and grows the block to fit); a short
    // heading runs into its paragraph.
    let plain = strip_markdown(text);
    let paras: Vec<&str> = plain.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i < paras.len() {
        let heading = i == 0 && text.trim_start().starts_with('#') && paras[0].chars().count() < 60 && paras.len() > 1;
        if heading {
            out.push(format!("{}: {}", paras[0], paras[1]));
            i += 2;
        } else {
            out.push(paras[i].to_string());
            i += 1;
        }
    }
    out.join("\n")
}

/// `name · most telling argument` for a tool call.
pub fn tool_line(name: &str, input: &Value) -> String {
    let name = match name.strip_prefix("mcp__") {
        Some(rest) => rest.replacen("__", ".", 1),
        None => name.to_string(),
    };
    let arg =
        ["description", "command", "cmd", "file_path", "path", "notebook_path", "pattern", "query", "url", "skill", "subject", "prompt"]
            .iter()
            .find_map(|k| match &input[*k] {
                Value::String(s) if !s.trim().is_empty() => Some(first_line(s)),
                Value::Array(a) if !a.is_empty() => Some(shell_words(a)),
                _ => None,
            })
            .unwrap_or_default();
    // A bare path: the file name is what matters. Commands get the screen treatment.
    let arg = if arg.starts_with('/') && !arg.contains(' ') { base_name(&arg) } else { humanize(&arg) };
    if arg.is_empty() { name } else { format!("{name} · {arg}") }
}

/// Terminal colour and cursor codes (`ESC [ … letter`) out of command output.
pub fn strip_ansi(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        if c == '\x1b' {
            if it.peek() == Some(&'[') {
                it.next();
                while let Some(&n) = it.peek() {
                    it.next();
                    if n.is_ascii_alphabetic() || n == '~' {
                        break;
                    }
                }
            }
            continue;
        }
        if c != '\r' {
            out.push(c);
        }
    }
    out
}

/// A few lines of code or output for a small screen: no blank lines, tabs as spaces, no trailing
/// space, at most `DETAIL_LINES` lines and `DETAIL_MAX` characters.
pub fn detail(text: &str) -> String {
    let text = strip_ansi(text);
    // Common indentation off (centred banners pad thousands of spaces).
    let indent = text.lines().filter(|l| !l.trim().is_empty()).map(|l| l.len() - l.trim_start().len()).min().unwrap_or(0);
    let mut out: Vec<String> = Vec::new();
    for l in text.lines().map(|l| l.get(indent..).unwrap_or(l.trim_start())) {
        let l = l.replace('\t', "  ");
        let l = l.trim_end();
        if l.trim().is_empty() {
            continue;
        }
        if out.len() == DETAIL_LINES {
            out.push("…".into());
            break;
        }
        out.push(l.to_string());
    }
    let mut s = out.join("\n");
    clip(&mut s, DETAIL_MAX);
    s
}

/// What a tool call changes, known when it's made: the edit as a mini diff, or a file's opening
/// lines. Empty for calls whose output tells more (shell, search), filled in from the result.
pub fn tool_detail(name: &str, input: &Value) -> String {
    let diff = |old: &str, new: &str| {
        let minus = old.lines().filter(|l| !l.trim().is_empty()).take(4).map(|l| format!("- {}", l.trim()));
        let plus = new.lines().filter(|l| !l.trim().is_empty()).take(8).map(|l| format!("+ {}", l.trim()));
        detail(&minus.chain(plus).collect::<Vec<_>>().join("\n"))
    };
    match name {
        "Edit" => diff(input["old_string"].as_str().unwrap_or_default(), input["new_string"].as_str().unwrap_or_default()),
        "MultiEdit" => {
            let e = &input["edits"][0];
            diff(e["old_string"].as_str().unwrap_or_default(), e["new_string"].as_str().unwrap_or_default())
        }
        "Write" => detail(input["content"].as_str().unwrap_or_default()),
        "NotebookEdit" => detail(input["new_source"].as_str().unwrap_or_default()),
        _ => String::new(),
    }
}

/// Largest full text sent for the popup (bytes; the display holds 16 KiB).
pub const FULL_MAX: usize = 16 * 1024;

/// The complete text behind a shortened line, for the display's popup: a tool call's full edit,
/// file or output (`tool` = its id), or with an empty `tool` the session's last reply. Read from
/// the agent's log or database on demand.
pub fn full_text(session: &str, tool: &str) -> Option<String> {
    let (agent, id) = session.split_once(':')?;
    let mut text = match agent {
        "claude" => claude::full_text(id, tool)?,
        "codex" => codex::full_text(id, tool)?,
        "pi" => pi::full_text(&home().join(".pi/agent"), id, tool)?,
        "omp" => pi::full_text(&home().join(".omp/agent"), id, tool)?,
        "opencode" => opencode::full_text(id, tool)?,
        _ => return None,
    };
    if text.len() > FULL_MAX {
        let mut cut = FULL_MAX - 3;
        while !text.is_char_boundary(cut) {
            cut -= 1;
        }
        text.truncate(cut);
        text.push('…');
    }
    Some(text)
}

/// A tool call in full: the whole edit as a diff, the file written, or the command and its output.
pub fn full_tool(name: &str, input: &Value, output: &str) -> String {
    let output = &strip_ansi(output);
    let lines = |s: &str, mark: &str| s.lines().map(|l| format!("{mark}{l}")).collect::<Vec<_>>().join("\n");
    match name {
        "Edit" => format!("{}\n{}", lines(input["old_string"].as_str().unwrap_or_default(), "- "), lines(input["new_string"].as_str().unwrap_or_default(), "+ ")),
        "MultiEdit" => input["edits"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|e| format!("{}\n{}", lines(e["old_string"].as_str().unwrap_or_default(), "- "), lines(e["new_string"].as_str().unwrap_or_default(), "+ ")))
            .collect::<Vec<_>>()
            .join("\n\n"),
        "Write" => input["content"].as_str().unwrap_or_default().to_string(),
        "NotebookEdit" => input["new_source"].as_str().unwrap_or_default().to_string(),
        "Bash" => format!("$ {}\n\n{}", input["command"].as_str().unwrap_or_default(), output.trim_end()),
        _ => output.trim_end().to_string(),
    }
}

/// Text of a tool result: a string, or the text blocks of a content array.
pub fn result_text(content: &Value) -> String {
    match content {
        Value::String(s) => s.clone(),
        Value::Array(a) => a.iter().filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n"),
        _ => String::new(),
    }
}

/// `["bash","-lc","cargo test"]` → `cargo test`.
pub fn shell_words(a: &[Value]) -> String {
    let words: Vec<&str> = a.iter().filter_map(Value::as_str).collect();
    match words.as_slice() {
        [sh, flag, script] if sh.ends_with("sh") && flag.starts_with('-') => first_line(script),
        _ => words.join(" "),
    }
}

/// `claude-opus-5-5` → `opus-5.5`, `claude-haiku-4-5-20251001` → `haiku-4.5`.
pub fn short_model(model: &str) -> String {
    let m = model.strip_prefix("claude-").unwrap_or(model);
    let mut parts: Vec<&str> = m.split('-').collect();
    if parts.last().is_some_and(|p| p.len() == 8 && p.chars().all(|c| c.is_ascii_digit())) {
        parts.pop();
    }
    let n = parts.len();
    if n >= 3 && [parts[n - 1], parts[n - 2]].iter().all(|p| !p.is_empty() && p.len() <= 2 && p.chars().all(|c| c.is_ascii_digit())) {
        let minor = parts.pop().unwrap();
        let major = parts.pop().unwrap();
        return format!("{}-{major}.{minor}", parts.join("-"));
    }
    parts.join("-")
}

// ---------------------------------------------------------------- providers

pub trait Provider: Send {
    /// Session logs written since `since`.
    fn logs(&self, since: SystemTime) -> Vec<PathBuf>;
    /// A fresh incremental parser for one log.
    fn parser(&self, path: &Path) -> Box<dyn LogParser>;
    /// Reload provider-wide data (e.g. thread names) before a poll.
    fn refresh(&mut self) {}
    /// Fill in what the log itself does not say.
    fn decorate(&self, _s: &mut Session) {}
    /// Sessions read whole rather than from logs (an agent that keeps them in a database), each
    /// with when it last changed (unix ms).
    fn sessions(&mut self, _since: SystemTime) -> Vec<(Session, i64)> {
        Vec::new()
    }
}

pub trait LogParser: Send {
    fn feed(&mut self, entry: &Value);
    /// The session so far; `None` while the log holds no conversation.
    fn session(&self) -> Option<Session>;
}

pub fn providers() -> Vec<Box<dyn Provider>> {
    vec![Box::new(ClaudeCode::new()), Box::new(Codex::new()), Box::new(Pi::new()), Box::new(Pi::omp()), Box::new(opencode::OpenCode::new())]
}

/// Read size per step when catching up on a log.
const CHUNK: usize = 4 << 20;

/// One followed log: parser state plus how far it has been read.
struct Tracked {
    parser: Box<dyn LogParser>,
    offset: u64,
    /// Incomplete last line, completed by the next read.
    partial: Vec<u8>,
    modified: SystemTime,
    len: u64,
}

impl Tracked {
    /// Feed everything appended since the last call.
    fn catch_up(&mut self, path: &Path) {
        let Ok(mut f) = File::open(path) else { return };
        if f.seek(SeekFrom::Start(self.offset)).is_err() {
            return;
        }
        let mut buf = vec![0u8; CHUNK];
        loop {
            let n = match f.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            self.offset += n as u64;
            let mut data = std::mem::take(&mut self.partial);
            data.extend_from_slice(&buf[..n]);
            let end = data.iter().rposition(|&b| b == b'\n').map(|i| i + 1).unwrap_or(0);
            for line in data[..end].split(|&b| b == b'\n') {
                if line.iter().any(|b| !b.is_ascii_whitespace())
                    && let Ok(v) = serde_json::from_slice::<Value>(line)
                {
                    self.parser.feed(&v);
                }
            }
            self.partial = data[end..].to_vec();
        }
    }
}

// ---------------------------------------------------------------- Claude Code hooks

/// What the hooks last said about one Claude Code session.
#[derive(Clone, Debug, Default)]
pub struct HookState {
    pub state: Option<State>,
    pub prompt: String,
    /// Claude's one-line description of each recent tool call, by tool_use_id (newest last).
    pub descriptions: VecDeque<(String, String)>,
    /// Receive time, unix ms.
    pub at: i64,
    /// `SessionEnd` time; the session is hidden unless its log is written to after this.
    pub ended: Option<i64>,
}

/// Fold one Claude Code hook event into the per-session table.
pub fn apply_hook(hooks: &mut HashMap<String, HookState>, ev: &Value, now_ms: i64) {
    let Some(sid) = ev["session_id"].as_str().filter(|s| !s.is_empty()) else { return };
    let event = ev["hook_event_name"].as_str().unwrap_or_default();
    let sub = ev["agent_id"].as_str().is_some_and(|a| !a.is_empty());
    let h = hooks.entry(sid.to_string()).or_default();
    let tool = ev["tool_name"].as_str().unwrap_or_default();
    let set = |h: &mut HookState, state: State, prompt: String| {
        h.state = Some(state);
        h.prompt = prompt;
        h.at = now_ms;
    };
    if let (Some(id), Some(desc)) = (ev["tool_use_id"].as_str(), ev["tool_input"]["description"].as_str())
        && !desc.trim().is_empty()
        && !h.descriptions.iter().any(|(i, _)| i == id)
    {
        if h.descriptions.len() == DESCRIPTIONS_KEPT {
            h.descriptions.pop_front();
        }
        h.descriptions.push_back((id.to_string(), first_line(desc)));
    }
    match event {
        "SessionStart" => {
            h.ended = None;
            h.at = now_ms;
        }
        "SessionEnd" => h.ended = Some(now_ms),
        "UserPromptSubmit" => {
            h.ended = None;
            set(h, State::Working, String::new());
        }
        "PreToolUse" if !sub && matches!(tool, "AskUserQuestion" | "ExitPlanMode") => {
            set(h, State::Question, question_text(&ev["tool_input"]))
        }
        // Claude asks permission to show its own question dialogs: that's a question, not a
        // permission prompt (the PreToolUse event just before carries the same text).
        "PermissionRequest" if matches!(tool, "AskUserQuestion" | "ExitPlanMode") => {
            set(h, State::Question, question_text(&ev["tool_input"]))
        }
        "PermissionRequest" => set(h, State::Permission, permission_text(tool, &ev["tool_input"])),
        "Notification" => match ev["notification_type"].as_str().unwrap_or_default() {
            // PermissionRequest (when present) already carries the tool; keep its richer text.
            "permission_prompt" if !matches!(h.state, Some(State::Question)) && (h.state != Some(State::Permission) || h.prompt.is_empty()) => {
                set(h, State::Permission, ev["message"].as_str().unwrap_or("Permission needed").to_string())
            }
            "elicitation_dialog" => set(h, State::Question, ev["message"].as_str().unwrap_or("Input needed").to_string()),
            _ => {}
        },
        "Stop" if !sub => set(h, State::Done, String::new()),
        "StopFailure" if !sub => {
            let msg = ev["error"].as_str().or(ev["message"].as_str()).unwrap_or("Turn failed").to_string();
            set(h, State::Error, msg)
        }
        "PreToolUse" | "PostToolUse" | "PostToolUseFailure" | "SubagentStart" | "SubagentStop" => set(h, State::Working, String::new()),
        _ => {}
    }
}

/// What a permission prompt is for: Claude's description, then the command itself.
fn permission_text(tool: &str, input: &Value) -> String {
    let line = tool_line(tool, input);
    match input["description"].as_str().map(str::trim).filter(|d| !d.is_empty()) {
        Some(desc) if input["command"].is_string() => format!("{tool} · {desc}\n{}", humanize(input["command"].as_str().unwrap_or_default())),
        _ => line,
    }
}

/// `Bash · cargo build` + description → `Bash · Build the firmware`.
fn describe(line: &str, desc: &str) -> String {
    match line.split_once(" · ") {
        Some((tool, _)) => format!("{tool} · {desc}"),
        None => format!("{line} · {desc}"),
    }
}

/// The question of an AskUserQuestion call, or the plan of ExitPlanMode.
pub fn question_text(input: &Value) -> String {
    let q = &input["questions"][0];
    let options: Vec<&str> = q["options"].as_array().into_iter().flatten().filter_map(|o| o["label"].as_str()).collect();
    q["question"]
        .as_str()
        .or(input["question"].as_str())
        .map(|t| if options.is_empty() { t.to_string() } else { format!("{t}\n{}", options.join("  ·  ")) })
        .or_else(|| input["plan"].as_str().map(|p| format!("Approve plan: {}", first_line(p).trim_start_matches('#').trim())))
        .unwrap_or_else(|| "Waiting for your answer".into())
}

// ---------------------------------------------------------------- monitor

pub struct Monitor {
    providers: Vec<Box<dyn Provider>>,
    files: HashMap<PathBuf, (usize, Tracked)>,
    pub hooks: HashMap<String, HookState>,
    /// Set by `with_liveness`: Claude Code's config dir; closed sessions are dropped.
    claude_registry: Option<PathBuf>,
    recent: Duration,
}

impl Monitor {
    pub fn new(providers: Vec<Box<dyn Provider>>) -> Self {
        Monitor { providers, files: HashMap::new(), hooks: HashMap::new(), recent: RECENT, claude_registry: None }
    }

    /// Drop sessions whose agent process has exited (see `live.rs`). `claude_dir` is Claude Code's
    /// config directory (`~/.claude`), which holds the running-session registry.
    pub fn with_liveness(mut self) -> Self {
        let dir = std::env::var_os("CLAUDE_CONFIG_DIR").map(PathBuf::from).unwrap_or_else(|| home().join(".claude"));
        self.claude_registry = Some(dir);
        self
    }

    /// Look further back than usual (debugging: `deskhud agents --since`).
    pub fn with_recent(mut self, recent: Duration) -> Self {
        self.recent = recent;
        self
    }

    pub fn hook(&mut self, ev: &Value) {
        apply_hook(&mut self.hooks, ev, unix_ms(SystemTime::now()));
    }

    pub fn poll(&mut self) -> Vec<Session> {
        self.poll_at(SystemTime::now())
    }

    pub fn poll_at(&mut self, now: SystemTime) -> Vec<Session> {
        let since = now - self.recent.max(WAITING_RECENT);
        let mut live = HashMap::new();
        for (pi, provider) in self.providers.iter_mut().enumerate() {
            provider.refresh();
            for path in provider.logs(since) {
                let Ok(meta) = fs::metadata(&path) else { continue };
                let (modified, len) = (meta.modified().unwrap_or(now), meta.len());
                let mut t = match self.files.remove(&path) {
                    // Rewritten or truncated logs are read again from the start.
                    Some((_, t)) if len >= t.len => t,
                    _ => Tracked { parser: provider.parser(&path), offset: 0, partial: Vec::new(), modified, len: 0 },
                };
                if t.len != len || t.offset == 0 {
                    t.catch_up(&path);
                }
                (t.modified, t.len) = (modified, len);
                live.insert(path, (pi, t));
            }
        }
        self.files = live;

        let now_ms = unix_ms(now);
        let live = self.claude_registry.as_deref().map(liveness::Live::scan);
        // Every session seen: from followed logs, then from providers that read them whole.
        let mut found: Vec<(Session, i64, PathBuf)> = Vec::new();
        for (path, (pi, t)) in &self.files {
            let Some(mut s) = t.parser.session() else { continue };
            if s.id.is_empty() {
                s.id = path.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            }
            self.providers[*pi].decorate(&mut s);
            found.push((s, unix_ms(t.modified), path.clone()));
        }
        for provider in self.providers.iter_mut() {
            found.extend(provider.sessions(since).into_iter().map(|(s, last)| (s, last, PathBuf::new())));
        }
        let mut out = Vec::new();
        for (mut s, mut last, path) in found {
            let path = path.as_path();
            if s.agent == "claude"
                && let Some(h) = self.hooks.get(&s.id)
            {
                if h.ended.is_some_and(|e| last <= e + 2000) {
                    continue;
                }
                for l in &mut s.lines {
                    if let Some((_, d)) = h.descriptions.iter().find(|(i, _)| !l.tool_id.is_empty() && *i == l.tool_id) {
                        l.s = describe(&l.s, d);
                    }
                }
                if let Some((_, d)) = h.descriptions.iter().find(|(i, _)| !s.activity_id.is_empty() && *i == s.activity_id) {
                    s.activity = describe(&s.activity, d);
                }
                if let Some(state) = h.state
                    && h.at >= s.log_time
                {
                    s.state = state;
                    s.prompt = h.prompt.clone();
                }
                last = last.max(h.at);
            }
            let idle_for = Duration::from_millis(now_ms.saturating_sub(last).max(0) as u64);
            // Closed agents leave the display. The grace period covers a session whose process
            // has not registered yet (or a log written just before the agent exits).
            if idle_for > CLOSED_GRACE
                && live.as_ref().and_then(|l| l.open(s.agent, path, &s.id, &s.cwd)) == Some(false)
            {
                continue;
            }
            let window = if s.state.waiting() { self.recent.max(WAITING_RECENT) } else { self.recent };
            if idle_for > window {
                continue;
            }
            if s.state == State::Working && idle_for > STALLED {
                s.state = State::Idle;
            }
            if !s.state.waiting() {
                s.prompt.clear();
            }
            s.id = format!("{}:{}", s.agent, s.id);
            s.updated = last / 1000;
            s.truncate();
            out.push(s);
        }
        sort(&mut out);
        out.truncate(MAX_SESSIONS);
        fit(&mut out, FRAME_BUDGET);
        out
    }
}

/// Bytes the `agents` frame may take (the display accepts frames up to 16 KiB).
pub const FRAME_BUDGET: usize = 36 * 1024;

fn frame_len(sessions: &[Session]) -> usize {
    serde_json::to_string(sessions).map(|s| s.len()).unwrap_or(0) + 32
}

/// Shrink the list until it fits `budget` bytes: stream lines of the less relevant sessions go
/// first, then long summaries, then whole sessions from the end.
pub fn fit(sessions: &mut Vec<Session>, budget: usize) {
    // Shorten the history of the less relevant sessions first, then drop it.
    for i in (1..sessions.len()).rev() {
        if frame_len(sessions) <= budget {
            return;
        }
        let n = sessions[i].lines.len();
        sessions[i].lines.drain(..n.saturating_sub(LINES_SHORT));
    }
    for i in (1..sessions.len()).rev() {
        if frame_len(sessions) <= budget {
            return;
        }
        sessions[i].lines.clear();
    }
    if frame_len(sessions) > budget {
        for s in sessions.iter_mut().skip(1) {
            clip(&mut s.summary, 160);
        }
    }
    while frame_len(sessions) > budget && sessions.len() > 1 {
        sessions.pop();
    }
    if frame_len(sessions) > budget
        && let Some(s) = sessions.first_mut()
    {
        s.lines.truncate(2);
        clip(&mut s.summary, 200);
    }
}

/// Waiting on the user first, then working, then most recent.
fn sort(sessions: &mut [Session]) {
    sessions.sort_by_key(|s| {
        let rank = match s.state {
            State::Permission | State::Question => 0,
            State::Working => 1,
            _ => 2,
        };
        (rank, std::cmp::Reverse(s.updated), s.id.clone())
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn humanized_commands() {
        let home = std::env::var("HOME").unwrap();
        assert_eq!(
            humanize("S=/tmp/claude-1000/-home-kunal-src-x/11a92edc-170c/scratchpad; deskhud get | jq '.settings'"),
            "deskhud get | jq '.settings'"
        );
        assert_eq!(humanize(&format!("cd {home}/src/app && cargo build --release")), "cargo build --release");
        assert_eq!(humanize("cd /x; export A=1 && B=\"two words\"; make"), "make");
        assert_eq!(
            humanize("magick /tmp/claude-1000/-home-u-p/abc/scratchpad/a.png out.png"),
            "magick scratchpad/a.png out.png"
        );
        assert_eq!(humanize(&format!("ls {home}/Work")), "ls ~/Work");
        // Not a prefix to drop: kept whole.
        assert_eq!(humanize("cd /x | tee log"), "cd /x | tee log");
        assert_eq!(humanize("A=1"), "A=1");
        assert_eq!(tool_line("Bash", &json!({"command":"cd /x && cargo test"})), "Bash · cargo test");
    }

    #[test]
    fn markdown_to_plain() {
        assert_eq!(
            strip_markdown("The project is published as a **private** repo at [GitHub](https://x).\nIt has `one` commit.\n\n- next\n- *step*"),
            "The project is published as a private repo at GitHub. It has one commit.\nnext step"
        );
        assert_eq!(prose("## Summary\n\nAll *three* tests pass.\n\nMore."), "Summary: All three tests pass.\nMore.");
        assert_eq!(prose("Plain first.\nsame paragraph\n\nsecond"), "Plain first. same paragraph\nsecond");
        assert_eq!(strip_markdown("```bash\nrm -rf /\n```\nDone."), "Done.");
        assert_eq!(inline_md("5 * 3 = 15"), "5 * 3 = 15");
    }

    #[test]
    fn hook_descriptions_replace_arguments() {
        let dir = tempfile::tempdir().unwrap();
        let proj = dir.path().join("projects").join("-home-u-proj");
        fs::create_dir_all(&proj).unwrap();
        let log = [
            json!({"type":"user","cwd":"/home/u/proj","timestamp":"2026-10-02T03:00:00Z","message":{"role":"user","content":"deploy it"}}),
            json!({"type":"assistant","message":{"id":"m1","stop_reason":"tool_use","content":[{"type":"tool_use","id":"toolu_1","name":"Bash","input":{"command":"cd /home/u/proj && ./tools/deploy.sh 2>&1 | tail -1"}}]}}),
        ];
        fs::write(proj.join("s1.jsonl"), log.iter().map(|v| v.to_string() + "\n").collect::<String>()).unwrap();
        let mut m = Monitor::new(vec![Box::new(ClaudeCode::with_root(dir.path().join("projects")))]);
        let s = m.poll().into_iter().next().unwrap();
        assert_eq!(s.activity, "Bash · ./tools/deploy.sh 2>&1 | tail -1");
        m.hook(&json!({"session_id":"s1","hook_event_name":"PreToolUse","tool_name":"Bash","tool_use_id":"toolu_1",
                       "tool_input":{"command":"cd /home/u/proj && ./tools/deploy.sh","description":"Build and install firmware over Wi-Fi"}}));
        let s = m.poll().into_iter().next().unwrap();
        assert_eq!(s.activity, "Bash · Build and install firmware over Wi-Fi");
        assert_eq!(s.lines.last().unwrap().s, "Bash · Build and install firmware over Wi-Fi");
        m.hook(&json!({"session_id":"s1","hook_event_name":"PermissionRequest","tool_name":"Bash",
                       "tool_input":{"command":"rm -rf build","description":"Remove the build dir"}}));
        let s = m.poll().into_iter().next().unwrap();
        assert_eq!((s.state, s.prompt.as_str()), (State::Permission, "Bash · Remove the build dir\nrm -rf build"));
    }
    use serde_json::json;

    #[test]
    fn clip_respects_chars() {
        let mut s = "héllo wörld".to_string();
        clip(&mut s, 5);
        assert_eq!(s, "héll…");
        let mut s = "short".to_string();
        clip(&mut s, 5);
        assert_eq!(s, "short");
    }

    #[test]
    fn model_names() {
        assert_eq!(short_model("claude-opus-5-5"), "opus-5.5");
        assert_eq!(short_model("claude-haiku-4-5-20251001"), "haiku-4.5");
        assert_eq!(short_model("gpt-6-astra"), "gpt-6-astra");
        assert_eq!(short_model("claude-sonnet-4-20250514"), "sonnet-4");
    }

    #[test]
    fn tool_lines() {
        assert_eq!(tool_line("Bash", &json!({"command":"cargo build\n--release"})), "Bash · cargo build");
        assert_eq!(tool_line("Read", &json!({"file_path":"/a/b/agents.rs"})), "Read · agents.rs");
        assert_eq!(tool_line("mcp__github__create_pr", &json!({})), "github.create_pr");
        assert_eq!(tool_line("shell", &json!({"command":["bash","-lc","cargo test"]})), "shell · cargo test");
    }

    #[test]
    fn hook_states() {
        let mut h = HashMap::new();
        let ev = |name: &str, extra: Value| {
            let mut v = json!({"session_id":"s1","hook_event_name":name});
            v.as_object_mut().unwrap().extend(extra.as_object().unwrap().clone());
            v
        };
        apply_hook(&mut h, &ev("UserPromptSubmit", json!({})), 1);
        assert_eq!(h["s1"].state, Some(State::Working));
        apply_hook(&mut h, &ev("PermissionRequest", json!({"tool_name":"Bash","tool_input":{"command":"rm -rf build"}})), 2);
        assert_eq!((h["s1"].state, h["s1"].prompt.as_str()), (Some(State::Permission), "Bash · rm -rf build"));
        // The generic notification that follows does not erase the tool.
        apply_hook(
            &mut h,
            &ev("Notification", json!({"notification_type":"permission_prompt","message":"Claude needs your permission"})),
            3,
        );
        assert_eq!(h["s1"].prompt, "Bash · rm -rf build");
        apply_hook(&mut h, &ev("PostToolUse", json!({"tool_name":"Bash"})), 4);
        assert_eq!(h["s1"].state, Some(State::Working));
        apply_hook(
            &mut h,
            &ev("PreToolUse", json!({"tool_name":"AskUserQuestion","tool_input":{"questions":[{"question":"Which board?"}]}})),
            5,
        );
        assert_eq!((h["s1"].state, h["s1"].prompt.as_str()), (Some(State::Question), "Which board?"));
        // A subagent finishing is not the session finishing.
        apply_hook(&mut h, &ev("Stop", json!({"agent_id":"a1"})), 6);
        // Claude asks permission to show a question dialog: still a question, with its options.
        let ask = json!({"tool_name":"AskUserQuestion","tool_input":{"questions":[{"question":"Which layout?","options":[{"label":"Split"},{"label":"Single"}]}]}});
        apply_hook(&mut h, &ev("PreToolUse", ask.clone()), 7);
        apply_hook(&mut h, &ev("PermissionRequest", ask), 8);
        apply_hook(&mut h, &ev("Notification", json!({"notification_type":"permission_prompt","message":"Claude needs your permission"})), 9);
        let hs = &h["s1"];
        assert_eq!((hs.state, hs.prompt.as_str()), (Some(State::Question), "Which layout?\nSplit  ·  Single"));
        assert_eq!(h["s1"].state, Some(State::Question));
        apply_hook(&mut h, &ev("Stop", json!({})), 7);
        assert_eq!(h["s1"].state, Some(State::Done));
        apply_hook(&mut h, &ev("Notification", json!({"notification_type":"idle_prompt","message":"waiting"})), 8);
        assert_eq!(h["s1"].state, Some(State::Done));
        apply_hook(&mut h, &ev("StopFailure", json!({"error":"overloaded"})), 9);
        assert_eq!((h["s1"].state, h["s1"].prompt.as_str()), (Some(State::Error), "overloaded"));
        apply_hook(&mut h, &ev("SessionEnd", json!({"reason":"exit"})), 10);
        assert_eq!(h["s1"].ended, Some(10));
    }

    /// Hooks override the log only when newer than the log's last state change.
    #[test]
    fn hook_log_fusion() {
        let dir = tempfile::tempdir().unwrap();
        let proj = dir.path().join("projects/-home-u-proj");
        fs::create_dir_all(&proj).unwrap();
        let log = proj.join("s1.jsonl");
        let now = SystemTime::now();
        let iso = |ms: i64| jiff::Timestamp::from_millisecond(ms).unwrap().to_string();
        let t0 = unix_ms(now) - 5000;
        let lines = [
            json!({"type":"user","cwd":"/home/u/proj","timestamp":iso(t0),"message":{"role":"user","content":"deploy it"}}),
            json!({"type":"assistant","timestamp":iso(t0 + 1000),"message":{"id":"m1","stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{"command":"./deploy.sh"}}]}}),
        ];
        fs::write(&log, lines.iter().map(|l| l.to_string() + "\n").collect::<String>()).unwrap();
        let mut m = Monitor::new(vec![Box::new(ClaudeCode::with_root(dir.path().join("projects")))]);
        let s = &m.poll()[0];
        assert_eq!((s.id.as_str(), s.state, s.project.as_str()), ("claude:s1", State::Working, "proj"));

        // Permission prompt: only the hook knows.
        m.hook(&json!({"session_id":"s1","hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"./deploy.sh"}}));
        let s = &m.poll()[0];
        assert_eq!((s.state, s.prompt.as_str()), (State::Permission, "Bash · ./deploy.sh"));

        // Denied: the log moves on (interrupt) after the hook; the log wins.
        let later = unix_ms(SystemTime::now()) + 1000;
        let mut f = fs::OpenOptions::new().append(true).open(&log).unwrap();
        use std::io::Write;
        writeln!(f, "{}", json!({"type":"user","timestamp":iso(later),"message":{"role":"user","content":[{"type":"text","text":"[Request interrupted by user for tool use]"}]}})).unwrap();
        drop(f);
        let s = &m.poll()[0];
        assert_eq!((s.state, s.prompt.as_str()), (State::Idle, ""));

        // SessionEnd hides it.
        m.hook(&json!({"session_id":"s1","hook_event_name":"SessionEnd"}));
        assert!(m.poll().is_empty());
    }

    #[test]
    fn frames_fit_the_display() {
        let big = |i: usize| {
            let mut s = Session { id: format!("s{i}"), state: State::Working, ..Session::new("claude") };
            s.title = "題".repeat(200);
            s.summary = "ü".repeat(900);
            s.prompt = "x".repeat(500);
            s.lines = (0..10).map(|_| Line::text("€".repeat(300))).collect();
            s.truncate();
            s
        };
        let mut v: Vec<Session> = (0..MAX_SESSIONS).map(big).collect();
        assert!(frame_len(&v) > FRAME_BUDGET);
        fit(&mut v, FRAME_BUDGET);
        assert!(frame_len(&v) <= FRAME_BUDGET);
        assert!(!v[0].lines.is_empty(), "the top session keeps its stream");
    }

    #[test]
    fn sort_order() {
        let mk = |id: &str, state, updated| Session { id: id.into(), state, updated, ..Session::new("claude") };
        let mut v = vec![mk("a", State::Done, 30), mk("b", State::Working, 10), mk("c", State::Question, 5), mk("d", State::Working, 20)];
        sort(&mut v);
        let ids: Vec<_> = v.iter().map(|s| s.id.as_str()).collect();
        assert_eq!(ids, ["c", "d", "b", "a"]);
    }
}

/// Against this machine's real logs: `cargo test -- --ignored --nocapture live`.
#[cfg(test)]
mod live {
    use super::*;

    fn parse_all(p: &dyn Provider, since: SystemTime) -> Vec<Session> {
        p.logs(since)
            .into_iter()
            .filter_map(|path| {
                let mut t = Tracked { parser: p.parser(&path), offset: 0, partial: Vec::new(), modified: SystemTime::now(), len: 0 };
                t.catch_up(&path);
                let mut s = t.parser.session()?;
                if s.id.is_empty() {
                    s.id = path.file_stem()?.to_string_lossy().into_owned();
                }
                p.decorate(&mut s);
                Some(s)
            })
            .collect()
    }

    #[test]
    #[ignore]
    fn live() {
        let since = SystemTime::now() - Duration::from_secs(14 * 24 * 3600);
        let mut provs = providers();
        for p in provs.iter_mut() {
            p.refresh();
            for s in parse_all(p.as_ref(), since) {
                println!("{:<7} {:<10?} {:<24} {:<44} {}", s.agent, s.state, s.project, s.title.chars().take(44).collect::<String>(), s.activity);
                if s.agent != "claude" {
                    assert!(!s.project.is_empty(), "{} has no project", s.id);
                    assert!(!s.title.is_empty(), "{} has no title", s.id);
                }
            }
        }
        let shown = Monitor::new(providers()).poll();
        println!("\nshown now: {}", serde_json::to_string_pretty(&shown).unwrap());
    }
}
