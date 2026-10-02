# DeskHUD wire protocol (v1)

The display is a server. PCs run the `deskhud` agent, discover the display over mDNS and connect to it.
Any number of PCs may be connected; exactly one is **active** (its data is on screen). The others are
**standby**: they keep a heartbeat going so the display can fail over to them instantly.

## Discovery

mDNS service `_deskhud._tcp`, port 80. TXT records:

| key  | value                                  |
|------|----------------------------------------|
| id   | device id: lowercase MAC without colons |
| name | user-visible device name               |
| fw   | firmware version                       |
| proto| protocol version (`1`)                 |

Hostname: `deskhud-<last 6 hex of MAC>.local`.

## Transport

* WebSocket: `ws://<device>/ws`, text frames, one JSON object per frame, every object has `"t"` (type).
* HTTP API: `http://<device>/api/...`, authenticated with `Authorization: Bearer <token>`.
* Max inbound frame: 16 KiB. Strings sent by PCs are truncated by the device if longer than the
  limits below; the monitoring service truncates first so nothing is cut mid-character.

## Session

```
PC                                   display
 |-- hello ------------------------->|
 |<----------------------- welcome --|   token valid
 |<------------------- pair_pending --|   unknown PC / bad token: user is asked on screen
 |<-------------------- pair_result --|   ok + token, or ok=false (connection then closed)
 |<--------------------------- role --|   active | standby   (sent again on every change)
 |-- stats / agents / notify ------->|   active: stats at 1 Hz, agents on change (>= every 5 s)
 |-- ping ---------------------------|   standby: every 2 s (stats/agents also count as liveness)
```

After `pair_result` with `ok:true` the monitoring service sends `get`, so the display does not need to send
`welcome` as well. The monitoring service pings every 5 s while active too and reconnects when nothing has
arrived from the display for 15 s, so the display must answer every `ping` with `pong`.

### PC → display

`hello`
```json
{"t":"hello","proto":1,"pc_id":"8c6e…(uuid)","host":"workstation","os":"Arch Linux","agent":"0.1.0","token":"…or empty"}
```
Optional `"role":"control"`: a client that only changes settings (the built-in web page, tools).
It pairs like a PC but never becomes the active data source and is not listed as an online PC.

`stats` (1 Hz while active). All fields optional; absent = unknown. Bytes are raw counts, rates are bytes/s.
```json
{"t":"stats",
 "cpu":{"name":"Ryzen 9 7950X","load":12.3,"temp":54.2,"mhz":5100,"cores":[3.1,40.0],"power":88.0},
 "mem":{"used":17179869184,"total":68719476736,"swap_used":0,"swap_total":8589934592},
 "gpus":[{"name":"RTX 4090","load":5.0,"temp":40.0,"vram_used":1073741824,"vram_total":25769803776,
          "power":31.0,"power_max":450.0,"fan":30.0,"mhz":2520}],
 "disks":[{"mount":"/","used":131000000000,"total":2000000000000}],
 "io":{"read":1048576,"write":2048},
 "net":{"iface":"enp8s0","ip":"192.168.1.20","down":12000,"up":3000},
 "load":[1.02,0.88,0.71],"uptime":12345,"procs":[{"name":"firefox","cpu":12.5,"mem":1234567890}]}
```

Additions in protocol 1.1 (all optional; displays ignore what they don't know):

* `hello` and `stats` carry `"tz"`: the PC's POSIX time zone (the footer of `/etc/localtime`, e.g.
  `"IST-5:30"`), so the display can follow the active PC's local time.
* `stats.mem` adds `available`, `cached` and `free` (bytes).
* `stats.net` adds `rx_total` and `tx_total`: bytes since boot on that interface.
* `stats.procs` is up to 16 entries: `{"pid":2298,"name":"ttlcd","cpu":0.1,"mem":57671680,"threads":6}`,
  sorted by CPU, then memory.
* `stats.cpu` adds `temps`: extra named temperatures when the sensor has them (AMD CCDs),
  e.g. `[{"name":"CCD1","temp":44.0}]`.

