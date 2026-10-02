# DeskHUD monitoring service

The program that runs on each PC. It finds the DeskHUD display on the network, pairs with it, and
streams two things over a WebSocket:

- **PC stats** (1 per second):
  - CPU load per thread, temperature (plus per-CCD on AMD), clock and package power;
  - memory as used, available, cached (page cache + reclaimable slab) and free, plus swap;
  - GPUs (NVIDIA through NVML, AMD through sysfs) with load, temperature, VRAM, power, fan and clock;
  - disks with I/O rates; network rates, totals since boot and address; load average and uptime;
  - the top 16 processes, one row per process (pid, name, CPU, memory, threads), btop-style;
  - the PC's time zone (from `/etc/localtime`), so the display shows the active PC's local time.
- **Coding-agent activity**, whenever it changes: every Claude Code, Codex, pi, oh-my-pi and opencode session on this PC. For each one it sends the conversation (your messages and the agent's replies, in markdown), every tool call with what it changed or printed, whether the agent is waiting for your permission or an answer, the model and context usage.
- **On request from the display:**
  - the complete text behind any shortened message or tool call (read from the agent's log or database when you tap it);
  - the PC's system info, from `fastfetch` with your config, its icons and logo;
  - a btop-style table of the top 40 processes.

It is a single Rust binary, `deskhud`, that is both the background service and the CLI. It reads
`/proc`, `/sys` and the agents' session logs (and opencode's database) directly; the only program
it runs is `fastfetch`, when the display asks for system info. The wire protocol is in
[`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## Install

```bash
cargo install --path .        # → ~/.cargo/bin/deskhud
deskhud install --hooks       # systemd user service + Claude Code hooks, starts it now
deskhud install --dry-run     # show what would be written, change nothing
deskhud uninstall             # remove the service and the hooks
```

`install` writes `~/.config/systemd/user/deskhud.service` and starts it. With `--hooks` it adds
`deskhud hook` as an async hook to `~/.claude/settings.json`, after saving a timestamped backup.
Existing settings are kept, and running it twice changes nothing.

The first time the service connects, the display asks **"<this PC> wants to connect"**: tap
Allow. The PC then gets a 256-bit token, which is the only credential it needs from then on.

## Several PCs

Run the service on every PC you want on the display; the display shows one at a time.

- The PC on screen sends stats every second.
- The others are on standby and only send a heartbeat.
- If the PC on screen goes quiet for 6 s (sleep, crash, cable pulled), the display switches to the standby PC it heard from most recently, and that PC starts streaming at once.
- `deskhud activate` puts this PC on screen. `deskhud activate --pin` keeps it there whenever it's online.

## CLI

| Command | |
|---|---|
| `deskhud status` | connection, display, on-screen or standby, and the sessions being sent |
| `deskhud agents [--since MIN]` | the coding-agent sessions exactly as sent; `--since` looks further back in the logs |
| `deskhud stats` | one stats sample exactly as sent |
| `deskhud sysinfo` | the system info and process table the display's popups show |
| `deskhud set key=value …` | change display settings, e.g. `deskhud set theme=ink accent=#cc785c brightness=80` (keys in the protocol doc) |
| `deskhud get` | the display's settings and device info |
| `deskhud page overview\|agents\|system\|settings` | switch the display's page; `sysinfo` / `procs` open those popups |
| `deskhud demo on\|off` | screenshot mode: sample coding-agent sessions and processes instead of yours, and placeholder names and addresses on the display |
| `deskhud notify "Title" "body" [--level info\|warn\|alert]` | show a toast on the display |
| `deskhud activate [--pin]` | put this PC on screen |
| `deskhud identify` / `deskhud reboot` | flash the display's name / restart it |
| `deskhud ota firmware.bin` | install firmware over Wi-Fi, then wait for the display to come back and check the version |
| `deskhud devices` | displays on the network (mDNS `_deskhud._tcp`) |
| `deskhud use <id\|host[:port]>` | pick a display; no argument means automatic |
| `deskhud daemon` | run the service in the foreground (systemd runs this) |
| `deskhud hook` | Claude Code hook entry point: reads the event on stdin, always exits 0 at once |

Commands that talk to the display go through the running service over a local socket
(`$XDG_RUNTIME_DIR/deskhud.sock`), so each PC keeps a single connection. `ota` talks HTTP
directly and works without the service.

## How coding agents are tracked

| Agent | Source | Notes |
|---|---|---|
| Claude Code | `~/.claude/projects/*/*.jsonl` (honors `CLAUDE_CONFIG_DIR`) + hooks | Title from Claude's own session title or the first prompt. Model and context window come from the log (1M for Opus/Sonnet/Fable 5). |
| Codex | `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl` (honors `CODEX_HOME`), last 7 days | Current rollout format. Injected context (environment, AGENTS.md) is filtered out of prompts, and `::directive{}` lines are stripped from results. |
| pi | `~/.pi/agent/sessions/*/*.jsonl` | Context window from pi's `models.json`. |
| oh-my-pi (`omp`) | `~/.omp/agent/sessions/*/*.jsonl` | A pi fork with the same log format; read by the same code, plus omp's tool intents, hashline edits, titles and exit records. |
| opencode | `~/.local/share/opencode/opencode.db` (SQLite, read-only; honors `XDG_DATA_HOME`) | Sessions, messages and parts read whole for the recently active sessions. Context window from `limit.context` in `~/.config/opencode/opencode.json`. |

- **Reading the logs:** each log is followed from its first byte and then incrementally, so a long session never loses its project, first prompt or token totals.
- **Written for a screen:** tool calls use Claude's own one-line description when the hooks deliver it (`Bash · Build and install firmware over Wi-Fi`). Otherwise commands are tidied: `~` for your home folder, `scratchpad` for Claude's temporary folders, and leading `cd … &&` or `VAR=…;` dropped; file tools show the file name. Permission prompts show the description and the exact command.
- **Replies and tool output:** agent replies keep their markdown (the display renders it). Each tool call carries a short detail (`d`: an edit as `- old` / `+ new` lines, the start of a file written, or the command's output, without terminal escape codes), its file name for syntax highlighting (`f`) and its id (`i`), which the display uses to fetch the whole thing when tapped.
- **Your messages:** both the prompt that starts a turn and anything you type while the agent works (Claude's queued messages) are sent as `user` lines, and the latest one is pinned on the display.
- **States:** `working`, `permission`, `question`, `done`, `error`, `idle`.
- **Closed sessions disappear** once their log has been quiet for 10 s. For Claude Code, the service checks the running-session registry Claude keeps in `~/.claude/sessions/`, and the `SessionEnd` hook removes a session the moment it ends. For Codex, pi, omp and opencode it checks for a running process of that agent that has the session open or runs in its folder (`src/agents/liveness.rs`).
- **Expiry:** sessions that are still open disappear after 30 min without activity, or 2 h if they're waiting for you. A working session that goes silent for 10 min becomes `idle`.

Claude Code doesn't write permission prompts to its log. The hooks fill that gap:

| Hook events | State |
|---|---|
| PermissionRequest, Notification (permission prompt) | `permission` |
| PreToolUse of AskUserQuestion / ExitPlanMode, Notification (dialog) | `question` |
| UserPromptSubmit, Pre/PostToolUse | `working` |
| Stop | `done` |
| StopFailure | `error` |
| SessionEnd | session removed |

Whichever is newer, the hook event or the latest log entry, decides the state. So a permission
prompt shows the moment it appears, and goes back to `working` (or `idle`, if denied) as soon as
the log moves on. Without hooks, the logs alone are used.

## Files

| Path | |
|---|---|
| `~/.config/deskhud/config.toml` | `pc_id` (generated once), optional `device` and `host_name` |
| `~/.local/state/deskhud/state.json` | pairing tokens and last known display addresses (mode 600) |
| `$XDG_RUNTIME_DIR/deskhud.sock` | CLI ↔ service socket (mode 600) |

On start the service tries the last known address of a paired display first, which connects
within a second. It only falls back to mDNS discovery when the display has moved.

## Code

| File | |
|---|---|
| `src/main.rs` | CLI |
| `src/daemon.rs` | the service: the stats and agents loops, the local socket, hook intake |
| `src/config.rs` | config, state file (tokens, last addresses) and paths |
| `src/link.rs` | display connection: resolve, pair, on-screen/standby role, reconnect with backoff; answers the display's requests (full text, system info, processes) |
| `src/discover.rs` | mDNS browsing |
| `src/stats.rs` | system stats |
| `src/sysview.rs` | the popups' data: fastfetch parsed into sections, and the process table |
| `src/demo.rs` | screenshot mode: sample sessions and processes |
| `src/agents/` | session tracking: `claude.rs`, `codex.rs`, `pi.rs` (pi and oh-my-pi), `opencode.rs` (its SQLite database), `liveness.rs` (which agents are still running), shared logic, hook fusion and full-text lookup in `mod.rs` |
| `src/ota.rs`, `src/http.rs` | firmware upload and the display's HTTP API |
| `src/install.rs` | systemd unit and Claude Code hook install / uninstall |
| `tests/link.rs` | end to end against an in-process mock display: pairing, streaming, standby, reconnect |

```bash
cargo test                                   # unit + end-to-end tests
cargo test -- --ignored --nocapture live     # parse every real session log of the last 14 days
```

## Troubleshooting

- **`deskhud status` says `discovering`**: the display isn't on the same network segment, or mDNS is blocked. `deskhud use 192.168.x.y` skips discovery.
- **`denied`**: pairing was declined on the display. It asks again after 2 minutes.
- **Logs**: `journalctl --user -u deskhud -f`. Set `RUST_LOG=debug` in the unit to log every frame.
- **Sessions look wrong**: `deskhud agents` shows exactly what the display receives.
