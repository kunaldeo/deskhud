#pragma once
#include <stdbool.h>

void server_start(void);

typedef struct {
    bool active;
    int percent;    // -1 while waiting for the first bytes
    char from[40];  // who is uploading
} ota_progress_t;
ota_progress_t server_ota_progress(void);
