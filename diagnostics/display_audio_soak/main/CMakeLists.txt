idf_component_register(
    SRCS "display_audio_soak_main.c" "../../../firmware/main/face.c"
    INCLUDE_DIRS "." "../../../firmware/main"
    REQUIRES esp32_s3_touch_amoled_1_8 esp_timer esp_codec_dev esp_driver_i2s
)
