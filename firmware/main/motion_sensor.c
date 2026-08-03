#include "motion_sensor.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// waveshare/qmi8658 2.0.0 defines its own float M_PI even when libc has
// already provided the standard extension. This module does not use M_PI.
#undef M_PI
#include "qmi8658.h"

#define IMU_SAMPLE_PERIOD_MS 50
#define IMU_RETRY_PERIOD_MS 1000
#define IMU_TASK_STACK_BYTES 4096
#define IMU_TASK_PRIORITY 3
#define IMU_PROBE_TIMEOUT_MS 100
#define QMI8658_RESET_REGISTER 0x60
#define QMI8658_RESET_COMMAND 0xb0
#define QMI8658_CTRL1_VALUE 0x60
#define QMI8658_RESET_DELAY_MS 20
#define GRAVITY_FILTER_ALPHA 0.06f
#define LINEAR_MOTION_THRESHOLD_MPS2 0.70f
#define ANGULAR_MOTION_THRESHOLD_DPS 12.0f
#define MOTION_EVENT_REFRACTORY_MS 350
#define KINETIC_RESPONSE_LIMIT 1.5f

static const char *TAG = "motion_sensor";

struct motion_sensor {
    portMUX_TYPE lock;
    motion_sensor_snapshot_t snapshot;
    qmi8658_dev_t imu;
    float gravity_x;
    float gravity_y;
    float gravity_z;
    bool gravity_initialized;
    uint32_t last_motion_event_ms;
};

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t detect_address(i2c_master_bus_handle_t bus,
                                uint8_t *address)
{
    static const uint8_t candidates[] = {
        QMI8658_ADDRESS_HIGH,
        QMI8658_ADDRESS_LOW,
    };
    for (size_t index = 0;
            index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        const esp_err_t error = i2c_master_probe(
            bus, candidates[index], IMU_PROBE_TIMEOUT_MS);
        if (error == ESP_OK) {
            *address = candidates[index];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t configure_imu(motion_sensor_t *sensor,
                               i2c_master_bus_handle_t bus)
{
    uint8_t address = 0;
    esp_err_t error = detect_address(bus, &address);
    if (error != ESP_OK) {
        return error;
    }

    error = qmi8658_init(&sensor->imu, bus, address);
    if (error != ESP_OK) {
        return error;
    }
    error = qmi8658_write_register(&sensor->imu,
                                   QMI8658_RESET_REGISTER,
                                   QMI8658_RESET_COMMAND);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(QMI8658_RESET_DELAY_MS));
    error = qmi8658_write_register(&sensor->imu, QMI8658_CTRL1,
                                   QMI8658_CTRL1_VALUE);
    if (error != ESP_OK) {
        return error;
    }
    error = qmi8658_set_accel_range(&sensor->imu,
                                    QMI8658_ACCEL_RANGE_4G);
    if (error == ESP_OK) {
        error = qmi8658_set_accel_odr(&sensor->imu,
                                      QMI8658_ACCEL_ODR_250HZ);
    }
    if (error == ESP_OK) {
        error = qmi8658_set_gyro_range(&sensor->imu,
                                       QMI8658_GYRO_RANGE_256DPS);
    }
    if (error == ESP_OK) {
        error = qmi8658_set_gyro_odr(&sensor->imu,
                                     QMI8658_GYRO_ODR_250HZ);
    }
    if (error != ESP_OK) {
        return error;
    }
    qmi8658_set_accel_unit_mps2(&sensor->imu, true);
    qmi8658_set_gyro_unit_dps(&sensor->imu, true);
    error = qmi8658_enable_sensors(
        &sensor->imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    if (error != ESP_OK) {
        return error;
    }

    uint8_t who_am_i = 0;
    error = qmi8658_get_who_am_i(&sensor->imu, &who_am_i);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 ready address=0x%02x who_am_i=0x%02x",
                 address, who_am_i);
    }
    return error;
}

static void publish_read_error(motion_sensor_t *sensor)
{
    portENTER_CRITICAL(&sensor->lock);
    sensor->snapshot.read_errors++;
    portEXIT_CRITICAL(&sensor->lock);
}

static void process_sample(motion_sensor_t *sensor,
                           const qmi8658_data_t *data)
{
    if (!sensor->gravity_initialized) {
        sensor->gravity_x = data->accelX;
        sensor->gravity_y = data->accelY;
        sensor->gravity_z = data->accelZ;
        sensor->gravity_initialized = true;
    }

    sensor->gravity_x += (data->accelX - sensor->gravity_x)
        * GRAVITY_FILTER_ALPHA;
    sensor->gravity_y += (data->accelY - sensor->gravity_y)
        * GRAVITY_FILTER_ALPHA;
    sensor->gravity_z += (data->accelZ - sensor->gravity_z)
        * GRAVITY_FILTER_ALPHA;

    const float linear_x = data->accelX - sensor->gravity_x;
    const float linear_y = data->accelY - sensor->gravity_y;
    const float linear_z = data->accelZ - sensor->gravity_z;
    const float linear_magnitude = sqrtf(
        linear_x * linear_x + linear_y * linear_y
        + linear_z * linear_z);
    const float gyro_magnitude = sqrtf(
        data->gyroX * data->gyroX + data->gyroY * data->gyroY
        + data->gyroZ * data->gyroZ);
    const float linear_score = linear_magnitude
        / LINEAR_MOTION_THRESHOLD_MPS2;
    const float angular_score = gyro_magnitude
        / ANGULAR_MOTION_THRESHOLD_DPS;
    const float motion_score = fmaxf(linear_score, angular_score);
    // The panel is software-rotated 90 degrees from the board's native axes:
    // native +Y becomes landscape +X and native +X becomes landscape -Y.
    // Use only the high-pass acceleration so a new resting angle naturally
    // settles rather than pinning the character's gaze indefinitely.
    const float kinetic_x = fmaxf(-KINETIC_RESPONSE_LIMIT,
        fminf(KINETIC_RESPONSE_LIMIT,
              linear_y / LINEAR_MOTION_THRESHOLD_MPS2));
    const float kinetic_y = fmaxf(-KINETIC_RESPONSE_LIMIT,
        fminf(KINETIC_RESPONSE_LIMIT,
              -linear_x / LINEAR_MOTION_THRESHOLD_MPS2));
    const uint32_t now = monotonic_ms();
    const bool event = motion_score >= 1.0f
        && now - sensor->last_motion_event_ms
            >= MOTION_EVENT_REFRACTORY_MS;
    if (event) {
        sensor->last_motion_event_ms = now;
    }

    portENTER_CRITICAL(&sensor->lock);
    sensor->snapshot.ready = true;
    sensor->snapshot.motion_score = motion_score;
    sensor->snapshot.kinetic_x = kinetic_x;
    sensor->snapshot.kinetic_y = kinetic_y;
    sensor->snapshot.accel_x = data->accelX;
    sensor->snapshot.accel_y = data->accelY;
    sensor->snapshot.accel_z = data->accelZ;
    sensor->snapshot.gyro_dps = gyro_magnitude;
    if (event) {
        sensor->snapshot.motion_events++;
    }
    portEXIT_CRITICAL(&sensor->lock);
}

static void motion_sensor_task(void *opaque_sensor)
{
    motion_sensor_t *sensor = opaque_sensor;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus unavailable");
        publish_read_error(sensor);
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t configure_error = configure_imu(sensor, bus);
    if (configure_error != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 initialization failed: %s",
                 esp_err_to_name(configure_error));
        publish_read_error(sensor);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        bool ready = false;
        esp_err_t error = qmi8658_is_data_ready(&sensor->imu, &ready);
        if (error == ESP_OK && ready) {
            qmi8658_data_t data = {0};
            error = qmi8658_read_sensor_data(&sensor->imu, &data);
            if (error == ESP_OK) {
                process_sample(sensor, &data);
            }
        }
        if (error != ESP_OK) {
            publish_read_error(sensor);
            vTaskDelay(pdMS_TO_TICKS(IMU_RETRY_PERIOD_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
        }
    }
}

esp_err_t motion_sensor_create(motion_sensor_t **out_sensor)
{
    if (out_sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    motion_sensor_t *sensor = calloc(1, sizeof(*sensor));
    if (sensor == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portMUX_INITIALIZE(&sensor->lock);

    const BaseType_t created = xTaskCreate(
        motion_sensor_task, "walle_motion", IMU_TASK_STACK_BYTES,
        sensor, IMU_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        free(sensor);
        return ESP_ERR_NO_MEM;
    }
    *out_sensor = sensor;
    return ESP_OK;
}

void motion_sensor_get_snapshot(motion_sensor_t *sensor,
                                motion_sensor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (sensor == NULL) {
        *snapshot = (motion_sensor_snapshot_t) {0};
        return;
    }
    portENTER_CRITICAL(&sensor->lock);
    *snapshot = sensor->snapshot;
    portEXIT_CRITICAL(&sensor->lock);
}
