#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { NET_NO_CONFIG, NET_CONNECTING, NET_CONNECTED, NET_FAILED } net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    char ip[16];
    int rssi;
    bool time_synced;
    char hostname[32];
    char id[13];  // MAC without colons
} net_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} net_ap_t;

void net_init(void);
net_status_t net_status(void);
/// Saves credentials and (re)connects.
void net_connect(const char *ssid, const char *pass);
/// Blocking scan (~2 s); returns number of unique APs written, strongest first.
int net_scan(net_ap_t *out, int max);
/// Re-announces mDNS (after a rename).
void net_update_mdns(void);
/// Applies the configured POSIX timezone.
void net_apply_tz(void);
