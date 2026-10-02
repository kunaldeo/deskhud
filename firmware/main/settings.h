#pragma once
// Persistent device settings and paired PCs (NVS). All access is serialized internally.
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#define MAX_PAIRED 8

typedef enum { FAILOVER_AUTO, FAILOVER_MANUAL } failover_t;

typedef struct {
    char name[32];
    int brightness;      // 5..100
    int dim_brightness;  // 0..100
    int dim_after;       // seconds
    bool wake_on_alert;
    uint32_t accent;     // 0xRRGGBB
    char theme[12];      // midnight | graphite | aurora | paper
    char temp_unit;      // 'C' | 'F'
    bool clock_24h;
    char tz[48];
    char page[12];       // overview | agents | system
    bool auto_page;
    bool flip;
    failover_t failover;
    char pinned_pc[40];
    // New fields go at the end: an older, shorter blob in NVS still loads (the rest keep defaults).
    int agent_text;      // agent text size: 0 small, 1 medium, 2 large
} settings_t;

typedef struct {
    char pc_id[40];
    char host[40];
    char token[65];
} paired_pc_t;

void settings_init(void);

/// Snapshot of the current settings.
settings_t settings_get(void);

/// Applies every known key present in `patch`; returns a bitmask of what changed (SET_*).
/// Invalid values are clamped. Persists and notifies listeners.
uint32_t settings_apply_json(const cJSON *patch);
cJSON *settings_to_json(void);

enum { SET_DISPLAY = 1, SET_THEME = 2, SET_CLOCK = 4, SET_NAME = 8, SET_REBOOT = 16, SET_ROUTING = 32 };
/// Callbacks run on the caller's task. They must not take the LVGL lock (the UI holds it while
/// reading the hub): UI code should only set a flag and pick the change up from its timer.
typedef void (*settings_cb_t)(uint32_t changed);
void settings_on_change(settings_cb_t cb);

// Wi-Fi credentials
bool settings_wifi(char ssid[33], char pass[65]);
void settings_set_wifi(const char *ssid, const char *pass);

// Paired PCs
bool settings_pc_find(const char *pc_id, paired_pc_t *out);
bool settings_pc_check(const char *pc_id, const char *token);
/// Adds or replaces a PC and returns its fresh token in `token_out` (65 bytes).
void settings_pc_add(const char *pc_id, const char *host, char *token_out);
void settings_pc_forget(const char *pc_id);
int settings_pc_list(paired_pc_t *out, int max);
/// Any paired PC's token (HTTP API auth).
bool settings_token_valid(const char *token);

void settings_factory_reset(void);
