#include "display_port.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "src/draw/sw/lv_draw_sw_utils.h"

// The CO5300 loses visible state under sustained or bursty partial writes even
// when every QSPI completion callback fires. TLS leaves insufficient contiguous
// internal RAM for the former 220-row buffer, so use two 110-row regions and
// pace every submission to the known-visible ~28-transfer/second envelope.
#define DISPLAY_DRAW_ROWS 110
#define MIN_TRANSFER_INTERVAL_US 35000
#define DISPLAY_BRIGHTNESS_PERCENT 45

static const char *TAG = "display_port";

typedef struct {
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    uint16_t *draw_buffer;
    atomic_bool transfer_in_flight;
    atomic_uint submitted;
    atomic_uint completed;
    atomic_uint submit_errors;
    atomic_uint overlap_errors;
    atomic_uint pacing_delays;
    atomic_uint pacing_wait_ms;
    int64_t last_submission_us;
} display_port_t;

static display_port_t s_port;

static void round_invalidated_area(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    if (area == NULL) {
        return;
    }
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void wake_display_task(lv_event_t *event)
{
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

static bool IRAM_ATTR transfer_done_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context)
{
    display_port_t *port = user_context;
    atomic_store_explicit(&port->transfer_in_flight, false,
                          memory_order_release);
    atomic_fetch_add_explicit(&port->completed, 1, memory_order_relaxed);
    lv_display_flush_ready(port->display);
    return false;
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
                          uint8_t *color_map)
{
    display_port_t *port = lv_display_get_driver_data(display);
    const uint32_t pixel_count = (uint32_t)lv_area_get_size(area);

    int64_t now_us = esp_timer_get_time();
    const int64_t wait_us = port->last_submission_us + MIN_TRANSFER_INTERVAL_US
        - now_us;
    if (wait_us > 0) {
        atomic_fetch_add_explicit(&port->pacing_delays, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&port->pacing_wait_ms,
                                  (uint32_t)((wait_us + 999) / 1000),
                                  memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        now_us = esp_timer_get_time();
    }

    if (atomic_exchange_explicit(&port->transfer_in_flight, true,
                                 memory_order_acq_rel)) {
        atomic_fetch_add_explicit(&port->overlap_errors, 1,
                                  memory_order_relaxed);
        ESP_LOGE(TAG, "LVGL reused display buffer before transfer completion");
    }

    lv_draw_sw_rgb565_swap(color_map, pixel_count);
    port->last_submission_us = now_us;
    atomic_fetch_add_explicit(&port->submitted, 1, memory_order_relaxed);
    const esp_err_t error = esp_lcd_panel_draw_bitmap(
        port->panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
        color_map);
    if (error != ESP_OK) {
        atomic_fetch_add_explicit(&port->submit_errors, 1,
                                  memory_order_relaxed);
        atomic_store_explicit(&port->transfer_in_flight, false,
                              memory_order_release);
        ESP_LOGE(TAG, "Panel transfer failed: %s", esp_err_to_name(error));
        lv_display_flush_ready(display);
    }
}

lv_display_t *display_port_start(void)
{
    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t error = lvgl_port_init(&lvgl_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LVGL initialization failed: %s", esp_err_to_name(error));
        return NULL;
    }

    error = bsp_display_new(NULL, &s_port.panel, &s_port.io);
    if (error != ESP_OK || s_port.panel == NULL || s_port.io == NULL) {
        ESP_LOGE(TAG, "Panel initialization failed: %s",
                 esp_err_to_name(error));
        return NULL;
    }

    // Brightness is a CO5300 QSPI command, not a separate backlight GPIO.
    // Send it before LVGL exists so it cannot collide with a color transfer.
    error = bsp_display_brightness_set(DISPLAY_BRIGHTNESS_PERCENT);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Brightness initialization failed: %s",
                 esp_err_to_name(error));
        return NULL;
    }

    const size_t buffer_pixels = BSP_LCD_H_RES * DISPLAY_DRAW_ROWS;
    s_port.draw_buffer = heap_caps_aligned_alloc(
        64, buffer_pixels * sizeof(*s_port.draw_buffer),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_port.draw_buffer == NULL) {
        ESP_LOGE(TAG, "Display DMA-buffer allocation failed");
        return NULL;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return NULL;
    }
    s_port.display = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (s_port.display != NULL) {
        lv_display_set_color_format(s_port.display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s_port.display, s_port.draw_buffer, NULL,
                               buffer_pixels * sizeof(*s_port.draw_buffer),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_driver_data(s_port.display, &s_port);
        lv_display_set_flush_cb(s_port.display, flush_display);
        lv_display_add_event_cb(s_port.display, round_invalidated_area,
                                LV_EVENT_INVALIDATE_AREA, NULL);
        lv_display_add_event_cb(s_port.display, wake_display_task,
                                LV_EVENT_REFR_REQUEST, NULL);
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = transfer_done_callback,
    };
    if (s_port.display != NULL) {
        error = esp_lcd_panel_io_register_event_callbacks(
            s_port.io, &callbacks, &s_port);
    } else {
        error = ESP_ERR_NO_MEM;
    }
    lvgl_port_unlock();

    if (error != ESP_OK || s_port.display == NULL) {
        ESP_LOGE(TAG, "Display callback registration failed: %s",
                 esp_err_to_name(error));
        return NULL;
    }

    esp_lcd_touch_handle_t touch = NULL;
    error = bsp_touch_new(NULL, &touch);
    if (error != ESP_OK || touch == NULL) {
        ESP_LOGE(TAG, "Touch initialization failed: %s",
                 esp_err_to_name(error));
        return NULL;
    }
    const lvgl_port_touch_cfg_t touch_config = {
        .disp = s_port.display,
        .handle = touch,
    };
    if (lvgl_port_add_touch(&touch_config) == NULL) {
        ESP_LOGE(TAG, "LVGL touch registration failed");
        return NULL;
    }

    ESP_LOGI(TAG,
             "QSPI display ready: one %u-row internal DMA buffer, minimum interval=%uus",
             (unsigned)DISPLAY_DRAW_ROWS,
             (unsigned)MIN_TRANSFER_INTERVAL_US);
    return s_port.display;
}

void display_port_get_snapshot(display_port_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    snapshot->submitted = atomic_load_explicit(&s_port.submitted,
                                                memory_order_relaxed);
    snapshot->completed = atomic_load_explicit(&s_port.completed,
                                                memory_order_relaxed);
    snapshot->submit_errors = atomic_load_explicit(&s_port.submit_errors,
                                                    memory_order_relaxed);
    snapshot->overlap_errors = atomic_load_explicit(&s_port.overlap_errors,
                                                     memory_order_relaxed);
    snapshot->pacing_delays = atomic_load_explicit(&s_port.pacing_delays,
                                                   memory_order_relaxed);
    snapshot->pacing_wait_ms = atomic_load_explicit(&s_port.pacing_wait_ms,
                                                    memory_order_relaxed);
}
