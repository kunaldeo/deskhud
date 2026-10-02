use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use clap::{Parser, Subcommand};
use serde_json::{Map, Value, json};

use deskhud::agents::Monitor;
use deskhud::config::{self, Config, State};
use deskhud::{daemon, discover, install, ota};

#[derive(Parser)]
#[command(name = "deskhud", version, about = "DeskHUD monitoring service: PC stats and coding-agent activity for the DeskHUD Wi-Fi display")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Run the monitoring service: find the display, stream stats and agent activity (systemd runs this).
    Daemon,
    /// Connection state, device, role and the coding-agent sessions being sent.
    Status {
        #[arg(long)]
        json: bool,
    },
    /// List displays on the network (mDNS, 3 s).
    Devices,
    /// Use this display: a device id from `devices`, or a static host[:port]. Empty: auto.
    Use { device: Option<String> },
    /// Change display settings: `deskhud set brightness=60 accent=#ff9e64 clock_24h=false`.
    Set {
        #[arg(required = true, value_name = "KEY=VALUE")]
        pairs: Vec<String>,
    },
    /// Show display settings and device info.
    Get,
    /// Show a toast on the display.
    Notify {
        title: String,
        body: Option<String>,
        /// info | warn | alert
        #[arg(long, default_value = "info")]
        level: String,
        /// Seconds on screen.
        #[arg(long, default_value_t = 8)]
        ttl: u32,
    },
    /// Make this PC the one on screen (`--pin`: keep it there whenever it is online).
    Activate {
        #[arg(long)]
        pin: bool,
    },
    /// Flash the display's name on screen.
    Identify,
    /// Show a page on the display: overview, agents, system or settings (or a popup: sysinfo, procs)
    Page {
        #[arg(value_parser = ["overview", "agents", "system", "settings", "sysinfo", "procs"])]
        page: String,
    },
    /// Screenshot mode: sample coding-agent sessions, and placeholder names and addresses on the display.
    Demo {
        #[arg(value_parser = ["on", "off"])]
        state: String,
    },
    /// Reboot the display.
    Reboot,
    /// Upload firmware over Wi-Fi.
    Ota {
        /// App image: firmware/build/deskhud.bin
        file: PathBuf,
        /// Display address (default: the daemon's display, else mDNS).
        #[arg(long)]
        host: Option<String>,
    },
    /// Print one sample of the system stats sent to the display.
    Stats,
    /// Print the system info (from fastfetch) and process table the display's popups show.
    Sysinfo,
    /// Claude Code hook entry point (reads the event on stdin; always exits 0).
    Hook,
    /// Print the coding-agent sessions as sent to the display.
    Agents {
        /// Scan logs directly (without the daemon), looking back this many minutes.
        #[arg(long)]
        since: Option<u64>,
    },
    /// Install the systemd user service (and, with --hooks, the Claude Code hooks).
    Install {
        #[arg(long)]
        hooks: bool,
        /// Print what would be installed.
        #[arg(long)]
        dry_run: bool,
    },
    /// Remove the service and the Claude Code hooks.
    Uninstall,
}

fn main() {
    let cli = Cli::parse();
    if matches!(cli.cmd, Cmd::Hook) {
        // Hooks run on every tool call: no logging setup, no config, no output.
        daemon::forward_hook();
        return;
    }
    tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()))
        .with_target(false)
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stderr()))
        .without_time()
        .init();
    if let Err(e) = run(cli.cmd) {
        eprintln!("deskhud: {e:#}");
        std::process::exit(1);
    }
}

/// Send a protocol message through the daemon, waiting for replies of the given types.
fn send(msg: Value, wait: &[&str], soft: bool) -> Result<Value> {
    let v = daemon::request(&json!({"op":"send","msg":msg,"wait":wait,"soft":soft}))?;
    Ok(v["result"].clone())
}

fn parse_value(v: &str) -> Value {
    match v {
        "true" | "on" | "yes" => Value::Bool(true),
        "false" | "off" | "no" => Value::Bool(false),
        _ => v
            .parse::<i64>()
            .map(Value::from)
            .or_else(|_| v.parse::<f64>().map(Value::from))
            .unwrap_or_else(|_| Value::String(v.to_string())),
    }
}

fn print_json(v: &Value) {
    println!("{}", serde_json::to_string_pretty(v).unwrap_or_default());
}

