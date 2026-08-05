set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/credentials.h")

idf_component_register(
    SRCS "app_main.c" "display_port.c" "face.c" "face_sprite_assets.c" "motion_sensor.c" "network_relay.c" "offline_echo.c" "pcm_batcher.c" "pcm_ring.c" "remote_event_queue.c" "remote_response.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 esp_timer esp_codec_dev esp_driver_i2s esp_lcd_touch esp_lvgl_port nvs_flash esp_wifi esp_netif esp_event esp_app_format mbedtls espressif__esp_websocket_client qmi8658
)
