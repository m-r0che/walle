#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define STRIP_ROWS 20
#define STRESS_TRANSFERS 1000
#define STRESS_TRANSFER_GAP_MS 20
#define TRANSFER_TIMEOUT_MS 1000
#define DISPLAY_BRIGHTNESS_PERCENT 45

static const char *TAG = "display_transfer";
static SemaphoreHandle_t s_transfer_done;
static volatile uint32_t s_completed;
static uint32_t s_submitted;
static uint32_t s_timeouts;
static uint32_t s_submit_errors;
static int64_t s_total_transfer_us;
static int64_t s_max_transfer_us;

static bool IRAM_ATTR transfer_done_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context)
{
    BaseType_t wake = pdFALSE;
    s_completed++;
    xSemaphoreGiveFromISR(s_transfer_done, &wake);
    return wake == pdTRUE;
}

static uint16_t rgb565_wire(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint16_t pixel = ((uint16_t)(red >> 3) << 11)
        | ((uint16_t)(green >> 2) << 5)
        | (blue >> 3);
    return __builtin_bswap16(pixel);
}

static void fill_solid(uint16_t *pixels, size_t count, uint16_t color)
{
    for (size_t index = 0; index < count; index++) {
        pixels[index] = color;
    }
}

static void fill_final_pattern(uint16_t *pixels, int y_start, int rows)
{
    const uint16_t cyan = rgb565_wire(80, 255, 255);
    const uint16_t deep_cyan = rgb565_wire(0, 70, 82);
    const uint16_t amber = rgb565_wire(255, 174, 56);
    const uint16_t black = rgb565_wire(0, 0, 0);

    for (int row = 0; row < rows; row++) {
        const int y = y_start + row;
        for (int x = 0; x < BSP_LCD_H_RES; x++) {
            uint16_t color = black;
            if (x < 5 || x >= BSP_LCD_H_RES - 5
                    || y < 5 || y >= BSP_LCD_V_RES - 5) {
                color = cyan;
            } else if (y >= 104 && y < 164 && x >= 45 && x < 155) {
                color = deep_cyan;
            } else if (y >= 104 && y < 164 && x >= 213 && x < 323) {
                color = deep_cyan;
            } else if (y >= 126 && y < 142
                       && ((x >= 88 && x < 112)
                           || (x >= 256 && x < 280))) {
                color = cyan;
            } else if (y >= 286 && y < 300 && x >= 125 && x < 243) {
                color = amber;
            }
            pixels[row * BSP_LCD_H_RES + x] = color;
        }
    }
}

static bool submit_strip(esp_lcd_panel_handle_t panel, uint16_t *pixels,
                         int y_start, int rows)
{
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
    }

    const int64_t started_us = esp_timer_get_time();
    s_submitted++;
    const esp_err_t error = esp_lcd_panel_draw_bitmap(
        panel, 0, y_start, BSP_LCD_H_RES, y_start + rows, pixels);
    if (error != ESP_OK) {
        s_submit_errors++;
        ESP_LOGE(TAG, "submit=%u y=%d rows=%d failed: %s",
                 (unsigned)s_submitted, y_start, rows,
                 esp_err_to_name(error));
        return false;
    }

    if (xSemaphoreTake(s_transfer_done,
                       pdMS_TO_TICKS(TRANSFER_TIMEOUT_MS)) != pdTRUE) {
        s_timeouts++;
        ESP_LOGE(TAG, "submit=%u timed out completed=%u",
                 (unsigned)s_submitted, (unsigned)s_completed);
        return false;
    }

    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    s_total_transfer_us += elapsed_us;
    if (elapsed_us > s_max_transfer_us) {
        s_max_transfer_us = elapsed_us;
    }
    return true;
}

static bool draw_final_frame(esp_lcd_panel_handle_t panel, uint16_t *pixels)
{
    for (int y = 0; y < BSP_LCD_V_RES; y += STRIP_ROWS) {
        const int rows = (y + STRIP_ROWS <= BSP_LCD_V_RES)
            ? STRIP_ROWS : BSP_LCD_V_RES - y;
        fill_final_pattern(pixels, y, rows);
        if (!submit_strip(panel, pixels, y, rows)) {
            return false;
        }
    }
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting finite QSPI transfer-ownership diagnostic");

    s_transfer_done = xSemaphoreCreateBinary();
    if (s_transfer_done == NULL) {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL semaphore allocation");
        return;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t error = bsp_display_new(NULL, &panel, &io);
    if (error != ESP_OK || panel == NULL || io == NULL) {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL panel init: %s",
                 esp_err_to_name(error));
        return;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = transfer_done_callback,
    };
    error = esp_lcd_panel_io_register_event_callbacks(io, &callbacks, NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL callback registration: %s",
                 esp_err_to_name(error));
        return;
    }

    error = bsp_display_brightness_set(DISPLAY_BRIGHTNESS_PERCENT);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL brightness: %s",
                 esp_err_to_name(error));
        return;
    }

    const size_t pixel_count = BSP_LCD_H_RES * STRIP_ROWS;
    uint16_t *pixels = heap_caps_aligned_alloc(
        64, pixel_count * sizeof(*pixels),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL DMA allocation");
        return;
    }

    bool passed = draw_final_frame(panel, pixels);
    for (uint32_t frame = 0; passed && frame < STRESS_TRANSFERS; frame++) {
        const uint16_t color = (frame & 1)
            ? rgb565_wire(0, 92, 108)
            : rgb565_wire(255, 174, 56);
        fill_solid(pixels, pixel_count, color);
        const int y = (frame * 7) % (BSP_LCD_V_RES - STRIP_ROWS);
        passed = submit_strip(panel, pixels, y, STRIP_ROWS);
        vTaskDelay(pdMS_TO_TICKS(STRESS_TRANSFER_GAP_MS));
    }
    if (passed) {
        passed = draw_final_frame(panel, pixels);
    }

    const int64_t average_us = s_submitted == 0
        ? 0 : s_total_transfer_us / s_submitted;
    ESP_LOGI(TAG,
             "submitted=%u completed=%u timeouts=%u submit_errors=%u avg=%lldus max=%lldus internal=%u psram=%u",
             (unsigned)s_submitted, (unsigned)s_completed,
             (unsigned)s_timeouts, (unsigned)s_submit_errors,
             (long long)average_us, (long long)s_max_transfer_us,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (passed && s_submitted == s_completed
            && s_timeouts == 0 && s_submit_errors == 0) {
        ESP_LOGI(TAG, "DISPLAY_TRANSFER_RESULT=PASS");
    } else {
        ESP_LOGE(TAG, "DISPLAY_TRANSFER_RESULT=FAIL");
    }

    ESP_LOGI(TAG, "Diagnostic complete; holding the final static pattern");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "DISPLAY_TRANSFER_HOLD submitted=%u completed=%u result=%s",
                 (unsigned)s_submitted, (unsigned)s_completed,
                 passed && s_submitted == s_completed
                     && s_timeouts == 0 && s_submit_errors == 0
                     ? "PASS" : "FAIL");
    }
}
