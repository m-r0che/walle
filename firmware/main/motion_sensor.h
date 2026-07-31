#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct motion_sensor motion_sensor_t;

typedef struct {
    bool ready;
    uint32_t motion_events;
    uint32_t read_errors;
    float motion_score;
    float accel_x;
    float accel_y;
    float accel_z;
    float gyro_dps;
} motion_sensor_snapshot_t;

/**
 * Start the board's QMI8658 sampler on the BSP-owned I2C bus.
 *
 * Initialization and sampling happen in a low-priority task. Failure leaves
 * the rest of the robot operational and is reflected in the snapshot.
 */
esp_err_t motion_sensor_create(motion_sensor_t **out_sensor);

/** Read the latest bounded aggregate; no raw sample history is retained. */
void motion_sensor_get_snapshot(motion_sensor_t *sensor,
                                motion_sensor_snapshot_t *snapshot);