fn run(cmd: Cmd) -> Result<()> {
    match cmd {
        Cmd::Daemon => daemon::run(),
        Cmd::Hook => Ok(()),
        Cmd::Sysinfo => {
            print_json(&deskhud::sysview::fastfetch());
            let mut top = deskhud::sysview::ProcTop::new();
            print_json(&top.sample(5));
            Ok(())
        }
        Cmd::Stats => {
            let mut s = deskhud::stats::Sampler::new();
            s.sample();
            std::thread::sleep(Duration::from_secs(1));
            print_json(&serde_json::to_value(s.sample())?);
            Ok(())
        }
        Cmd::Status { json } => {
            let v = daemon::request(&json!({"op":"status"}))?;
            if json {
                print_json(&v);
                return Ok(());
            }
            let s = &v["status"];
            let dev = &s["device"];
            let st = s["state"].as_str().unwrap_or("?");
            println!("state     {st}");
            if !s["device_id"].as_str().unwrap_or_default().is_empty() {
                println!(
                    "display   {} ({}) at {}  fw {}",
                    dev["name"].as_str().unwrap_or("?"),
                    s["device_id"].as_str().unwrap_or("?"),
                    s["addr"].as_str().unwrap_or("?"),
                    dev["fw"].as_str().unwrap_or("?")
                );
            }
            match s["active"].as_bool() {
                Some(true) => println!("role      active (this PC is on screen)"),
                Some(false) => println!("role      standby (on screen: {})", s["active_host"].as_str().unwrap_or("another PC")),
                None => {}
            }
            if let Some(code) = s["pair_code"].as_str().filter(|c| !c.is_empty()) {
                let _ = code;
                println!("pairing   tap Allow on the display");
            }
            if let Some(e) = s["last_error"].as_str().filter(|e| !e.is_empty()) {
                println!("error     {e}");
            }
            let sessions = v["sessions"].as_array().cloned().unwrap_or_default();
            println!("agents    {} session(s)", sessions.len());
            for x in sessions {
                println!(
                    "  {:<10} {:<7} {:<22} {}",
                    x["state"].as_str().unwrap_or(""),
                    x["agent"].as_str().unwrap_or(""),
                    x["project"].as_str().unwrap_or(""),
                    x["prompt"].as_str().or(x["activity"].as_str()).or(x["title"].as_str()).unwrap_or("")
                );
            }
            Ok(())
        }
        Cmd::Devices => {
            let cfg = Config::load()?;
            let state = State::load(&config::state_dir());
            let found = discover::browse(Duration::from_secs(3), None)?;
            if found.is_empty() {
                println!("no displays found (is it on the same network and powered?)");
            }
            for f in found {
                let mark = if cfg.device == f.id { "*" } else { " " };
                let paired = if state.tokens.contains_key(&f.id) { "paired" } else { "" };
                println!("{mark} {:<14} {:<20} {:<22} fw {:<10} {paired}", f.id, f.name, f.addr, f.fw);
            }
            Ok(())
        }
        Cmd::Use { device } => {
            let mut cfg = Config::load()?;
            cfg.device = device.unwrap_or_default();
            cfg.save()?;
            println!("using {}", if cfg.device.is_empty() { "the display found on the network" } else { &cfg.device });
            if daemon::request(&json!({"op":"retarget","device":cfg.device})).is_ok() {
                println!("daemon reconnecting");
            }
            Ok(())
        }
        Cmd::Set { pairs } => {
            let mut settings = Map::new();
            for p in pairs {
                let (k, v) = p.split_once('=').ok_or_else(|| anyhow!("expected KEY=VALUE, got {p:?}"))?;
                settings.insert(k.trim().to_string(), parse_value(v.trim()));
            }
            let r = send(json!({"t":"set","settings":settings}), &["settings"], false)?;
            print_json(&r["settings"]);
            Ok(())
        }
        Cmd::Get => {
            print_json(&send(json!({"t":"get"}), &["settings", "device"], false)?);
            Ok(())
        }
        Cmd::Notify { title, body, level, ttl } => {
            if !matches!(level.as_str(), "info" | "warn" | "alert") {
                bail!("--level must be info, warn or alert");
            }
            send(json!({"t":"notify","level":level,"title":title,"body":body.unwrap_or_default(),"ttl":ttl}), &[], false)?;
            Ok(())
        }
        Cmd::Activate { pin } => {
            let mut msg = json!({"t":"cmd","cmd":"activate"});
            if pin {
                msg["arg"] = json!("pin");
            }
            send(msg, &["role"], true)?;
            let v = daemon::request(&json!({"op":"status"}))?;
            println!("{}", if v["status"]["active"] == true { "this PC is on screen" } else { "requested; the display has not switched" });
            Ok(())
        }
        Cmd::Identify => send(json!({"t":"cmd","cmd":"identify"}), &[], false).map(|_| ()),
        Cmd::Page { page } => send(json!({"t":"cmd","cmd":"page","arg":page}), &[], false).map(|_| ()),
        Cmd::Demo { state } => {
            // Sample sessions here, placeholder names and addresses on the display.
            daemon::request(&json!({"op":"demo","on":state == "on"}))?;
            send(json!({"t":"cmd","cmd":"privacy","arg":state}), &[], false).map(|_| ())
        }
        Cmd::Reboot => send(json!({"t":"cmd","cmd":"reboot"}), &[], false).map(|_| ()),
        Cmd::Ota { file, host } => ota::run(&file, host.as_deref()),
        Cmd::Agents { since } => {
            let sessions = match since {
                None => match daemon::request(&json!({"op":"agents"})) {
                    Ok(v) => v["sessions"].clone(),
                    Err(_) => serde_json::to_value(Monitor::new(deskhud::agents::providers()).with_liveness().poll())?,
                },
                Some(m) => {
                    serde_json::to_value(Monitor::new(deskhud::agents::providers()).with_recent(Duration::from_secs(m * 60)).poll())?
                }
            };
            print_json(&sessions);
            Ok(())
        }
        Cmd::Install { hooks, dry_run } => install::install(hooks, dry_run),
        Cmd::Uninstall => install::uninstall(),
    }
}