`agents` (on change, and at least every 5 s while active). Full replacement list, most relevant first, ≤ 8 sessions.
```json
{"t":"agents","sessions":[{
  "id":"stable session id","agent":"claude|codex|pi","project":"ttlcd","title":"Fix the build",
  "state":"working|permission|question|done|error|idle",
  "activity":"Bash · cargo build --release",
  "prompt":"question or permission request text, when state is permission/question",
  "summary":"final answer of the last finished turn",
  "ask":"the person's latest message (turn prompt or a follow-up typed while it worked)",
  "model":"opus-5.5","ctx":65442,"ctx_max":1000000,"out":1996,
  "turn_start":1790875018,"updated":1790875045,
  "lines":[{"k":"user","s":"fix the parser"},{"k":"tool","s":"Read · agents.rs","i":"toolu_01A"},
           {"k":"tool","s":"Bash · Run the tests","d":"test result: ok. 12 passed","i":"toolu_01B"},
           {"k":"text","s":"Looking at the parser."}]}]}
```
Line kinds: `user` (something the person typed), `tool` (a tool call), `text` (agent prose).
Text is written for a screen, not a terminal: Claude's own one-line tool descriptions are used when
known (`Bash · Build firmware and deploy`), the home directory shows as `~`, `cd … &&` prefixes and
variable assignments are dropped, file tools show the file name, and prose loses its markdown
(one paragraph per line).
Tool lines may carry `d`, what the call changed or printed (an edit as `- old` / `+ new` lines, the
start of a file written, or the output; ≤ 12 lines / 400 chars), and `i`, the call's id, which the
display uses to ask for the full text (`full`, below).
Limits: title 120 chars, activity 160, prompt 400, ask 300, summary 600, `lines` ≤ 40 entries of ≤ 400 chars.
The monitoring service keeps the whole frame under 36 KiB (dropping lines of lower sessions first);
the display accepts frames up to 40 KiB. `id` is
`<agent>:<session id>`; `updated` and `turn_start` are unix seconds.

| state      | meaning                                                         |
|------------|-----------------------------------------------------------------|
| working    | running a turn                                                  |
| permission | blocked on a tool-permission prompt                             |
| question   | asked the user something (AskUserQuestion, plan approval, idle prompt) |
| done       | turn finished; `summary` holds the answer                       |
| error      | turn failed (API error, crash)                                  |
| idle       | interrupted / stalled / session open with nothing happening     |

`notify` — toast on the display (`deskhud notify`).
```json
{"t":"notify","level":"info|warn|alert","title":"Build finished","body":"ttlcd release in 41 s","ttl":8}
```

`set` — change settings (any subset). Reply: `settings`.
```json
{"t":"set","settings":{"brightness":70}}
```

`get` — `{"t":"get"}` → `settings` + `device`.

`cmd` — `{"t":"cmd","cmd":"activate|pin|unpin|reboot|identify|forget|page","arg":"…"}`
* `activate`: make the sending PC active (and pin it if `arg` is `"pin"`); any other `arg` is the pc_id to show.
* `pin` / `unpin`: pin PC `arg` / clear the pin.
* `identify`: flash the screen with the device name for 3 s.
* `forget`: unpair PC `arg` (pc_id).
* `page`: show page `arg` (`overview` / `agents` / `system` / `settings`).

`ping` — `{"t":"ping"}` → `{"t":"pong"}`.

### Display → PC

`welcome` `{"t":"welcome","device":{…device…},"settings":{…settings…}}`

`pair_pending` `{"t":"pair_pending","code":"482913"}` — the same code is shown on the display.

`pair_result` `{"t":"pair_result","ok":true,"token":"64 hex chars"}`

`role` `{"t":"role","active":true,"active_host":"workstation"}`

`settings` `{"t":"settings","settings":{…}}`

`device`
```json
{"id":"441bf6ca22a0","name":"DeskHUD","fw":"0.1.0","idf":"v5.5.5","w":1024,"h":600,
 "ip":"192.168.1.40","rssi":-52,"ssid":"home","heap":123456,"psram":7000000,"uptime":3600,
 "pcs":[{"pc_id":"…","host":"workstation","online":true,"active":true}]}
```

