idf_component_register(
    SRCS "gateway_main.c" "media/media_protocol.c" "media/mic_stream.c" "media/camera_stream.c"
    INCLUDE_DIRS "." "media"
    REQUIRES driver esp_driver_i2s esp_event esp_netif esp_wifi json nvs_flash esp_timer
    PRIV_REQUIRES esp_websocket_client esp32-camera mbedtls
)
