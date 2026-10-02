//! `deskhud demo on`: sample coding-agent sessions instead of the real ones, for screenshots and
//! demos (nobody wants their own prompts in a README).

use serde_json::{Value, json};

/// Sessions as they would arrive from the monitor; `started` anchors the timers (unix seconds).
pub fn sessions(started: i64, now: i64) -> Value {
    let line = |k: &str, s: &str| json!({"k": k, "s": s});
    let tool = |s: &str, d: &str| {
        // The file the call works on, for highlighting: "Edit · x" in the demo is about game.js.
        let f = if s.starts_with("Write · ") { s.trim_start_matches("Write · ") } else if s.starts_with("Edit") { "game.js" } else { "" };
        json!({"k": "tool", "s": s, "d": d, "f": f})
    };
    json!([
        {
            "id": "claude:demo-snake", "agent": "claude", "project": "snake-game",
            "title": "Classic snake game in the browser", "state": "working",
            "activity": "Edit · Speed up as the snake grows",
            "ask": "Add a high score that survives a page reload, and make it speed up a little every 5 apples.",
            "model": "opus-5.5", "ctx": 48_200, "ctx_max": 1_000_000, "out": 9_400,
            "turn_start": started - 74, "updated": now,
            "lines": [
                line("user", "Make a classic snake game I can play in the browser. Arrow keys, a score, and a game-over screen."),
                line("text", "I'll keep it to one HTML file with a canvas and a small game.js, so it runs anywhere without a build step."),
                tool("Write · index.html", "<!doctype html>\n<title>Snake</title>\n<canvas id=\"board\" width=\"400\" height=\"400\"></canvas>\n<script src=\"game.js\"></script>"),
                tool("Write · game.js", "const CELL = 20, SIZE = 20;\nlet snake = [{ x: 10, y: 10 }], dir = { x: 1, y: 0 };\nlet apple = spawn(), score = 0, tick = 120;"),
                tool("Bash · Start a local web server", "Serving HTTP on 0.0.0.0 port 8000 (http://0.0.0.0:8000/) ..."),
                line("text", "It runs at http://localhost:8000. Arrow keys steer, apples add 10 points, and hitting a wall or yourself shows the game-over screen with Space to restart."),
                line("user", "Add a high score that survives a page reload, and make it speed up a little every 5 apples."),
                line("text", "Two changes:\n\n- **High score:** saved in `localStorage`, so it survives a reload\n- **Speed:** the tick shortens by *8 ms* every fifth apple, down to a floor so it stays playable"),
                line("tool", "Read · game.js"),
                tool("Edit · Keep the high score in localStorage", "- let best = 0;\n+ let best = Number(localStorage.getItem(\"best\")) || 0;\n+ function saveBest() { localStorage.setItem(\"best\", best); }"),
                tool("Edit · Speed up as the snake grows", "- if (ate) score += 10;\n+ if (ate) {\n+   score += 10;\n+   if (score % 50 === 0) tick = Math.max(60, tick - 8);\n+ }"),
            ]
        },
        {
            "id": "codex:demo-leaderboard", "agent": "codex", "project": "snake-leaderboard",
            "title": "Leaderboard API for the snake game", "state": "done",
            "summary": "Added POST /scores and GET /scores?top=10 backed by SQLite, with input validation and 12 tests. Run it with `cargo run`; the game can post scores to http://localhost:3000.",
            "ask": "Write a tiny leaderboard API the snake game can post scores to.",
            "model": "gpt-6.1-sol", "ctx": 31_900, "ctx_max": 258_400, "out": 6_100,
            "turn_start": started - 900, "updated": now - 240,
            "lines": [
                line("user", "Write a tiny leaderboard API the snake game can post scores to."),
                tool("shell · cargo new snake-leaderboard", "    Creating binary (application) `snake-leaderboard` package"),
                line("tool", "edit · main.rs"),
                tool("shell · cargo test", "running 12 tests\n............\ntest result: ok. 12 passed; 0 failed"),
                line("text", "All 12 tests pass."),
            ]
        },
        {
            "id": "pi:demo-sprites", "agent": "pi", "project": "snake-sprites",
            "title": "Pixel-art sprites for the snake", "state": "done",
            "summary": "Drew 16x16 sprites for the head, body, tail and apple in four directions and exported a single sprite sheet.",
            "model": "k3-256k", "ctx": 12_300, "ctx_max": 262_144, "out": 2_800,
            "updated": now - 600,
            "lines": [
                line("user", "Make 16x16 pixel-art sprites for the snake and the apple."),
                line("tool", "write · sprites.svg"),
                line("text", "Done: one sprite sheet with all four directions."),
            ]
        },
        {
            "id": "opencode:demo-sound", "agent": "opencode", "project": "snake-sound",
            "title": "Sound effects for the snake game", "state": "done",
            "summary": "Added eat, turn and game-over sounds with the Web Audio API, no files to load.",
            "model": "qwen3.8-27b", "ctx": 21_900, "ctx_max": 172_032, "out": 2_400,
            "updated": now - 900,
            "lines": [
                line("user", "Add little sound effects: eating an apple, turning, and game over."),
                tool("Write · sound.js", "const ctx = new AudioContext();\nexport function beep(freq, ms) {"),
                line("text", "Added `sound.js`: short **Web Audio** tones, so there are no files to load."),
            ]
        }
    ])
}

/// A sample process table (for the processes popup in screenshot mode): no real names or commands.
pub fn procs() -> Value {
    let p = |pid: u32, name: &str, cmd: &str, threads: u32, mem_mb: u64, cpu: f64| {
        json!({"pid": pid, "name": name, "cmd": cmd, "threads": threads, "user": "user", "mem": mem_mb << 20, "cpu": cpu})
    };
    json!({"count": 412, "procs": [
        p(4210, "cargo", "cargo build --release", 18, 412, 38.6),
        p(4388, "rustc", "rustc --crate-name snake_leaderboard --edition=2024", 9, 655, 21.4),
        p(3120, "node", "node ./node_modules/.bin/vite --port 8000", 11, 214, 6.2),
        p(2981, "claude", "claude", 30, 458, 2.1),
        p(3077, "codex", "codex", 22, 196, 1.4),
        p(1713, "Hyprland", "Hyprland", 9, 214, 1.1),
        p(3502, "chrome", "chrome --type=renderer", 41, 520, 0.9),
        p(3299, "code", "code --unity-launch ~/snake-game", 34, 610, 0.8),
        p(2140, "rust-analyzer", "rust-analyzer", 26, 1288, 0.6),
        p(1774, "quickshell", "quickshell", 23, 590, 0.4),
        p(2810, "pi", "pi", 12, 160, 0.3),
        p(2602, "kitty", "kitty", 4, 230, 0.2),
        p(2390, "deskhud", "deskhud daemon", 5, 41, 0.1),
        p(1380, "pipewire", "pipewire", 3, 18, 0.1),
        p(1002, "systemd", "systemd --user", 1, 14, 0.0),
    ]})
}