`error` `{"t":"error","msg":"…"}`

`full` `{"t":"full","req":7,"session":"claude:…","tool":"toolu_01B"}` — the complete text behind a
shortened line, for the popup: a tool call by its `i` (the whole edit, file or command output), or
with an empty `tool` the session's last reply. Sent to the active PC only. The PC answers
`{"t":"full","req":7,"text":"…"}` (≤ 16 KiB; empty when it has nothing more); the display ignores
replies to anything but its latest request.

`sysinfo` `{"t":"sysinfo","req":8}` — the PC's system info for the popup behind its name. The PC
answers `{"t":"sysinfo","req":8,"sections":[{"title":"Hardware","rows":[{"icon":"\uf4bc","key":"CPU",
"value":"AMD Ryzen 9 9950X3D (32) @ 5.76 GHz","colors":[]}]}],"logo":["████ …"]}`, built from
`fastfetch` with the user's config: Nerd Font icons, swatch colours as `#rrggbb`, the logo as text
lines (block art). `{"error":"…"}` when fastfetch is missing.

`procs` `{"t":"procs","req":9,"n":40}` — the process table popup. The PC answers
`{"t":"procs","req":9,"count":586,"procs":[{"pid":2339,"name":"ttlcd","cmd":"ttlcd daemon",
"threads":6,"user":"kunal","mem":68681728,"cpu":0.2}]}`, sorted by CPU (percent of the whole
machine since the previous request), then memory. Replies of both are ≤ 16 KiB.

## Settings

| key            | type   | default     | meaning                                                   |
|----------------|--------|-------------|-----------------------------------------------------------|
| name           | string | `DeskHUD`   | device name (mDNS TXT, screens)                           |
| brightness     | 5–100  | 80          | backlight while showing data                              |
| dim_brightness | 0–100  | 30          | backlight when no PC is active; 0 = off                   |
| dim_after      | s      | 86400       | idle seconds without an active PC before dimming; >= 86400 = never (default) |
| wake_on_alert  | bool   | true        | full brightness + banner when an agent needs input        |
| accent         | hex    | `#7aa2f7`   | accent color                                              |
| theme          | string | `midnight`  | `midnight` / `graphite` / `aurora` / `paper` / `ink` / `newsprint`              |
| temp_unit      | `C`/`F`| `C`         |                                                           |
| clock_24h      | bool   | true        |                                                           |
| tz             | string | `auto`      | POSIX TZ string, or `auto` to follow the active PC        |
| page           | string | `overview`  | start page: `overview` / `agents` / `system`              |
| auto_page      | bool   | true        | jump to Agents page when an agent needs input             |
| flip           | bool   | false       | rotate 180°                                               |
| failover       | string | `auto`      | `auto`: standby PC takes over when the active one goes silent; `manual`: only by tapping / `cmd activate` |
| pinned_pc      | string | ``          | pc_id that always wins when online                        |
| agent_text     | string | `large`     | agent text size: `small` / `medium` / `large`             |

## Active-PC selection

1. A pinned PC (`pinned_pc`) that is online is always active.
2. Otherwise the current active PC stays active while it sends anything at least every 6 s.
3. When it disconnects or goes silent for 6 s, the standby PC that was heard from most recently becomes
   active (`failover=auto`). New PCs that connect while another is active start as standby.
4. Touching a PC in the display's PC switcher, or `cmd activate`, switches immediately.

## HTTP API

| method | path            | auth | body / result                                         |
|--------|-----------------|------|-------------------------------------------------------|
| GET    | `/api/info`     | no   | `device` object (without `pcs`)                       |
| GET    | `/api/settings` | yes  | settings                                              |
| POST   | `/api/settings` | yes  | partial settings JSON → settings                      |
| POST   | `/api/ota`      | yes  | raw firmware `.bin`; reboots into it, rolls back if the new image does not come up |
| POST   | `/api/reboot`   | yes  |                                                       |
| GET    | `/`             | no   | settings web page (asks for a token or uses pairing)  |
