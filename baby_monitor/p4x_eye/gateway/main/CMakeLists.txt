idf_component_register(
    SRCS
        "gateway_main.c"
        "media_protocol.c"
        "realtime_tx_guard.c"
        "camera_stream.c"
        "mic_stream.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_driver_i2s
        esp_driver_jpeg
        esp_driver_ppa
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        json
        nvs_flash
        esp_timer
        esp32_p4_eye
    PRIV_REQUIRES
        bootloader_support
        esp_app_format
        esp_hosted
        esp_websocket_client
        lwip
        mbedtls
        tcp_transport
        esp_video
        esp_cam_sensor
        esp_codec_dev
)

set(required_gateway_env
    GATEWAY_WIFI_SSID
    GATEWAY_WIFI_PASSWORD
    GATEWAY_DEVICE_ID
    GATEWAY_DEVICE_KEY
)

foreach(name IN LISTS required_gateway_env)
    if(NOT DEFINED ENV{${name}} OR "$ENV{${name}}" STREQUAL "")
        message(FATAL_ERROR "Required environment variable ${name} is not set")
    endif()
    string(REPLACE "\\" "\\\\" escaped "$ENV{${name}}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${name}="${escaped}")
endforeach()

set(GATEWAY_HTTP_ORIGIN "https://renesas.zyxserver.online")
set(GATEWAY_WS_ORIGIN "wss://renesas.zyxserver.online")
if(DEFINED ENV{GATEWAY_HTTP_ORIGIN} AND NOT "$ENV{GATEWAY_HTTP_ORIGIN}" STREQUAL "")
    set(GATEWAY_HTTP_ORIGIN "$ENV{GATEWAY_HTTP_ORIGIN}")
endif()
if(DEFINED ENV{GATEWAY_WS_ORIGIN} AND NOT "$ENV{GATEWAY_WS_ORIGIN}" STREQUAL "")
    set(GATEWAY_WS_ORIGIN "$ENV{GATEWAY_WS_ORIGIN}")
endif()
foreach(name IN ITEMS GATEWAY_HTTP_ORIGIN GATEWAY_WS_ORIGIN)
    string(REPLACE "\\" "\\\\" escaped "${${name}}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${name}="${escaped}")
endforeach()

if(CONFIG_IDF_TARGET_ESP32P4)
    target_add_binary_data(
        ${COMPONENT_LIB}
        "assets/esp_hosted_c6_2_12_12.bin"
        BINARY
    )
endif()

# The P4-EYE board is wired to its onboard ESP32-C6 through SDIO. A stale
# ignored sdkconfig can otherwise silently select the generic H2/SPI defaults,
# drive GPIO6 as a handshake interrupt, and watchdog-reset this board.
if(NOT CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6 OR
   NOT CONFIG_ESP32P4_EYE_C6_BOARD OR
   NOT CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
    message(FATAL_ERROR
        "P4-EYE gateway requires ESP32-C6 over SDIO. Build with the tracked "
        "sdkconfig.defaults and an isolated SDKCONFIG/build directory.")
endif()
