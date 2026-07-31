#pragma once

#include <stdint.h>

#include "lvgl.h"

typedef struct {
    uint32_t submitted;
    uint32_t completed;
    uint32_t submit_errors;
    uint32_t overlap_errors;
    uint32_t pacing_delays;
    uint32_t pacing_wait_ms;
} display_port_snapshot_t;

/** Start LVGL on the QSPI CO5300 with transfer-complete buffer ownership. */
lv_display_t *display_port_start(void);

/** Read lock-free transfer telemetry. */
void display_port_get_snapshot(display_port_snapshot_t *snapshot);
