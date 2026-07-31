#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define SAMPLE_RATE_HZ 24000
#define BITS_PER_SAMPLE 16
#define CHANNEL_COUNT 1
#define FRAME_SAMPLES 256
#define SETTLE_FRAMES 32
#define SOAK_SECONDS 30
#define OUTPUT_VOLUME_PERCENT 20
#define INPUT_GAIN_DB 24.0f

static const char *TAG = "display_audio_soak";

static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static volatile bool s_rx_done;
static volatile bool s_tx_done;
static uint32_t s_read_errors;
static uint32_t s_write_errors;
static uint32_t s_read_max_us;
static uint32_t s_write_max_us;
static uint64_t s_sum_squares;
static uint64_t s_sample_count;
static uint32_t s_peak;
static int64_t s_rx_elapsed_us;
static int64_t s_tx_elapsed_us;

static esp_codec_dev_sample_info_t sample_info(void)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNEL_COUNT,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE_HZ,
        .mclk_multiple = 256,
    };
}

static esp_err_t initialize_codec(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_handle, &s_rx_handle),
                        TAG, "create duplex I2S channels");

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &i2s_config),
                        TAG, "initialize I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &i2s_config),
                        TAG, "initialize I2S RX");

    audio_codec_i2s_cfg_t data_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create I2S codec interface");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize board I2C");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec GPIO interface");

    audio_codec_i2c_cfg_t control_config = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *control_if =
        audio_codec_new_i2c_ctrl(&control_config);
    ESP_RETURN_ON_FALSE(control_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec control interface");

    const esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create ES8311 interface");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG,
                        "create duplex codec device");

    esp_codec_dev_sample_info_t format = sample_info();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &format) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open duplex codec");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_vol(s_codec, OUTPUT_VOLUME_PERCENT) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "set output volume");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_in_gain(s_codec, INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "set input gain");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_mute(s_codec, true) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "mute output");
    return ESP_OK;
}

static void update_microphone_metrics(const int16_t *samples, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        const int32_t value = samples[index];
        const uint32_t magnitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
        s_sum_squares += (uint64_t)((int64_t)value * value);
        s_sample_count++;
        if (magnitude > s_peak) {
            s_peak = magnitude;
        }
    }
}

static void microphone_task(void *unused)
{
    (void)unused;
    int16_t samples[FRAME_SAMPLES];
    for (int frame = 0; frame < SETTLE_FRAMES; frame++) {
        if (esp_codec_dev_read(s_codec, samples, sizeof(samples)) != ESP_CODEC_DEV_OK) {
            s_read_errors++;
        }
    }

    const int total_frames = (SAMPLE_RATE_HZ * SOAK_SECONDS) / FRAME_SAMPLES;
    const int64_t started_us = esp_timer_get_time();
    for (int frame = 0; frame < total_frames; frame++) {
        const int64_t call_started_us = esp_timer_get_time();
        const int result = esp_codec_dev_read(s_codec, samples, sizeof(samples));
        const uint32_t call_us = (uint32_t)(esp_timer_get_time() - call_started_us);
        if (call_us > s_read_max_us) {
            s_read_max_us = call_us;
        }
        if (result != ESP_CODEC_DEV_OK) {
            s_read_errors++;
        } else {
            update_microphone_metrics(samples, FRAME_SAMPLES);
        }
    }
    s_rx_elapsed_us = esp_timer_get_time() - started_us;
    s_rx_done = true;
    vTaskDelete(NULL);
}

static void speaker_task(void *unused)
{
    (void)unused;
    int16_t samples[FRAME_SAMPLES];
    memset(samples, 0, sizeof(samples));
    const int total_frames = (SAMPLE_RATE_HZ * SOAK_SECONDS) / FRAME_SAMPLES;
    const int64_t started_us = esp_timer_get_time();
    for (int frame = 0; frame < total_frames; frame++) {
        const int64_t call_started_us = esp_timer_get_time();
        const int result = esp_codec_dev_write(s_codec, samples, sizeof(samples));
        const uint32_t call_us = (uint32_t)(esp_timer_get_time() - call_started_us);
        if (call_us > s_write_max_us) {
            s_write_max_us = call_us;
        }
        if (result != ESP_CODEC_DEV_OK) {
            s_write_errors++;
        }
    }
    s_tx_elapsed_us = esp_timer_get_time() - started_us;
    s_tx_done = true;
    vTaskDelete(NULL);
}

static face_t *start_face(void)
{
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        return NULL;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(45));
    if (!bsp_display_lock(1000)) {
        return NULL;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    face_t *face = face_create(screen);
    bsp_display_unlock();
    return face;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting %d-second display/audio soak", SOAK_SECONDS);
    ESP_LOGI(TAG, "TX is silence and speaker output is muted");
    ESP_ERROR_CHECK(start_face() == NULL ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(initialize_codec());

    uint32_t minimum_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t minimum_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    BaseType_t rx_created = xTaskCreatePinnedToCore(
        microphone_task, "soak_microphone", 4096, NULL, 6, NULL, 0);
    BaseType_t tx_created = xTaskCreatePinnedToCore(
        speaker_task, "soak_speaker", 4096, NULL, 6, NULL, 1);
    ESP_ERROR_CHECK(rx_created == pdPASS && tx_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    uint32_t elapsed_seconds = 0;
    while (!s_rx_done || !s_tx_done) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_seconds++;
        const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        minimum_internal = LV_MIN(minimum_internal, internal);
        minimum_psram = LV_MIN(minimum_psram, psram);
        ESP_LOGI(TAG, "progress=%" PRIu32 "s rx_done=%d tx_done=%d internal=%" PRIu32
                 " psram=%" PRIu32,
                 elapsed_seconds, s_rx_done, s_tx_done, internal, psram);
    }

    const double rms = s_sample_count == 0
        ? 0.0
        : sqrt((double)s_sum_squares / s_sample_count);
    ESP_LOGI(TAG, "audio samples=%llu rms=%.1f peak=%" PRIu32,
             (unsigned long long)s_sample_count, rms, s_peak);
    ESP_LOGI(TAG, "audio read_errors=%" PRIu32 " write_errors=%" PRIu32
             " read_max=%" PRIu32 "us write_max=%" PRIu32 "us",
             s_read_errors, s_write_errors, s_read_max_us, s_write_max_us);
    ESP_LOGI(TAG, "audio rx_elapsed=%lldms tx_elapsed=%lldms",
             (long long)(s_rx_elapsed_us / 1000),
             (long long)(s_tx_elapsed_us / 1000));
    ESP_LOGI(TAG, "heap minimum_internal=%" PRIu32 " minimum_psram=%" PRIu32,
             minimum_internal, minimum_psram);

    const int64_t minimum_duration_us = (SOAK_SECONDS - 1) * 1000000LL;
    const int64_t maximum_duration_us = (SOAK_SECONDS + 5) * 1000000LL;
    const bool passed = s_read_errors == 0 && s_write_errors == 0
        && s_sample_count > 0
        && s_rx_elapsed_us >= minimum_duration_us
        && s_rx_elapsed_us <= maximum_duration_us
        && s_tx_elapsed_us >= minimum_duration_us
        && s_tx_elapsed_us <= maximum_duration_us;
    ESP_LOGI(TAG, "DISPLAY_AUDIO_SOAK_RESULT=%s", passed ? "PASS" : "FAIL");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
