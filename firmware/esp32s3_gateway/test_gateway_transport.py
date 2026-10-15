from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = (ROOT / "main" / "gateway_main.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
DEFAULTS = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")


def test_realtime_transport_has_left_blocking_https() -> None:
    assert "esp_http_client_perform" not in SOURCE
    assert '"/api/v1/batch"' not in SOURCE
    assert "init_http()" not in SOURCE
    assert "esp_http_client" not in CMAKE
    assert "init_websocket();" in SOURCE


def test_telemetry_uses_fixed_2hz_latest_only_slot() -> None:
    assert "#define TELEMETRY_INTERVAL_MS 500" in SOURCE
    assert "telemetry_producer_task" in SOURCE
    assert (
        "vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));"
        in SOURCE
    )
    assert "s_telemetry_latest" in SOURCE
    assert "s_telemetry_generation != s_telemetry_copied_generation" in SOURCE
    assert "s_telemetry_inflight" in SOURCE
    assert "s_telemetry_inflight_sequence == sequence" in SOURCE
    assert "TELEMETRY_ACK_TIMEOUT_MS" in SOURCE
    assert 'prefix[] = "{\\"kind\\":\\"telemetry\\",\\"payload\\":";' in SOURCE


def test_matrices_use_an_independent_latest_only_wss() -> None:
    assert '#define MATRIX_WSS_URI "wss://renesas.zyxserver.online/api/v1/device/matrices"' in SOURCE
    assert "#define PRESSURE_INTERVAL_MS 250" in SOURCE
    assert "#define THERMAL_INTERVAL_MS 500" in SOURCE
    assert 'memcpy(frame, "RA8M", 4);' in SOURCE
    assert "static esp_websocket_client_handle_t s_matrix_ws;" in SOURCE
    assert "matrix_network_task" in SOURCE
    assert "matrix_websocket_event_handler" in SOURCE
    assert "esp_websocket_client_send_bin(\n        s_matrix_ws," in SOURCE
    assert "delivery->upload_sequence" in SOURCE
    assert "delivery->source_sequence" in SOURCE
    assert "retrying identical frame" in SOURCE
    assert "#define MATRIX_ACK_TIMEOUT_MS 2000" in SOURCE
    assert "delivery->source->pending = false;" in SOURCE
    assert "MATRIX_SEND_BUDGET_MS" not in SOURCE
    assert "MATRIX_CONGESTION_THRESHOLD_MS" not in SOURCE
    assert "MATRIX_SUSPEND_MS" not in SOURCE
    assert "#define WSS_SEND_TIMEOUT_MS 3000" in SOURCE
    assert "#define TELEMETRY_ACK_TIMEOUT_MS 4000" in SOURCE
    assert "#define TELEMETRY_ACK_RESTART_THRESHOLD 3" in SOURCE
    assert "retrying latest on current WSS" in SOURCE
    assert "s_consecutive_telemetry_ack_timeouts = 0;" in SOURCE
    assert "#define MATRIX_WSS_SEND_TIMEOUT_MS 5000" in SOURCE
    assert "prefer_thermal = !prefer_thermal;" in SOURCE
    assert "suspend_matrices_for_congestion" not in SOURCE
    assert "rows=64 and cols=80" in SOURCE
    assert "add_sensor_summary" in SOURCE
    assert '"pressure",' in SOURCE
    assert "pressure_fields" in SOURCE


def test_compact_sd_diagnostics_remain_in_realtime_telemetry() -> None:
    for field in (
        "sdr",
        "sds",
        "sdfs",
        "sdfl",
        "sdtry",
        "sdfail",
        "sdrec",
        "sdfsp",
        "sdff",
    ):
        assert f'"{field}"' in SOURCE
    assert "CPU1 SD diagnostics are published by RA8 inside the mic object" in SOURCE


def test_scd41_is_advertised_and_forwarded_as_compact_environment() -> None:
    assert 'cJSON_CreateString("environment_scd41")' in SOURCE
    assert 'cJSON *scd = cJSON_GetObjectItemCaseSensitive(ra8, "scd");' in SOURCE
    fields_start = SOURCE.index("static const char *environment_fields[]")
    fields_end = SOURCE.index("};", fields_start)
    environment_fields = SOURCE[fields_start:fields_end]
    for field in (
        "seq",
        "s",
        "co2",
        "t_mc",
        "rh_mp",
        "ready",
        "crc_err",
        "i2c_err",
        "to",
        "rec",
        "fsp",
        "last",
    ):
        assert f'"{field}"' in environment_fields
    assert (
        'add_sensor_summary(sensors,\n'
        '                       "environment",\n'
        '                       scd,\n'
        '                       environment_fields,'
    ) in SOURCE


def test_wss_reconnect_and_bidirectional_control_contract() -> None:
    assert ".enable_close_reconnect = true" in SOURCE
    assert "WEBSOCKET_EVENT_CLOSED" in SOURCE
    for kind in ("hello", "event", "command_ack", "heartbeat"):
        assert f'websocket_send_json("{kind}"' in SOURCE
    for ack in (
        "hello_ack",
        "telemetry_ack",
        "event_ack",
        "matrix_ack",
        "command_ack_received",
        "heartbeat_ack",
    ):
        assert f'"{ack}"' in SOURCE
    assert "s_server_acked_event_valid" in SOURCE
    assert "s_server_acked_telemetry_valid" in SOURCE
    assert "COMMAND_RESULT_CACHE_LENGTH 16" in SOURCE
    assert "find_command_result(command->id)" in SOURCE
    assert "now - s_last_telemetry_produced_ms >= HEARTBEAT_INTERVAL_MS" in SOURCE
    assert "#define WSS_PING_INTERVAL_SEC 300" in SOURCE
    assert ".disable_pingpong_discon = true" in SOURCE
    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096" in DEFAULTS
    assert "# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set" in DEFAULTS
    assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in DEFAULTS
    assert "# CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC is not set" in DEFAULTS
    assert "CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y" in DEFAULTS
    assert "CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=5000" in DEFAULTS
    assert "portENTER_CRITICAL(&s_telemetry_ack_mux)" in SOURCE
    assert "s_telemetry_inflight_sent_ms = 0;" in SOURCE
    assert "s_telemetry_inflight_sent_ms = now_ms();" in SOURCE
    assert ".keep_alive_enable = true" in SOURCE
    assert ".reconnect_timeout_ms = 1000" in SOURCE
    assert ".task_prio = 8" in SOURCE


def test_gateway_reserves_priority_path_before_media() -> None:
    network_task = SOURCE.index(
        'xTaskCreatePinnedToCore(network_task, "gateway_net"'
    )
    gateway_wss = SOURCE.index("    init_websocket();", network_task)
    matrix_task = SOURCE.index(
        'matrix_network_task, "matrix_net"', gateway_wss
    )
    matrix_wss = SOURCE.index("    init_matrix_websocket();", matrix_task)
    microphone = SOURCE.index("    ESP_ERROR_CHECK(mic_stream_start());", matrix_wss)
    camera = SOURCE.index("    ESP_ERROR_CHECK(camera_stream_start());", microphone)
    assert network_task < gateway_wss < matrix_task < matrix_wss < microphone < camera
    assert "while (!s_ws_connected)" in SOURCE[gateway_wss:microphone]
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in SOURCE
