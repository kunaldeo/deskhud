#pragma once
// PC connections and the data they send. Network tasks feed it; the UI reads snapshots.
//
// Exactly one connected PC is *active* (its data is shown). Others are standby and only keep a
// heartbeat. See docs/PROTOCOL.md "Active-PC selection".
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define MAX_CORES 64
#define MAX_GPUS 2
#define MAX_DISKS 4
#define MAX_PROCS 16
#define MAX_TEMPS 4
#define MAX_AGENTS 8
#define MAX_LINES 40
#define MAX_CONNS 6
#define HISTORY 120

typedef struct {
    char cpu_name[64];
    float cpu_load, cpu_temp, cpu_power;  // NAN = unknown
    int ntemps;
    struct {
        char name[12];
        float temp;
    } temps[MAX_TEMPS];  // extra sensors (AMD CCDs)
    int cpu_mhz, ncores;
    float cores[MAX_CORES];
    uint64_t mem_used, mem_total, swap_used, swap_total, mem_available, mem_cached, mem_free;
    int ngpus;
    struct {
        char name[48];
        float load, temp, power, power_max, fan;
        int mhz;
        uint64_t vram_used, vram_total;
    } gpus[MAX_GPUS];
    int ndisks;
    struct {
        char mount[32];
        uint64_t used, total;
    } disks[MAX_DISKS];
    double io_read, io_write, net_down, net_up;
    uint64_t rx_total, tx_total;
    char iface[16], ip[16];
    float load[3];
    uint32_t uptime;
    int nprocs;
    struct {
        char name[32];
        float cpu;
        uint64_t mem;
        int pid, threads;
    } procs[MAX_PROCS];
} stats_t;

enum { LINE_TEXT, LINE_TOOL, LINE_USER };

typedef enum { AG_WORKING, AG_PERMISSION, AG_QUESTION, AG_DONE, AG_ERROR, AG_IDLE } ag_state_t;

typedef struct {
    char id[96];
    char agent[12];  // claude | codex | pi | ...
    char project[48];
    char title[124];
    ag_state_t state;
    char activity[164];
    char prompt[404];
    char ask[304];  // the person's latest message to the agent
    char summary[604];
    char model[32];
    uint64_t ctx, ctx_max, out;
    int64_t turn_start, updated;
    int nlines;
    struct {
        uint8_t kind;  // LINE_*
        char s[404];
        char d[404];  // tool lines: the edit, the file written, or the output (may be empty)
        char i[40];   // tool lines: the call's id, to fetch its full output (may be empty)
        char f[40];   // tool lines: the file it reads or writes, for syntax highlighting (may be empty)
    } lines[MAX_LINES];
} agent_t;

typedef struct {
    float cpu[HISTORY], gpu[HISTORY], down[HISTORY], up[HISTORY];
    int len;  // valid samples (oldest first)
} history_t;

typedef struct {
    char pc_id[40];
    char host[40];
    char os[40];
    bool online, active, paired;
} pc_info_t;

typedef struct {
    // Bumped whenever the matching data changes; the UI compares against what it last drew.
    uint32_t stats_ver, agents_ver, pcs_ver;
    bool has_active;
    char active_host[40];
    char active_os[40];
} hub_versions_t;

void hub_init(void);

// ---- network side (server.c)
void hub_on_open(int fd);
void hub_on_close(int fd);
void hub_on_message(int fd, const char *data, int len);
/// Sends JSON text to a connection (implemented in server.c).
void server_send(int fd, const char *text);
void server_close(int fd);

// ---- UI side
hub_versions_t hub_versions(void);
/// Copies of the active PC's data (zeroed when none). Agents are most relevant first.
void hub_stats(stats_t *out, history_t *hist);
int hub_agents(agent_t *out, int max);

/// Ask the active PC for the full text of a tool call's output (`tool` = its id) or, with an
/// empty `tool`, of the session's result. Returns a request number, 0 when no PC is active.
int hub_request_full(const char *session, const char *tool);
/// The reply to request `req`, once it has arrived (valid until the next request); else NULL.
const char *hub_full_result(int req);
/// Ask the active PC for a popup's data: "sysinfo" (fastfetch) or "procs" (the process table).
/// The reply arrives as its whole JSON message through hub_full_result.
int hub_request(const char *kind);
int hub_pcs(pc_info_t *out, int max);
/// Switch the active PC by pc_id (touch PC switcher).
void hub_activate(const char *pc_id, bool pin);
/// Approve / reject the pending pairing request.
void hub_pair_respond(bool allow);
/// Pending pairing request, if any: host name and the 6-digit code.
bool hub_pair_pending(char host[40], char code[7]);

typedef struct {
    int level;  // 0 info, 1 warn, 2 alert
    char title[80];
    char body[200];
    int ttl;
} toast_t;
/// Pops the next queued toast; false when empty.
bool hub_next_toast(toast_t *out);
void hub_push_toast(int level, const char *title, const char *body, int ttl);

/// POSIX time zone reported by the active PC ("" when unknown).
void hub_active_tz(char out[48]);

/// Page requested by a PC (`cmd page <name>`): 0 overview, 1 agents, 2 system, 3 settings; -1 none.
int hub_take_page_request(void);

/// Called by the UI when a device setting changed so connected PCs see it.
void hub_broadcast_settings(void);
cJSON *hub_device_json(bool with_pcs);
