#pragma once

// Copy this file to arduino_config.h and fill in local credentials.
static constexpr char WIFI_SSID[] = "your-wifi";
static constexpr char WIFI_PASSWORD[] = "your-password";
static constexpr char SERVER_BASE_URL[] = "https://renesas.zyxserver.online";
static constexpr char DEVICE_ID[] = "esp32cam-01";
static constexpr char DEVICE_KEY[] = "replace-with-server-device-key";
static constexpr char FIRMWARE_VERSION[] = "esp32cam-ra8-gateway-0.4.9-collection";
static constexpr char WSS_HOST[] = "renesas.zyxserver.online";
static constexpr uint16_t WSS_PORT = 443;
static constexpr char WSS_PATH[] = "/api/v1/device/stream";

// UART0: RA8 TX -> GPIO3 RX, RA8 RX <- GPIO1 TX, and common GND.
static constexpr int RA8_UART_RX_PIN = 3;
static constexpr int RA8_UART_TX_PIN = 1;
static constexpr uint32_t RA8_UART_BAUD = 921600;

// Status is prioritized. Large matrix uploads are latest-only and rate-limited.
static constexpr uint32_t TELEMETRY_INTERVAL_MS = 500;
static constexpr uint32_t PRESSURE_UPLOAD_INTERVAL_MS = 500;
static constexpr uint32_t THERMAL_UPLOAD_INTERVAL_MS = 1000;
static constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 10000;
static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static constexpr uint32_t RA8_OFFLINE_MS = 2000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 3000;
static constexpr uint32_t NETWORK_STALL_RESTART_MS = 30000;

// Keep bulk media local during sensor experiments. Enabling either option can
// consume enough TLS time and bandwidth to delay telemetry on classic ESP32.
static constexpr bool CLOUD_VIDEO_UPLOAD_ENABLED = false;
static constexpr bool CLOUD_AUDIO_UPLOAD_ENABLED = false;
