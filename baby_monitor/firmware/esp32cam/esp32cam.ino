#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "arduino_config.h"

namespace {

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr char BUILD_FIRMWARE_VERSION[] = "esp32cam-ra8-gateway-0.4.12-fat83";
constexpr uint8_t TYPE_STATUS_UPLOAD = 0x10;
constexpr uint8_t TYPE_THERMAL_CHUNK = 0x11;
constexpr uint8_t TYPE_PRESSURE_UPLOAD = 0x12;
constexpr uint8_t TYPE_SAFETY_EVENT = 0x13;
constexpr uint8_t TYPE_CMD = 0x20;
constexpr uint8_t TYPE_SAFETY_ACK = 0x22;
constexpr size_t MAX_RA8_PAYLOAD = 4096;
constexpr size_t EVENT_QUEUE_LENGTH = 8;
constexpr size_t UART_BYTES_PER_SLICE = 2048;
constexpr uint32_t WSS_RECONFIGURE_INTERVAL_MS = 5000;
constexpr uint32_t WSS_OFFLINE_RESTART_MS = 120000;
constexpr uint32_t WSS_ACTIVITY_TIMEOUT_MS = 12000;
constexpr uint32_t WSS_ACTIVITY_RESTART_MS = 30000;
constexpr uint32_t WIFI_OFFLINE_RESTART_MS = 120000;
constexpr uint32_t HTTPS_FALLBACK_TELEMETRY_MS = 2000;
constexpr uint32_t HTTPS_FALLBACK_PRESSURE_MS = 2000;
constexpr uint32_t HTTPS_FALLBACK_THERMAL_MS = 3000;
constexpr bool MEDIA_FEATURES_ENABLED = false;
constexpr uint16_t THERMAL_SOURCE_ROWS = 64;
constexpr uint16_t THERMAL_SOURCE_COLS = 80;
constexpr uint16_t THERMAL_WEB_DOWNSAMPLE = 2;
constexpr size_t NETWORK_BINARY_BUFFER_SIZE = 20U * 1024U;
constexpr size_t BATCH_HEADER_SIZE = 48U;
constexpr uint8_t BATCH_PRESSURE = 1U << 0;
constexpr uint8_t BATCH_THERMAL = 1U << 1;
constexpr uint8_t BATCH_VIDEO = 1U << 2;
constexpr uint8_t BATCH_AUDIO = 1U << 3;
constexpr uint32_t CLOUD_AUDIO_SAMPLE_RATE = 8000U;
// GPIO4 drives the AI Thinker ESP32-CAM flash LED. Disable the debug UART in
// normal operation so the LED stays off and does not waste power or heat up.
constexpr bool DEBUG_UART_ENABLED = false;
constexpr int DEBUG_UART_TX_PIN = 4;
constexpr uint32_t DEBUG_UART_BAUD = 115200;
#define DEBUG_PRINTF(...) do { if (DEBUG_UART_ENABLED) DebugSerial.printf(__VA_ARGS__); } while (0)

uint32_t elapsedMs(uint32_t now, uint32_t then) {
  const int32_t delta = static_cast<int32_t>(now - then);
  return delta >= 0 ? static_cast<uint32_t>(delta) : 0U;
}

HardwareSerial Ra8Serial(0);
HardwareSerial DebugSerial(2);
SemaphoreHandle_t statusMutex;
QueueHandle_t eventQueue;

struct GatewayStats {
  volatile uint32_t validFrames = 0;
  volatile uint32_t statusFrames = 0;
  volatile uint32_t crcErrors = 0;
  volatile uint32_t lengthErrors = 0;
  volatile uint32_t droppedEvents = 0;
  volatile uint32_t lastRa8StatusMs = 0;
  volatile uint32_t lastRa8Sequence = 0;
};

struct NetworkStats {
  uint32_t requestCount = 0;
  uint32_t failureCount = 0;
  uint32_t telemetrySuccess = 0;
  uint32_t matrixSuccess = 0;
  uint32_t lastHttpDurationMs = 0;
  uint32_t maxHttpDurationMs = 0;
  int lastHttpCode = 0;
  uint32_t lastTelemetrySuccessMs = 0;
  uint32_t lastThermalSuccessMs = 0;
  uint32_t lastPressureSuccessMs = 0;
  uint32_t websocketReconnects = 0;
  uint32_t websocketActivityTimeouts = 0;
  uint32_t batchConnectionOpens = 0;
  uint32_t batchConnectionReuses = 0;
  uint32_t batchConnectionResets = 0;
};

struct SafetyEvent {
  uint32_t eventId;
  uint16_t length;
  char json[768];
};

GatewayStats stats;
NetworkStats networkStats;
WiFiClientSecure networkClient;
WiFiClientSecure commandClient;
WiFiClientSecure batchClient;
HTTPClient batchHttp;
bool batchHttpReady = false;
WebSocketsClient deviceSocket;
volatile bool websocketConnected = false;
bool websocketConfigured = false;
String websocketHeaders;
uint32_t websocketOfflineSinceMs = 0;
volatile uint32_t websocketLastActivityMs = 0;
uint32_t websocketLastReconfigureMs = 0;
String latestStatus = "{}";
uint16_t txSequence = 1;
uint32_t telemetrySequence = 1;
uint32_t lastCommandId = 0;
bool cameraReady = false;
volatile uint32_t cameraInitAttempts = 0;
volatile int32_t cameraInitError = ESP_OK;
bool mediaServersStarted = false;
httpd_handle_t videoServer = nullptr;
httpd_handle_t audioServer = nullptr;
portMUX_TYPE audioClientsLock = portMUX_INITIALIZER_UNLOCKED;
int audioClients[4] = {-1, -1, -1, -1};
SemaphoreHandle_t matrixMutex;
SemaphoreHandle_t audioUploadMutex;
int16_t thermalMatrix[80 * 64]{};
uint8_t thermalSeen[(80 * 64 + 7) / 8]{};
uint32_t thermalSequence = 0;
uint16_t thermalReceived = 0;
bool thermalPending = false;
int16_t pressureMatrix[16 * 16]{};
uint8_t pressureSeen[(16 * 16 + 7) / 8]{};
uint32_t pressureSequence = 0;
uint16_t pressureReceived = 0;
bool pressurePending = false;
uint8_t *websocketBinaryBuffer = nullptr;
volatile uint32_t networkHeartbeatMs = 0;
volatile uint32_t wifiOfflineSinceMs = 0;
volatile bool telemetryUrgent = true;
uint8_t requestsSinceTelemetry = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t networkStallRestarts = 0;
RTC_DATA_ATTR uint32_t wifiOfflineRestarts = 0;
int16_t *audioBuildBuffer = nullptr;
int16_t *audioSendBuffer = nullptr;
size_t audioBuildSamples = 0;
size_t audioSendSamples = 0;
bool audioUploadPending = false;
uint32_t audioUploadSequence = 0;
uint32_t videoUploadSequence = 0;
volatile bool cloudVideoPreviewEnabled = false;
volatile bool cloudAudioPreviewEnabled = false;
volatile uint32_t cloudPreviewExpiresAtMs = 0;

void startMediaServers();
bool initCamera();
void executeCommand(JsonObjectConst command, uint32_t id);

void debugNetworkState(const char *tag) {
  DEBUG_PRINTF(
      "[%10lu] %s wifi=%d heap=%u min=%u req=%lu fail=%lu code=%d last=%lums max=%lums tel=%lu mat=%lu\r\n",
      millis(), tag, WiFi.status(), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
      networkStats.requestCount, networkStats.failureCount, networkStats.lastHttpCode,
      networkStats.lastHttpDurationMs, networkStats.maxHttpDurationMs,
      networkStats.telemetrySuccess, networkStats.matrixSuccess);
}

void writeLe16(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint16_t readBe16(const uint8_t *data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t readBe32(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void acceptMatrixChunk(uint8_t type, const uint8_t *payload, uint16_t length) {
  if (length < 12 || !matrixMutex) return;
  const uint32_t sequence = readBe32(payload);
  const uint16_t width = readBe16(payload + 4);
  const uint16_t height = readBe16(payload + 6);
  const uint16_t offset = readBe16(payload + 8);
  const uint16_t count = readBe16(payload + 10);
  const uint32_t total = static_cast<uint32_t>(width) * height;
  if (length != 12 + count * 2U || offset + count > total) return;

  int16_t *matrix = nullptr;
  uint8_t *seen = nullptr;
  uint32_t *currentSequence = nullptr;
  uint16_t *received = nullptr;
  bool *pending = nullptr;
  uint32_t expected = 0;
  if (type == TYPE_THERMAL_CHUNK && width == 80 && height == 64) {
    matrix = thermalMatrix; seen = thermalSeen; currentSequence = &thermalSequence;
    received = &thermalReceived; pending = &thermalPending; expected = 80U * 64U;
  } else if (type == TYPE_PRESSURE_UPLOAD && width == 16 && height == 16) {
    matrix = pressureMatrix; seen = pressureSeen; currentSequence = &pressureSequence;
    received = &pressureReceived; pending = &pressurePending; expected = 16U * 16U;
  } else {
    return;
  }
  if (xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(2)) != pdTRUE) return;
  if (*currentSequence != sequence) {
    *currentSequence = sequence;
    *received = 0;
    *pending = false;
    memset(seen, 0, (expected + 7) / 8);
  }
  for (uint16_t i = 0; i < count; ++i) {
    const uint16_t index = offset + i;
    const uint8_t mask = static_cast<uint8_t>(1U << (index & 7));
    if (!(seen[index >> 3] & mask)) {
      seen[index >> 3] |= mask;
      ++*received;
    }
    matrix[index] = static_cast<int16_t>(readBe16(payload + 12 + i * 2));
  }
  if (*received == expected) *pending = true;
  xSemaphoreGive(matrixMutex);
}

uint16_t crc16CcittFalse(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

void sendRa8Frame(uint8_t type, const uint8_t *payload, uint16_t length) {
  uint8_t header[8] = {0xAA, 0x55, PROTOCOL_VERSION, type,
                       static_cast<uint8_t>(txSequence >> 8), static_cast<uint8_t>(txSequence),
                       static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
  uint16_t crc = crc16CcittFalse(header, sizeof(header));
  for (uint16_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(payload[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  Ra8Serial.write(header, sizeof(header));
  if (length) Ra8Serial.write(payload, length);
  uint8_t trailer[2] = {static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
  Ra8Serial.write(trailer, sizeof(trailer));
  ++txSequence;
}

uint32_t eventIdFromJson(const uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return 0;
  if (!doc["event"].isNull()) return doc["event"].as<uint32_t>();
  return doc["event_id"] | 0U;
}

void handleRa8Frame(uint8_t type, uint16_t sequence, const uint8_t *payload, uint16_t length) {
  ++stats.validFrames;
  if (type == TYPE_STATUS_UPLOAD) {
    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      latestStatus = String(reinterpret_cast<const char *>(payload), length);
      xSemaphoreGive(statusMutex);
    }
    stats.lastRa8StatusMs = millis();
    stats.lastRa8Sequence = sequence;
    ++stats.statusFrames;
    return;
  }

  if (type == TYPE_THERMAL_CHUNK || type == TYPE_PRESSURE_UPLOAD) {
    acceptMatrixChunk(type, payload, length);
    return;
  }

  if (type == TYPE_SAFETY_EVENT) {
    const uint32_t eventId = eventIdFromJson(payload, length);
    if (!eventId) return;
    SafetyEvent event{};
    event.eventId = eventId;
    event.length = min(static_cast<size_t>(sizeof(event.json) - 1), static_cast<size_t>(length));
    memcpy(event.json, payload, event.length);
    event.json[event.length] = '\0';
    if (xQueueSend(eventQueue, &event, 0) == pdTRUE) {
      uint8_t ack[4] = {static_cast<uint8_t>(eventId >> 24), static_cast<uint8_t>(eventId >> 16),
                        static_cast<uint8_t>(eventId >> 8), static_cast<uint8_t>(eventId)};
      sendRa8Frame(TYPE_SAFETY_ACK, ack, sizeof(ack));
    } else {
      // Leave it unacknowledged so RA8 retries after queue space is available.
      ++stats.droppedEvents;
    }
  }
}

void uartTask(void *) {
  enum State : uint8_t { MAGIC_AA, MAGIC_55, HEADER, PAYLOAD, CRC_HIGH, CRC_LOW };
  State state = MAGIC_AA;
  uint8_t header[8]{};
  uint8_t payload[MAX_RA8_PAYLOAD]{};
  uint8_t headerPos = 0;
  uint16_t payloadLength = 0;
  uint16_t payloadPos = 0;
  uint16_t receivedCrc = 0;

  for (;;) {
    size_t processed = 0;
    while (Ra8Serial.available() && processed < UART_BYTES_PER_SLICE) {
      const uint8_t value = static_cast<uint8_t>(Ra8Serial.read());
      ++processed;
      switch (state) {
        case MAGIC_AA:
          if (value == 0xAA) state = MAGIC_55;
          break;
        case MAGIC_55:
          if (value == 0x55) {
            header[0] = 0xAA;
            header[1] = 0x55;
            headerPos = 2;
            state = HEADER;
          } else {
            state = (value == 0xAA) ? MAGIC_55 : MAGIC_AA;
          }
          break;
        case HEADER:
          header[headerPos++] = value;
          if (headerPos == sizeof(header)) {
            payloadLength = (static_cast<uint16_t>(header[6]) << 8) | header[7];
            payloadPos = 0;
            if (payloadLength > MAX_RA8_PAYLOAD) {
              ++stats.lengthErrors;
              state = MAGIC_AA;
            } else {
              state = payloadLength ? PAYLOAD : CRC_HIGH;
            }
          }
          break;
        case PAYLOAD:
          payload[payloadPos++] = value;
          if (payloadPos == payloadLength) state = CRC_HIGH;
          break;
        case CRC_HIGH:
          receivedCrc = static_cast<uint16_t>(value) << 8;
          state = CRC_LOW;
          break;
        case CRC_LOW: {
          receivedCrc |= value;
          uint16_t crc = crc16CcittFalse(header, sizeof(header));
          for (uint16_t i = 0; i < payloadLength; ++i) {
            crc ^= static_cast<uint16_t>(payload[i]) << 8;
            for (uint8_t bit = 0; bit < 8; ++bit) {
              crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                   : static_cast<uint16_t>(crc << 1);
            }
          }
          if (crc == receivedCrc) {
            const uint16_t sequence = (static_cast<uint16_t>(header[4]) << 8) | header[5];
            handleRa8Frame(header[3], sequence, payload, payloadLength);
          } else {
            ++stats.crcErrors;
          }
          state = MAGIC_AA;
          break;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool beginHttp(HTTPClient &http, WiFiClientSecure &client, const String &path,
               const char *contentType = "application/json") {
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String(SERVER_BASE_URL) + path)) return false;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(true);
  http.addHeader("Content-Type", contentType);
  http.addHeader("X-Device-ID", DEVICE_ID);
  http.addHeader("X-Device-Key", DEVICE_KEY);
  return true;
}

void recordHttpResult(int code, uint32_t startedAt) {
  const uint32_t duration = millis() - startedAt;
  ++networkStats.requestCount;
  networkStats.lastHttpCode = code;
  networkStats.lastHttpDurationMs = duration;
  networkStats.maxHttpDurationMs = max(networkStats.maxHttpDurationMs, duration);
  if (code < 200 || code >= 300) {
    ++networkStats.failureCount;
    DEBUG_PRINTF("[%10lu] http_fail code=%d duration=%lu heap=%u\r\n",
                 millis(), code, duration, ESP.getFreeHeap());
  }
}

int postJson(const String &path, const String &body) {
  HTTPClient http;
  const uint32_t startedAt = millis();
  if (!beginHttp(http, networkClient, path)) {
    recordHttpResult(-1, startedAt);
    return -1;
  }
  const int code = http.POST(body);
  if (code > 0) (void) http.getString();
  http.end();
  networkClient.stop();
  recordHttpResult(code, startedAt);
  return code;
}

int postBinary(const String &path, const char *contentType, const uint8_t *data, size_t length) {
  HTTPClient http;
  const uint32_t startedAt = millis();
  if (!beginHttp(http, networkClient, path, contentType)) {
    recordHttpResult(-1, startedAt);
    return -1;
  }
  const int code = http.POST(const_cast<uint8_t *>(data), length);
  if (code > 0) (void) http.getString();
  http.end();
  networkClient.stop();
  recordHttpResult(code, startedAt);
  return code;
}

void resetBatchHttp() {
  if (batchHttpReady) {
    batchHttp.end();
    batchHttpReady = false;
  }
  batchClient.stop();
  ++networkStats.batchConnectionResets;
}

bool ensureBatchHttp() {
  if (batchHttpReady) return true;
  batchClient.setInsecure();
  batchClient.setTimeout(HTTP_TIMEOUT_MS);
  if (!batchHttp.begin(batchClient, String(SERVER_BASE_URL) + "/api/v1/batch")) {
    batchClient.stop();
    return false;
  }
  batchHttp.setTimeout(HTTP_TIMEOUT_MS);
  batchHttp.setReuse(true);
  batchHttp.addHeader("Content-Type", "application/octet-stream");
  batchHttp.addHeader("X-Device-ID", DEVICE_ID);
  batchHttp.addHeader("X-Device-Key", DEVICE_KEY);
  batchHttpReady = true;
  ++networkStats.batchConnectionOpens;
  return true;
}

int postBatch(const uint8_t *data, size_t length) {
  const uint32_t startedAt = millis();
  const bool reusedSocket = batchHttpReady && batchClient.connected();
  if (!ensureBatchHttp()) {
    recordHttpResult(-1, startedAt);
    return -1;
  }
  if (reusedSocket) ++networkStats.batchConnectionReuses;

  const int code = batchHttp.POST(const_cast<uint8_t *>(data), length);
  const String response = code > 0 ? batchHttp.getString() : String();
  recordHttpResult(code, startedAt);
  if (code < 200 || code >= 300) resetBatchHttp();
  if (code >= 200 && code < 300 && !response.isEmpty()) {
    JsonDocument doc;
    if (!deserializeJson(doc, response) && !doc["command"].isNull()) {
      JsonObjectConst command = doc["command"].as<JsonObjectConst>();
      const uint32_t id = command["id"] | 0U;
      if (id) {
        // Command ACK uses the general HTTPS client. Release the persistent
        // batch TLS session first so classic ESP32 has enough TLS heap.
        resetBatchHttp();
        executeCommand(command, id);
      }
    }
  }
  return code;
}

bool sendWebsocketJson(const char *kind, const String &payload) {
  const uint32_t startedAt = millis();
  if (!websocketConnected) return false;
  String envelope;
  envelope.reserve(payload.length() + strlen(kind) + 24U);
  envelope = "{\"kind\":\"";
  envelope += kind;
  envelope += "\",\"payload\":";
  envelope += payload;
  envelope += '}';
  const bool sent = deviceSocket.sendTXT(envelope);
  const uint32_t duration = millis() - startedAt;
  ++networkStats.requestCount;
  networkStats.lastHttpCode = sent ? 101 : -1;
  networkStats.lastHttpDurationMs = duration;
  networkStats.maxHttpDurationMs = max(networkStats.maxHttpDurationMs, duration);
  if (!sent) {
    ++networkStats.failureCount;
    DEBUG_PRINTF("[%10lu] ws_text_fail kind=%s bytes=%u duration=%lu\r\n",
                 millis(), kind, envelope.length(), duration);
  }
  return sent;
}

bool sendHello() {
  JsonDocument doc;
  doc["protocol_version"] = 1;
  doc["firmware"] = BUILD_FIRMWARE_VERSION;
  doc["hardware"] = "ESP32-CAM";
  JsonArray capabilities = doc["capabilities"].to<JsonArray>();
  capabilities.add("thermal");
  capabilities.add("pressure");
  capabilities.add("ra8_audio_collection");
  capabilities.add("ra8_camera_collection");
  capabilities.add("ra8_gateway");
  doc["metadata"]["media_enabled"] = false;
  doc["metadata"]["local_ip"] = WiFi.localIP().toString();
  doc["metadata"]["gateway_ip"] = WiFi.gatewayIP().toString();
  doc["metadata"]["subnet_mask"] = WiFi.subnetMask().toString();
  doc["metadata"]["mac"] = WiFi.macAddress();
  doc["metadata"]["boot_count"] = bootCount;
  doc["metadata"]["reset_reason"] = static_cast<uint32_t>(esp_reset_reason());
  doc["metadata"]["network_stall_restarts"] = networkStallRestarts;
  doc["metadata"]["wifi_offline_restarts"] = wifiOfflineRestarts;
  String body;
  serializeJson(doc, body);
  if (websocketConnected) return sendWebsocketJson("hello", body);
  const int code = postJson("/api/v1/device/hello", body);
  return code >= 200 && code < 300;
}

void capturePressureMatrix(JsonObjectConst pressure) {
  JsonArrayConst values = pressure["matrix"].as<JsonArrayConst>();
  const uint32_t sequence = pressure["seq"] | 0U;
  if (!sequence || values.size() != 16U * 16U || !matrixMutex) return;
  if (xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  if (pressureSequence != sequence) {
    size_t index = 0;
    for (JsonVariantConst value : values) {
      pressureMatrix[index++] = value.as<int16_t>();
    }
    pressureSequence = sequence;
    pressureReceived = 16U * 16U;
    pressurePending = true;
  }
  xSemaphoreGive(matrixMutex);
}

void copyFields(JsonObject destination, JsonObjectConst source,
                const char *const *fields, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    JsonVariantConst value = source[fields[index]];
    if (!value.isNull()) destination[fields[index]] = value;
  }
}

void addSensorSummary(JsonObject sensors, const char *outputName, JsonObjectConst source,
                      const char *const *fields, size_t count) {
  if (source.isNull()) return;
  JsonObject destination = sensors[outputName].to<JsonObject>();
  copyFields(destination, source, fields, count);
}

String buildTelemetryPayload() {
  String statusJson;
  if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    statusJson = latestStatus;
    xSemaphoreGive(statusMutex);
  }
  JsonDocument ra8;
  const bool statusValid = !deserializeJson(ra8, statusJson);
  JsonDocument out;
  out["protocol_version"] = 1;
  out["sequence"] = telemetrySequence++;
  out["device_time_ms"] = millis();
  out["ra8_tick"] = statusValid ? (ra8["tick"] | 0U) : 0U;
  out["wifi_rssi"] = WiFi.RSSI();
  out["firmware"] = BUILD_FIRMWARE_VERSION;

  JsonObject sensors = out["sensors"].to<JsonObject>();
  if (statusValid) {
    capturePressureMatrix(ra8["prs"].as<JsonObjectConst>());
    static const char *thermalFields[] = {
      "seq", "s", "rx", "min_cd", "max_cd", "ctr_cd", "avg_cd", "err", "uart_err",
      "ovf", "valid", "crc_rx", "crc_calc", "fifo_ovf", "crc_bad", "data_bad", "build"
    };
    static const char *temperatureFields[] = {
      "seq", "s", "ok", "err", "fs", "ch", "fsp", "pca", "t_mc", "addr"
    };
    static const char *pressureFields[] = {
      "seq", "s", "ok", "err", "fs", "ch", "fsp", "up_ch"
    };
    static const char *microphoneFields[] = {
      "seq", "cmd_seq", "ack", "s", "rate", "bits", "cb", "q", "read", "err", "to",
      "fsp", "ev", "peak", "mean", "nz", "rec", "rec_bytes", "rec_sec", "wst", "werr",
      "wpoll", "wopen", "wwr", "wbytes", "wseq", "wchunk", "slot", "shift", "gain", "file"
    };
    static const char *audioFields[] = {
      "seq", "cmd_seq", "ack", "s", "track", "count", "vol", "elapsed", "duration", "rate", "file"
    };
    addSensorSummary(sensors, "thermal", ra8["thm"].as<JsonObjectConst>(),
                     thermalFields, sizeof(thermalFields) / sizeof(thermalFields[0]));
    addSensorSummary(sensors, "bed_temperature", ra8["tmp"].as<JsonObjectConst>(),
                     temperatureFields, sizeof(temperatureFields) / sizeof(temperatureFields[0]));
    addSensorSummary(sensors, "pressure", ra8["prs"].as<JsonObjectConst>(),
                     pressureFields, sizeof(pressureFields) / sizeof(pressureFields[0]));
    addSensorSummary(sensors, "microphone", ra8["mic"].as<JsonObjectConst>(),
                     microphoneFields, sizeof(microphoneFields) / sizeof(microphoneFields[0]));
    addSensorSummary(sensors, "audio_player", ra8["aud"].as<JsonObjectConst>(),
                     audioFields, sizeof(audioFields) / sizeof(audioFields[0]));
    if (ra8["cam"].is<JsonObjectConst>()) sensors["camera"] = ra8["cam"];
    if (!ra8["fusion"].isNull()) out["inference"] = ra8["fusion"];
    if (!ra8["vision"].isNull()) out["vision"] = ra8["vision"];
  }
  JsonObject health = out["health"].to<JsonObject>();
  health["ra8_online"] = stats.lastRa8StatusMs && (millis() - stats.lastRa8StatusMs <= RA8_OFFLINE_MS);
  health["camera_ready"] = cameraReady;
  health["uart_valid_frames"] = stats.validFrames;
  health["uart_status_frames"] = stats.statusFrames;
  health["uart_crc_errors"] = stats.crcErrors;
  health["uart_length_errors"] = stats.lengthErrors;
  health["event_queue_drops"] = stats.droppedEvents;
  if (statusValid && !ra8["safe"].isNull()) health["safety"] = ra8["safe"];
  if (statusValid && !ra8["link"].isNull()) health["ra8_link"] = ra8["link"];
  JsonObject esp32 = health["esp32"].to<JsonObject>();
  esp32["local_ip"] = WiFi.localIP().toString();
  esp32["gateway_ip"] = WiFi.gatewayIP().toString();
  esp32["mac"] = WiFi.macAddress();
  esp32["free_heap"] = ESP.getFreeHeap();
  esp32["min_free_heap"] = ESP.getMinFreeHeap();
  esp32["free_psram"] = ESP.getFreePsram();
  esp32["uptime_ms"] = millis();
  esp32["video_sequence"] = videoUploadSequence;
  esp32["audio_sequence"] = audioUploadSequence;
  esp32["camera_ready"] = cameraReady;
  esp32["camera_init_attempts"] = cameraInitAttempts;
  esp32["camera_init_error"] = cameraInitError;
  esp32["boot_count"] = bootCount;
  esp32["reset_reason"] = static_cast<uint32_t>(esp_reset_reason());
  esp32["network_stall_restarts"] = networkStallRestarts;
  esp32["wifi_offline_restarts"] = wifiOfflineRestarts;
  esp32["wifi_status"] = static_cast<int32_t>(WiFi.status());
  esp32["wifi_offline_ms"] = wifiOfflineSinceMs ?
      elapsedMs(millis(), wifiOfflineSinceMs) : 0U;

  JsonObject gateway = health["gateway"].to<JsonObject>();
  gateway["transport"] = "https_keepalive";
  gateway["ws_connected"] = websocketConnected;
  gateway["ws_reconnects"] = networkStats.websocketReconnects;
  gateway["ws_activity_timeouts"] = networkStats.websocketActivityTimeouts;
  gateway["ws_offline_ms"] = websocketConnected || !websocketOfflineSinceMs ?
      0U : millis() - websocketOfflineSinceMs;
  gateway["ws_activity_age_ms"] = websocketLastActivityMs ?
      millis() - websocketLastActivityMs : UINT32_MAX;
  gateway["http_requests"] = networkStats.requestCount;
  gateway["http_failures"] = networkStats.failureCount;
  gateway["last_http_code"] = networkStats.lastHttpCode;
  gateway["last_http_ms"] = networkStats.lastHttpDurationMs;
  gateway["max_http_ms"] = networkStats.maxHttpDurationMs;
  gateway["telemetry_ok"] = networkStats.telemetrySuccess;
  gateway["matrix_ok"] = networkStats.matrixSuccess;
  gateway["ra8_status_age_ms"] = stats.lastRa8StatusMs ? millis() - stats.lastRa8StatusMs : UINT32_MAX;
  gateway["telemetry_age_ms"] = networkStats.lastTelemetrySuccessMs ?
      millis() - networkStats.lastTelemetrySuccessMs : UINT32_MAX;
  gateway["thermal_age_ms"] = networkStats.lastThermalSuccessMs ?
      millis() - networkStats.lastThermalSuccessMs : UINT32_MAX;
  gateway["pressure_age_ms"] = networkStats.lastPressureSuccessMs ?
      millis() - networkStats.lastPressureSuccessMs : UINT32_MAX;
  gateway["thermal_pending"] = thermalPending;
  gateway["pressure_pending"] = pressurePending;
  gateway["pressure_received"] = pressureReceived;
  gateway["cloud_video"] = cloudVideoPreviewEnabled;
  gateway["cloud_audio"] = cloudAudioPreviewEnabled;
  gateway["cloud_preview_remaining_ms"] = cloudPreviewExpiresAtMs &&
      static_cast<int32_t>(cloudPreviewExpiresAtMs - millis()) > 0 ?
      cloudPreviewExpiresAtMs - millis() : 0U;
  gateway["matrix_transport"] = "https_keepalive";
  gateway["batch_http_ready"] = batchHttpReady;
  gateway["batch_socket_connected"] = batchClient.connected();
  gateway["batch_connection_opens"] = networkStats.batchConnectionOpens;
  gateway["batch_connection_reuses"] = networkStats.batchConnectionReuses;
  gateway["batch_connection_resets"] = networkStats.batchConnectionResets;
  gateway["thermal_web_rows"] = THERMAL_SOURCE_ROWS / THERMAL_WEB_DOWNSAMPLE;
  gateway["thermal_web_cols"] = THERMAL_SOURCE_COLS / THERMAL_WEB_DOWNSAMPLE;
  gateway["thermal_web_downsample"] = THERMAL_WEB_DOWNSAMPLE;

  String body;
  serializeJson(out, body);
  return body;
}

void uploadTelemetry() {
  const String body = buildTelemetryPayload();
  const bool sent = websocketConnected ? sendWebsocketJson("telemetry", body) :
      (postJson("/api/v1/telemetry", body) >= 200 && networkStats.lastHttpCode < 300);
  if (sent) {
    ++networkStats.telemetrySuccess;
    networkStats.lastTelemetrySuccessMs = millis();
  }
  requestsSinceTelemetry = 0;
}

bool uploadQueuedEvent() {
  SafetyEvent event{};
  if (xQueuePeek(eventQueue, &event, 0) != pdTRUE) return false;
  JsonDocument source;
  deserializeJson(source, event.json, event.length);
  JsonDocument out;
  out["protocol_version"] = 1;
  out["event_id"] = event.eventId;
  const char *eventType = source["type"] | static_cast<const char *>(nullptr);
  if (!eventType) eventType = source["event_type"] | "ra8_safety";
  out["event_type"] = eventType;
  out["state"] = source["state"] | 0;
  out["severity"] = source["severity"] | 0;
  out["confidence"] = source["confidence"] | 0.0;
  out["occurred_at_ms"] = source["tick"] | millis();
  out["payload"] = source.as<JsonVariantConst>();
  String body;
  serializeJson(out, body);
  const bool sent = websocketConnected ? sendWebsocketJson("event", body) :
      (resetBatchHttp(),
       postJson("/api/v1/events", body) >= 200 && networkStats.lastHttpCode < 300);
  if (sent) xQueueReceive(eventQueue, &event, 0);
  return true;
}

bool recordingFilenameValid(const char *filename) {
  if (!filename || !filename[0]) return true;
  const size_t length = strlen(filename);
  if (length > 8) return false;
  for (size_t index = 0; index < length; ++index) {
    const char value = filename[index];
    if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '_' || value == '-')) return false;
  }
  return true;
}

String recordingCommandForRa8(const char *prefix, JsonObjectConst args) {
  const char *filename = args["filename"] | "";
  if (!recordingFilenameValid(filename)) return String();
  String result(prefix);
  if (filename[0]) {
    result += ':';
    result += filename;
  }
  return result;
}

String commandForRa8(JsonObjectConst command) {
  const char *name = command["command"] | "";
  JsonObjectConst args = command["args"].as<JsonObjectConst>();
  if (!strcmp(name, "status.refresh")) return "status";
  if (!strcmp(name, "safety.test")) return "safety_test";
  if (!strcmp(name, "collection.start") || !strcmp(name, "collection.stop")) {
    const bool start = !strcmp(name, "collection.start");
    JsonArrayConst kinds = args["kinds"].as<JsonArrayConst>();
    for (JsonVariantConst kind : kinds) {
      const char *value = kind.as<const char *>();
      if (value && !strcmp(value, "audio")) {
        return start ? recordingCommandForRa8("mic_record", args) : String("mic_record_stop");
      }
      if (value && !strcmp(value, "camera")) {
        return start ? recordingCommandForRa8("camera_record", args) : String("camera_record_stop");
      }
    }
  }
  if (!strcmp(name, "collection.capture")) {
    JsonArrayConst kinds = args["kinds"].as<JsonArrayConst>();
    for (JsonVariantConst kind : kinds) {
      const char *value = kind.as<const char *>();
      if (value && !strcmp(value, "camera")) return recordingCommandForRa8("camera_capture", args);
    }
  }
  if (!strcmp(name, "ra8.command")) return args["payload"] | "";
  return "";
}

bool handleGatewayCommand(JsonObjectConst command, String &message) {
  const char *name = command["command"] | "";
  if (!strcmp(name, "media.preview.start")) {
    cloudVideoPreviewEnabled = false;
    cloudAudioPreviewEnabled = false;
    cloudPreviewExpiresAtMs = 0;
    telemetryUrgent = true;
    message = "ESP32 media disabled; RA8 gateway remains active";
    return true;
  }
  if (!strcmp(name, "media.preview.stop")) {
    cloudVideoPreviewEnabled = false;
    cloudAudioPreviewEnabled = false;
    cloudPreviewExpiresAtMs = 0;
    telemetryUrgent = true;
    message = "remote media preview disabled";
    return true;
  }
  return false;
}

void acknowledgeCommand(uint32_t id, bool ok, const char *message) {
  JsonDocument bodyDoc;
  bodyDoc["id"] = id;
  bodyDoc["ok"] = ok;
  bodyDoc["result"]["message"] = message;
  String body;
  serializeJson(bodyDoc, body);
  if (websocketConnected) {
    sendWebsocketJson("command_ack", body);
  } else {
    postJson("/api/v1/commands/" + String(id) + "/ack", body);
  }
}

void executeCommand(JsonObjectConst command, uint32_t id) {
  if (id == lastCommandId) {
    acknowledgeCommand(id, true, "duplicate already handled");
    return;
  }
  String message;
  if (handleGatewayCommand(command, message)) {
    lastCommandId = id;
    acknowledgeCommand(id, true, message.c_str());
    return;
  }
  const String payload = commandForRa8(command);
  if (payload.isEmpty()) {
    acknowledgeCommand(id, false, "unsupported command");
    return;
  }
  sendRa8Frame(TYPE_CMD, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
  lastCommandId = id;
  acknowledgeCommand(id, true, "forwarded to RA8");
}

void pollCommand() {
  HTTPClient http;
  const uint32_t startedAt = millis();
  resetBatchHttp();
  if (!beginHttp(http, networkClient, "/api/v1/commands/poll")) {
    recordHttpResult(-1, startedAt);
    return;
  }
  const int code = http.GET();
  const String response = code > 0 ? http.getString() : String();
  http.end();
  networkClient.stop();
  recordHttpResult(code, startedAt);
  if (code != 200) return;
  JsonDocument doc;
  if (deserializeJson(doc, response) || doc["command"].isNull()) return;
  JsonObjectConst command = doc["command"].as<JsonObjectConst>();
  const uint32_t id = command["id"] | 0U;
  if (!id) return;
  executeCommand(command, id);
}

void websocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  const uint32_t now = millis();
  if (type == WStype_CONNECTED) {
    websocketConnected = true;
    websocketOfflineSinceMs = 0;
    websocketLastActivityMs = now;
    networkClient.stop();
    telemetryUrgent = true;
    requestsSinceTelemetry = 0;
    (void) sendHello();
    debugNetworkState("ws_connected");
    return;
  }
  if (type == WStype_DISCONNECTED || type == WStype_ERROR) {
    websocketConnected = false;
    if (!websocketOfflineSinceMs) websocketOfflineSinceMs = now;
    DEBUG_PRINTF("[%10lu] ws_event type=%d\r\n", now, static_cast<int>(type));
    return;
  }
  if (type == WStype_PING || type == WStype_PONG || type == WStype_BIN) {
    websocketLastActivityMs = now;
    return;
  }
  if (type != WStype_TEXT || !payload || !length) return;
  websocketLastActivityMs = now;

  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char *kind = doc["kind"] | "";
  if (strcmp(kind, "command")) return;
  JsonObjectConst command = doc["payload"].as<JsonObjectConst>();
  const uint32_t id = command["id"] | 0U;
  if (!id) return;
  executeCommand(command, id);
}

void configureWebsocket() {
  if (websocketConfigured) return;
  // WebSocketsClient appends the final CRLF itself. A trailing CRLF here
  // terminates the HTTP header early and turns its User-Agent into an invalid
  // WebSocket frame immediately after the server accepts the upgrade.
  websocketHeaders = "X-Device-ID: " + String(DEVICE_ID) + "\r\nX-Device-Key: " + String(DEVICE_KEY);
  deviceSocket.setExtraHeaders(websocketHeaders.c_str());
  deviceSocket.onEvent(websocketEvent);
  deviceSocket.setReconnectInterval(2000);
  deviceSocket.enableHeartbeat(5000, 3000, 2);
  deviceSocket.beginSSL(WSS_HOST, WSS_PORT, WSS_PATH, "", "");
  websocketConfigured = true;
}

void resetWebsocketTransport(uint32_t now) {
  debugNetworkState("ws_reset");
  deviceSocket.disconnect();
  websocketConnected = false;
  websocketConfigured = false;
  websocketLastReconfigureMs = now;
  if (!websocketOfflineSinceMs) websocketOfflineSinceMs = now;
  ++networkStats.websocketReconnects;
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  /* Reset the station driver between failed association attempts.  In
   * particular, do not call begin() immediately after disconnect(true): the
   * latter powers the Wi-Fi interface down asynchronously on ESP32. */
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_STA);
  delay(150);
  WiFi.setSleep(false);
  networkClient.stop();
  commandClient.stop();
  resetBatchHttp();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(deadline - millis()) > 0) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);
    telemetryUrgent = true;
    if (MEDIA_FEATURES_ENABLED) startMediaServers();
    debugNetworkState("wifi_connected");
  } else {
    debugNetworkState("wifi_connect_failed");
    WiFi.disconnect(true, false);
    delay(150);
  }
}

bool uploadPendingMatrix(bool thermal) {
  if (!matrixMutex || !websocketBinaryBuffer) return false;
  const uint16_t rows = thermal ? THERMAL_SOURCE_ROWS / THERMAL_WEB_DOWNSAMPLE : 16U;
  const uint16_t cols = thermal ? THERMAL_SOURCE_COLS / THERMAL_WEB_DOWNSAMPLE : 16U;
  const size_t count = static_cast<size_t>(rows) * cols;
  uint32_t sequence = 0;
  int16_t *samples = reinterpret_cast<int16_t *>(websocketBinaryBuffer + 14U);
  if (xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  const bool pending = thermal ? thermalPending : pressurePending;
  if (pending && thermal) {
    sequence = thermalSequence;
    for (uint16_t row = 0; row < rows; ++row) {
      for (uint16_t col = 0; col < cols; ++col) {
        int32_t sum = 0;
        for (uint16_t dy = 0; dy < THERMAL_WEB_DOWNSAMPLE; ++dy) {
          for (uint16_t dx = 0; dx < THERMAL_WEB_DOWNSAMPLE; ++dx) {
            const size_t sourceRow = row * THERMAL_WEB_DOWNSAMPLE + dy;
            const size_t sourceCol = col * THERMAL_WEB_DOWNSAMPLE + dx;
            sum += thermalMatrix[sourceRow * THERMAL_SOURCE_COLS + sourceCol];
          }
        }
        samples[static_cast<size_t>(row) * cols + col] = static_cast<int16_t>(
            sum / (THERMAL_WEB_DOWNSAMPLE * THERMAL_WEB_DOWNSAMPLE));
      }
    }
  } else if (pending) {
    sequence = pressureSequence;
    memcpy(samples, pressureMatrix, count * sizeof(int16_t));
  }
  xSemaphoreGive(matrixMutex);
  if (!pending) return false;

  bool sent = false;
  if (websocketConnected) {
    uint8_t *frame = websocketBinaryBuffer;
    frame[0] = 'R'; frame[1] = 'A'; frame[2] = '8'; frame[3] = 'M';
    frame[4] = 1U;
    frame[5] = thermal ? 1U : 2U;
    frame[6] = static_cast<uint8_t>(rows >> 8); frame[7] = static_cast<uint8_t>(rows);
    frame[8] = static_cast<uint8_t>(cols >> 8); frame[9] = static_cast<uint8_t>(cols);
    frame[10] = static_cast<uint8_t>(sequence >> 24);
    frame[11] = static_cast<uint8_t>(sequence >> 16);
    frame[12] = static_cast<uint8_t>(sequence >> 8);
    frame[13] = static_cast<uint8_t>(sequence);
    const uint32_t startedAt = millis();
    sent = deviceSocket.sendBIN(frame, 14U + count * sizeof(int16_t));
    ++networkStats.requestCount;
    networkStats.lastHttpCode = sent ? 101 : -1;
    networkStats.lastHttpDurationMs = millis() - startedAt;
    networkStats.maxHttpDurationMs = max(networkStats.maxHttpDurationMs,
                                         networkStats.lastHttpDurationMs);
    if (!sent) ++networkStats.failureCount;
    if (!sent) {
      DEBUG_PRINTF("[%10lu] ws_matrix_fail kind=%s bytes=%u duration=%lu\r\n",
                   millis(), thermal ? "thermal" : "pressure",
                   14U + count * sizeof(int16_t), networkStats.lastHttpDurationMs);
    }
  } else {
    const String kind = thermal ? "thermal" : "pressure";
    const String unit = thermal ? "deci_c" : "raw";
    const String path = "/api/v1/matrices/" + kind + "?sequence=" + String(sequence) +
        "&rows=" + String(rows) + "&cols=" + String(cols) + "&unit=" + unit;
    const int code = postBinary(path, "application/octet-stream",
                                reinterpret_cast<const uint8_t *>(samples),
                                count * sizeof(int16_t));
    sent = code >= 200 && code < 300;
  }
  if (!sent) return true;

  ++networkStats.matrixSuccess;
  if (thermal) networkStats.lastThermalSuccessMs = millis();
  else networkStats.lastPressureSuccessMs = millis();
  if (xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (thermal && thermalSequence == sequence) thermalPending = false;
    if (!thermal && pressureSequence == sequence) pressurePending = false;
    xSemaphoreGive(matrixMutex);
  }
  return true;
}

bool uploadLatestBatch(bool includeThermal, bool includeVideo, bool includeAudio) {
  if (!matrixMutex || !websocketBinaryBuffer) return false;

  const String telemetry = buildTelemetryPayload();
  if (telemetry.length() > UINT16_MAX ||
      BATCH_HEADER_SIZE + telemetry.length() > NETWORK_BINARY_BUFFER_SIZE) {
    uploadTelemetry();
    return false;
  }

  constexpr uint16_t pressureRows = 16U;
  constexpr uint16_t pressureCols = 16U;
  constexpr uint16_t thermalRows = THERMAL_SOURCE_ROWS / THERMAL_WEB_DOWNSAMPLE;
  constexpr uint16_t thermalCols = THERMAL_SOURCE_COLS / THERMAL_WEB_DOWNSAMPLE;
  constexpr size_t pressureBytes = pressureRows * pressureCols * sizeof(int16_t);
  constexpr size_t thermalBytes = thermalRows * thermalCols * sizeof(int16_t);

  uint8_t flags = 0;
  uint32_t pressureSnapshotSequence = 0;
  uint32_t thermalSnapshotSequence = 0;
  uint32_t videoSnapshotSequence = 0;
  uint32_t audioSnapshotSequence = 0;
  uint32_t videoBytes = 0;
  uint32_t audioBytes = 0;
  size_t offset = BATCH_HEADER_SIZE + telemetry.length();
  if (xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;

  if (pressurePending && offset + pressureBytes <= NETWORK_BINARY_BUFFER_SIZE) {
    flags |= BATCH_PRESSURE;
    pressureSnapshotSequence = pressureSequence;
    memcpy(websocketBinaryBuffer + offset, pressureMatrix, pressureBytes);
    offset += pressureBytes;
  }
  if (includeThermal && thermalPending && offset + thermalBytes <= NETWORK_BINARY_BUFFER_SIZE) {
    flags |= BATCH_THERMAL;
    thermalSnapshotSequence = thermalSequence;
    for (uint16_t row = 0; row < thermalRows; ++row) {
      for (uint16_t col = 0; col < thermalCols; ++col) {
        int32_t sum = 0;
        for (uint16_t dy = 0; dy < THERMAL_WEB_DOWNSAMPLE; ++dy) {
          for (uint16_t dx = 0; dx < THERMAL_WEB_DOWNSAMPLE; ++dx) {
            const size_t sourceRow = row * THERMAL_WEB_DOWNSAMPLE + dy;
            const size_t sourceCol = col * THERMAL_WEB_DOWNSAMPLE + dx;
            sum += thermalMatrix[sourceRow * THERMAL_SOURCE_COLS + sourceCol];
          }
        }
        const int16_t sample = static_cast<int16_t>(
            sum / (THERMAL_WEB_DOWNSAMPLE * THERMAL_WEB_DOWNSAMPLE));
        writeLe16(websocketBinaryBuffer + offset, static_cast<uint16_t>(sample));
        offset += sizeof(sample);
      }
    }
  }
  xSemaphoreGive(matrixMutex);

  if (includeVideo && cameraReady) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame) {
      if (frame->len && offset + frame->len <= NETWORK_BINARY_BUFFER_SIZE) {
        flags |= BATCH_VIDEO;
        videoSnapshotSequence = ++videoUploadSequence;
        videoBytes = static_cast<uint32_t>(frame->len);
        memcpy(websocketBinaryBuffer + offset, frame->buf, frame->len);
        offset += frame->len;
      }
      esp_camera_fb_return(frame);
    }
  }

  if (includeAudio && audioUploadMutex && audioUploadPending &&
      xSemaphoreTake(audioUploadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    constexpr size_t cloudAudioSamples = 8000U;
    constexpr size_t cloudAudioBytes = cloudAudioSamples * sizeof(int16_t);
    if (audioUploadPending && audioSendSamples >= 16000U &&
        offset + cloudAudioBytes <= NETWORK_BINARY_BUFFER_SIZE) {
      flags |= BATCH_AUDIO;
      audioSnapshotSequence = audioUploadSequence;
      audioBytes = cloudAudioBytes;
      int16_t *destination = reinterpret_cast<int16_t *>(websocketBinaryBuffer + offset);
      for (size_t i = 0; i < cloudAudioSamples; ++i) destination[i] = audioSendBuffer[i * 2U];
      offset += cloudAudioBytes;
    }
    xSemaphoreGive(audioUploadMutex);
  }

  uint8_t *header = websocketBinaryBuffer;
  header[0] = 'R'; header[1] = 'A'; header[2] = '8'; header[3] = 'B';
  header[4] = 2U;
  header[5] = flags;
  writeLe16(header + 6, static_cast<uint16_t>(telemetry.length()));
  writeLe32(header + 8, pressureSnapshotSequence);
  writeLe32(header + 12, thermalSnapshotSequence);
  writeLe16(header + 16, flags & BATCH_PRESSURE ? pressureRows : 0U);
  writeLe16(header + 18, flags & BATCH_PRESSURE ? pressureCols : 0U);
  writeLe16(header + 20, flags & BATCH_THERMAL ? thermalRows : 0U);
  writeLe16(header + 22, flags & BATCH_THERMAL ? thermalCols : 0U);
  writeLe32(header + 24, videoSnapshotSequence);
  writeLe32(header + 28, videoBytes);
  writeLe32(header + 32, audioSnapshotSequence);
  writeLe32(header + 36, audioBytes);
  writeLe32(header + 40, CLOUD_AUDIO_SAMPLE_RATE);
  header[44] = 1U;
  header[45] = 16U;
  writeLe16(header + 46, 0U);
  memcpy(header + BATCH_HEADER_SIZE, telemetry.c_str(), telemetry.length());

  const int code = postBatch(header, offset);
  if (code < 200 || code >= 300) return false;

  const uint32_t completedAt = millis();
  ++networkStats.telemetrySuccess;
  networkStats.lastTelemetrySuccessMs = completedAt;
  if (flags & BATCH_PRESSURE) {
    ++networkStats.matrixSuccess;
    networkStats.lastPressureSuccessMs = completedAt;
  }
  if (flags & BATCH_THERMAL) {
    ++networkStats.matrixSuccess;
    networkStats.lastThermalSuccessMs = completedAt;
  }
  if (flags && xSemaphoreTake(matrixMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if ((flags & BATCH_PRESSURE) && pressureSequence == pressureSnapshotSequence) {
      pressurePending = false;
    }
    if ((flags & BATCH_THERMAL) && thermalSequence == thermalSnapshotSequence) {
      thermalPending = false;
    }
    xSemaphoreGive(matrixMutex);
  }
  if ((flags & BATCH_AUDIO) && audioUploadMutex &&
      xSemaphoreTake(audioUploadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (audioUploadSequence == audioSnapshotSequence) audioUploadPending = false;
    xSemaphoreGive(audioUploadMutex);
  }
  requestsSinceTelemetry = 0;
  return true;
}

void uploadCloudVideo() {
  if (!cameraReady) return;
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) return;
  const uint32_t sequence = ++videoUploadSequence;
  resetBatchHttp();
  postBinary("/api/v1/media/video?sequence=" + String(sequence), "image/jpeg", frame->buf, frame->len);
  esp_camera_fb_return(frame);
}

void uploadCloudAudio() {
  if (!audioUploadMutex || !audioUploadPending) return;
  const int16_t *buffer = nullptr;
  size_t samples = 0;
  uint32_t sequence = 0;
  if (xSemaphoreTake(audioUploadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (audioUploadPending) {
      buffer = audioSendBuffer;
      samples = audioSendSamples;
      sequence = audioUploadSequence;
    }
    xSemaphoreGive(audioUploadMutex);
  }
  if (!buffer || !samples) return;
  const String path = "/api/v1/media/audio?sequence=" + String(sequence) +
                      "&sample_rate=16000&channels=1&bits=16";
  resetBatchHttp();
  postBinary(path, "application/octet-stream", reinterpret_cast<const uint8_t *>(buffer),
             samples * sizeof(int16_t));
  if (xSemaphoreTake(audioUploadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (audioUploadSequence == sequence) audioUploadPending = false;
    xSemaphoreGive(audioUploadMutex);
  }
}

void networkTask(void *) {
  uint32_t lastWifiAttempt = 0;
  uint32_t lastBatch = 0;
  uint32_t lastEvent = 0;
  uint32_t lastThermal = 0;
  uint32_t lastVideo = 0;
  uint32_t lastCommand = 0;
  uint32_t lastCameraAttempt = 0;
  uint32_t lastDebug = 0;
  bool wifiWasConnected = false;
  bool helloSent = false;
  websocketConnected = false;
  networkHeartbeatMs = millis();
  wifiOfflineSinceMs = networkHeartbeatMs;
  for (;;) {
    networkHeartbeatMs = millis();
    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (!wifiConnected) {
      if (!wifiOfflineSinceMs) wifiOfflineSinceMs = now;
      if (wifiWasConnected) {
        networkClient.stop();
        commandClient.stop();
        resetBatchHttp();
      }
      wifiWasConnected = false;
      helloSent = false;
      if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS) {
        lastWifiAttempt = now;
        connectWifi();
      }
    } else {
      wifiOfflineSinceMs = 0;
      if (cloudPreviewExpiresAtMs &&
          static_cast<int32_t>(now - cloudPreviewExpiresAtMs) >= 0) {
        cloudVideoPreviewEnabled = false;
        cloudAudioPreviewEnabled = false;
        cloudPreviewExpiresAtMs = 0;
        telemetryUrgent = true;
      }
      if (!wifiWasConnected) {
        wifiWasConnected = true;
        networkClient.stop();
        resetBatchHttp();
        telemetryUrgent = true;
      }
      if (MEDIA_FEATURES_ENABLED && !cameraReady && now - lastCameraAttempt >= 5000) {
        lastCameraAttempt = now;
        cameraReady = initCamera();
        if (cameraReady) startMediaServers();
        telemetryUrgent = true;
      }
      if (!helloSent) {
        helloSent = sendHello();
      } else if (now - lastCommand >= COMMAND_POLL_INTERVAL_MS &&
                 (!networkStats.lastTelemetrySuccessMs ||
                  now - networkStats.lastTelemetrySuccessMs > COMMAND_POLL_INTERVAL_MS)) {
        pollCommand();
        lastCommand = millis();
      } else if (now - lastEvent >= 500 && uploadQueuedEvent()) {
        lastEvent = millis();
      } else if (telemetryUrgent || now - lastBatch >= TELEMETRY_INTERVAL_MS) {
        const bool includeThermal = now - lastThermal >= THERMAL_UPLOAD_INTERVAL_MS;
        const bool includeVideo = false;
        const bool includeAudio = false;
        telemetryUrgent = false;
        (void) uploadLatestBatch(includeThermal, includeVideo, includeAudio);
        lastBatch = millis();
        if (includeThermal) lastThermal = lastBatch;
        if (includeVideo) lastVideo = lastBatch;
      }
    }
    if (now - lastDebug >= 2000) {
      lastDebug = now;
      debugNetworkState("net");
    }
    networkHeartbeatMs = millis();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void networkWatchdogTask(void *) {
  for (;;) {
    const uint32_t now = millis();
    const bool networkTaskStalled = networkHeartbeatMs &&
        elapsedMs(now, networkHeartbeatMs) > NETWORK_STALL_RESTART_MS;
    const bool wifiOfflineTooLong = wifiOfflineSinceMs &&
        elapsedMs(now, wifiOfflineSinceMs) > WIFI_OFFLINE_RESTART_MS;
    if (networkTaskStalled || wifiOfflineTooLong) {
      DEBUG_PRINTF("[%10lu] watchdog net_stall=%u wifi_offline=%u heartbeat_age=%lu wifi_age=%lu\r\n",
                   now,
                   networkTaskStalled ? 1U : 0U,
                   wifiOfflineTooLong ? 1U : 0U,
                   networkHeartbeatMs ? elapsedMs(now, networkHeartbeatMs) : UINT32_MAX,
                   wifiOfflineSinceMs ? elapsedMs(now, wifiOfflineSinceMs) : 0U);
      if (networkTaskStalled) ++networkStallRestarts;
      if (wifiOfflineTooLong) ++wifiOfflineRestarts;
      WiFi.disconnect(true, false);
      delay(20);
      ESP.restart();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

bool initCamera() {
  ++cameraInitAttempts;
  if (cameraInitAttempts > 1) {
    (void) esp_camera_deinit();
    delay(100);
  }
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  cameraInitError = esp_camera_init(&config);
  return cameraInitError == ESP_OK;
}

const char MEDIA_INDEX[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM live</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:16px}"
    "img{width:min(100%,640px);display:block}button{margin-top:12px;padding:10px}</style>"
    "<h2>ESP32-CAM live</h2><img src='/stream'><button onclick='audio()'>Start audio</button>"
    "<script>let c;function audio(){c=new AudioContext({sampleRate:16000});let t=c.currentTime;"
    "let w=new WebSocket('ws://'+location.hostname+':82/audio');w.binaryType='arraybuffer';"
    "w.onmessage=e=>{let s=new Int16Array(e.data),f=new Float32Array(s.length);"
    "for(let i=0;i<s.length;i++)f[i]=s[i]/32768;let b=c.createBuffer(1,f.length,16000);"
    "b.copyToChannel(f,0);let n=c.createBufferSource();n.buffer=b;n.connect(c.destination);"
    "t=Math.max(t,c.currentTime+.04);n.start(t);t+=b.duration}}</script>";

esp_err_t rootHandler(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_send(request, MEDIA_INDEX, HTTPD_RESP_USE_STRLEN);
}

esp_err_t videoStreamHandler(httpd_req_t *request) {
  static const char boundary[] = "\r\n--frame\r\n";
  httpd_resp_set_type(request, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  for (;;) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) return ESP_FAIL;
    char header[96];
    const int headerLength = snprintf(header, sizeof(header),
                                      "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                      static_cast<unsigned>(frame->len));
    esp_err_t result = httpd_resp_send_chunk(request, boundary, strlen(boundary));
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, header, headerLength);
    if (result == ESP_OK) result = httpd_resp_send_chunk(
        request, reinterpret_cast<const char *>(frame->buf), frame->len);
    esp_camera_fb_return(frame);
    if (result != ESP_OK) return result;
  }
}

void addAudioClient(int fd) {
  portENTER_CRITICAL(&audioClientsLock);
  for (int &client : audioClients) {
    if (client == fd) {
      portEXIT_CRITICAL(&audioClientsLock);
      return;
    }
  }
  for (int &client : audioClients) {
    if (client < 0) {
      client = fd;
      break;
    }
  }
  portEXIT_CRITICAL(&audioClientsLock);
}

void removeAudioClient(int fd) {
  portENTER_CRITICAL(&audioClientsLock);
  for (int &client : audioClients) if (client == fd) client = -1;
  portEXIT_CRITICAL(&audioClientsLock);
}

esp_err_t audioWebsocketHandler(httpd_req_t *request) {
  const int fd = httpd_req_to_sockfd(request);
  if (request->method == HTTP_GET) {
    addAudioClient(fd);
    return ESP_OK;
  }
  httpd_ws_frame_t frame{};
  const esp_err_t result = httpd_ws_recv_frame(request, &frame, 0);
  if (result != ESP_OK || frame.type == HTTPD_WS_TYPE_CLOSE) {
    removeAudioClient(fd);
  }
  return result;
}

void audioTask(void *) {
  int32_t raw[320];
  int16_t pcm[320];
  for (;;) {
    size_t bytesRead = 0;
    if (i2s_read(I2S_NUM_1, raw, sizeof(raw), &bytesRead, portMAX_DELAY) != ESP_OK) continue;
    const size_t count = min(bytesRead / sizeof(raw[0]), static_cast<size_t>(320));
    for (size_t i = 0; i < count; ++i) {
      int32_t sample = raw[i] >> 14;
      pcm[i] = static_cast<int16_t>(constrain(sample, INT16_MIN, INT16_MAX));
    }
    if (audioUploadMutex && audioBuildBuffer && audioSendBuffer &&
        xSemaphoreTake(audioUploadMutex, 0) == pdTRUE) {
      size_t source = 0;
      while (source < count) {
        const size_t room = 16000 - audioBuildSamples;
        const size_t amount = min(room, count - source);
        memcpy(audioBuildBuffer + audioBuildSamples, pcm + source, amount * sizeof(int16_t));
        audioBuildSamples += amount;
        source += amount;
        if (audioBuildSamples == 16000) {
          if (!audioUploadPending) {
            int16_t *swap = audioSendBuffer;
            audioSendBuffer = audioBuildBuffer;
            audioBuildBuffer = swap;
            audioSendSamples = audioBuildSamples;
            audioBuildSamples = 0;
            audioUploadPending = true;
            ++audioUploadSequence;
          } else {
            // Keep the newest complete second after the in-flight upload finishes.
            audioBuildSamples = 0;
          }
        }
      }
      xSemaphoreGive(audioUploadMutex);
    }
    int clients[4];
    portENTER_CRITICAL(&audioClientsLock);
    memcpy(clients, audioClients, sizeof(clients));
    portEXIT_CRITICAL(&audioClientsLock);
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = reinterpret_cast<uint8_t *>(pcm);
    frame.len = count * sizeof(pcm[0]);
    for (int fd : clients) {
      if (fd < 0 || !audioServer) continue;
      if (httpd_ws_get_fd_info(audioServer, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
          httpd_ws_send_frame_async(audioServer, fd, &frame) != ESP_OK) {
        removeAudioClient(fd);
        httpd_sess_trigger_close(audioServer, fd);
      }
    }
  }
}

bool initMicrophone() {
  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = 16000;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 320;
  config.use_apll = false;
  i2s_pin_config_t pins{};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = 14;
  pins.ws_io_num = 15;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = 13;
  if (i2s_driver_install(I2S_NUM_1, &config, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_1);
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
  return xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, nullptr, 1) == pdPASS;
}

void startMediaServers() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cameraReady && !videoServer) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 6144;
    if (httpd_start(&videoServer, &config) == ESP_OK) {
      httpd_uri_t root{};
      root.uri = "/";
      root.method = HTTP_GET;
      root.handler = rootHandler;
      httpd_uri_t stream{};
      stream.uri = "/stream";
      stream.method = HTTP_GET;
      stream.handler = videoStreamHandler;
      httpd_register_uri_handler(videoServer, &root);
      httpd_register_uri_handler(videoServer, &stream);
    }
  }
  if (!audioServer) {
    httpd_config_t audioConfig = HTTPD_DEFAULT_CONFIG();
    audioConfig.server_port = 82;
    audioConfig.ctrl_port = 32769;
    audioConfig.stack_size = 4096;
    if (httpd_start(&audioServer, &audioConfig) == ESP_OK) {
      httpd_uri_t audio{};
      audio.uri = "/audio";
      audio.method = HTTP_GET;
      audio.handler = audioWebsocketHandler;
      audio.is_websocket = true;
      httpd_register_uri_handler(audioServer, &audio);
    }
  }
  mediaServersStarted = videoServer || audioServer;
}

}  // namespace

void setup() {
  pinMode(DEBUG_UART_TX_PIN, OUTPUT);
  digitalWrite(DEBUG_UART_TX_PIN, LOW);
  if (DEBUG_UART_ENABLED) {
    DebugSerial.begin(DEBUG_UART_BAUD, SERIAL_8N1, -1, DEBUG_UART_TX_PIN);
    delay(50);
    DEBUG_PRINTF("\r\nESP32-CAM debug boot reset=%d heap=%u\r\n",
                 static_cast<int>(esp_reset_reason()), ESP.getFreeHeap());
  }
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32-CAM RA8 gateway boot");
  ++bootCount;
  statusMutex = xSemaphoreCreateMutex();
  matrixMutex = xSemaphoreCreateMutex();
  eventQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(SafetyEvent));
  if (!statusMutex || !matrixMutex || !eventQueue) ESP.restart();

  const uint32_t mediaCaps = psramFound() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT;
  websocketBinaryBuffer = static_cast<uint8_t *>(
      heap_caps_malloc(NETWORK_BINARY_BUFFER_SIZE, mediaCaps));
  if (!websocketBinaryBuffer) ESP.restart();

  Ra8Serial.setRxBufferSize(16384);
  Ra8Serial.begin(RA8_UART_BAUD, SERIAL_8N1, RA8_UART_RX_PIN, RA8_UART_TX_PIN);
  cameraReady = false;

  if (xTaskCreatePinnedToCore(uartTask, "ra8_uart", 8192, nullptr, 4, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 2, nullptr, 0) != pdPASS ||
      xTaskCreatePinnedToCore(networkWatchdogTask, "net_watchdog", 2048, nullptr, 3, nullptr, 1) != pdPASS) {
    ESP.restart();
  }
}

void loop() {
  if (networkHeartbeatMs && millis() - networkHeartbeatMs > NETWORK_STALL_RESTART_MS) {
    ++networkStallRestarts;
    delay(50);
    ESP.restart();
  }
  delay(1000);
}
