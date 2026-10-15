import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parent / "main" / "gateway_main.c").read_text(
    encoding="utf-8"
)
MIC_SOURCE = (Path(__file__).resolve().parent / "main" / "mic_stream.c").read_text(
    encoding="utf-8"
)
CAMERA_SOURCE = (
    Path(__file__).resolve().parent / "main" / "camera_stream.c"
).read_text(encoding="utf-8")
TX_GUARD_SOURCE = (
    Path(__file__).resolve().parent / "main" / "realtime_tx_guard.c"
).read_text(encoding="utf-8")
SDKCONFIG_DEFAULTS = (
    Path(__file__).resolve().parent / "sdkconfig.defaults"
).read_text(encoding="utf-8")


class GatewayProtocolContractTest(unittest.TestCase):
    def test_thermal_header_is_rows_then_columns(self) -> None:
        self.assertIn("thermal is rows=64 and cols=80", SOURCE)
        self.assertIn("frame[6] = (uint8_t)(matrix->height >> 8);", SOURCE)
        self.assertIn("frame[8] = (uint8_t)(matrix->width >> 8);", SOURCE)
        self.assertNotIn(
            "frame[6] = (uint8_t)(matrix->width >> 8); frame[7]", SOURCE
        )

    def test_environment_capabilities_are_advertised(self) -> None:
        self.assertIn('cJSON_CreateString("environment_scd41")', SOURCE)
        self.assertIn('cJSON_CreateString("environment_bme688")', SOURCE)

    def test_scd41_and_bme688_use_canonical_environment_path(self) -> None:
        self.assertIn(
            'cJSON *scd = cJSON_GetObjectItemCaseSensitive(ra8, "scd");', SOURCE
        )
        self.assertIn(
            'cJSON *bme = cJSON_GetObjectItemCaseSensitive(ra8, "bme");', SOURCE
        )
        self.assertIn('cJSON_AddItemToObject(environment, "bme688", bme_copy);', SOURCE)
        self.assertIn('cJSON_AddItemToObject(sensors, "environment", environment);', SOURCE)
        self.assertNotIn('add_duplicate_if_object(sensors, "bme688", bme);', SOURCE)

    def test_ra8_diagnostics_remain_complete_objects(self) -> None:
        self.assertIn('add_duplicate_if_object(sensors, "microphone", mic);', SOURCE)
        self.assertIn('add_duplicate_if_object(sensors, "audio_player", aud);', SOURCE)
        self.assertIn('add_duplicate_if_object(sensors, "camera", cam);', SOURCE)
        self.assertIn('add_duplicate_if_object(sensors, "warm_light", light);', SOURCE)
        self.assertNotIn('cJSON_DeleteItemFromObjectCaseSensitive(copy, "wst")', SOURCE)
        self.assertNotIn('cJSON_DeleteItemFromObjectCaseSensitive(copy, "werr")', SOURCE)
        self.assertNotIn('cJSON_DeleteItemFromObjectCaseSensitive(copy, "wbytes")', SOURCE)

    def test_existing_telemetry_wss_recovery_is_preserved(self) -> None:
        self.assertIn("TELEMETRY_STALL_MS", SOURCE)
        self.assertIn("request_telemetry_ws_recovery", SOURCE)
        self.assertIn("ws_recovery_count", SOURCE)

    def test_telemetry_recovery_runs_outside_the_realtime_loop(self) -> None:
        self.assertIn("#define TELEMETRY_STALL_MS 15000", SOURCE)
        self.assertIn("#define TELEMETRY_DISCONNECTED_RECOVERY_MS 30000", SOURCE)
        self.assertIn("s_ws_connected && esp_websocket_client_is_connected(s_ws)", SOURCE)
        recovery_start = SOURCE.index("static void telemetry_ws_recovery_task")
        recovery_end = SOURCE.index("static void start_telemetry_ws_recovery", recovery_start)
        self.assertNotIn("esp_restart()", SOURCE[recovery_start:recovery_end])

    def test_persistent_telemetry_failure_has_bounded_self_restart(self) -> None:
        self.assertIn("#define TELEMETRY_SELF_RESTART_MS 180000", SOURCE)
        self.assertIn("#define TELEMETRY_STABLE_CLEAR_MS 600000", SOURCE)
        self.assertIn("#define TELEMETRY_SELF_RESTART_LIMIT 2U", SOURCE)
        self.assertIn("RTC_DATA_ATTR static uint32_t s_ws_self_restart_attempts", SOURCE)
        self.assertIn("unrecovered_ms >= TELEMETRY_SELF_RESTART_MS", SOURCE)
        self.assertIn("s_ws_self_restart_attempts < TELEMETRY_SELF_RESTART_LIMIT", SOURCE)
        self.assertIn("!story_restart_guard", SOURCE)
        self.assertIn('"ws_self_restart_attempts"', SOURCE)

    def test_p4_soft_reset_uses_accessible_open_drain_gpio(self) -> None:
        self.assertIn("#define RA8_SOFT_RESET_PIN GPIO_NUM_10", SOURCE)
        self.assertIn(".mode = GPIO_MODE_OUTPUT_OD", SOURCE)
        self.assertIn("gpio_set_level(RA8_SOFT_RESET_PIN, 0);", SOURCE)
        self.assertIn("gpio_set_level(RA8_SOFT_RESET_PIN, 1);", SOURCE)
        self.assertIn("RA8_RESET_BOOT_GRACE_MS", SOURCE)
        self.assertIn("RA8_RESET_COOLDOWN_MS", SOURCE)
        self.assertIn("RA8_RESET_LINK_SILENCE_MS", SOURCE)
        self.assertIn("s_stats.last_valid_frame_ms = now_ms();", SOURCE)
        self.assertIn("static uint32_t elapsed_ms", SOURCE)
        self.assertIn("delta < 0 ? 0U", SOURCE)
        self.assertIn("status_stale && transport_silent", SOURCE)
        self.assertIn('"ra8_transport_online"', SOURCE)

    def test_p4_recovers_its_uart_before_resetting_ra8(self) -> None:
        self.assertIn("#define RA8_UART_RX_RECOVERY_MS 8000", SOURCE)
        self.assertIn("#define RA8_UART_RX_RECOVERY_COOLDOWN_MS 15000", SOURCE)
        self.assertIn("static esp_err_t recover_ra8_uart_rx", SOURCE)
        self.assertIn("uart_wait_tx_done(RA8_UART", SOURCE)
        self.assertIn("uart_driver_delete(RA8_UART)", SOURCE)
        self.assertIn("configure_ra8_uart_driver()", SOURCE)
        self.assertIn("state = WAIT_AA;", SOURCE)
        self.assertIn("local_uart_recovered", SOURCE)
        self.assertIn('"uart_rx_bytes"', SOURCE)
        self.assertIn('"uart_raw_rx_age_ms"', SOURCE)
        self.assertIn('"uart_rx_pin_level"', SOURCE)
        self.assertIn('"uart_rx_pin_transitions"', SOURCE)
        self.assertIn('"uart_rx_recovery_count"', SOURCE)

    def test_realtime_network_buffers_use_psram_with_bounded_windows(self) -> None:
        self.assertIn("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM=64", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM=64", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65534", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_LWIP_TCP_WND_DEFAULT=65534", SDKCONFIG_DEFAULTS)
        self.assertIn("#define TELEMETRY_WSS_BUFFER_BYTES (12U * 1024U)", SOURCE)
        self.assertIn("#define MATRIX_WSS_BUFFER_BYTES (12U * 1024U)", SOURCE)
        self.assertIn(".buffer_size = TELEMETRY_WSS_BUFFER_BYTES", SOURCE)
        self.assertIn(".buffer_size = MATRIX_WSS_BUFFER_BYTES", SOURCE)
        self.assertIn("#define VIDEO_WSS_BUFFER_BYTES (32U * 1024U)", CAMERA_SOURCE)
        self.assertIn(".buffer_size = VIDEO_WSS_BUFFER_BYTES", CAMERA_SOURCE)
        self.assertIn('"telemetry_ws_buffer_bytes"', SOURCE)
        self.assertIn('"matrix_ws_buffer_bytes"', SOURCE)

    def test_internal_heap_fragmentation_is_reported(self) -> None:
        self.assertIn('"min_free_internal_heap"', SOURCE)
        self.assertIn('"largest_internal_block"', SOURCE)

    def test_audio_small_frames_use_nodelay_and_serialized_client_io(self) -> None:
        self.assertIn(".ext_transport = s_audio_transport", MIC_SOURCE)
        self.assertIn("setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY", MIC_SOURCE)
        self.assertIn("WEBSOCKET_EVENT_CONNECTED", MIC_SOURCE)
        self.assertIn("enable_audio_tcp_nodelay();", MIC_SOURCE)
        self.assertIn("CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=n", SDKCONFIG_DEFAULTS)
        self.assertIn('"audio_nodelay_failures"', SOURCE)
        self.assertIn('"audio_separate_tx_lock"', SOURCE)

    def test_media_control_sequence_resets_on_every_new_wss_connection(self) -> None:
        for media_source, enabled_name in (
            (MIC_SOURCE, "s_audio_enabled"),
            (CAMERA_SOURCE, "s_video_enabled"),
        ):
            connected = media_source.index(
                "if (event_id == WEBSOCKET_EVENT_CONNECTED)"
            )
            disconnected = media_source.index(
                "WEBSOCKET_EVENT_DISCONNECTED", connected
            )
            connected_block = media_source[connected:disconnected]
            self.assertIn("s_has_control_sequence = false;", connected_block)
            self.assertIn(f"{enabled_name} = false;", connected_block)
            error = media_source.index("WEBSOCKET_EVENT_ERROR", disconnected)
            error_block = media_source[error : error + 320]
            self.assertIn("s_has_control_sequence = false;", error_block)
            self.assertIn(f"{enabled_name} = false;", error_block)

        self.assertIn('"video_control_count"', SOURCE)
        self.assertIn('"audio_control_count"', SOURCE)

    def test_media_drops_on_cross_client_contention_without_touching_wss(self) -> None:
        self.assertIn("realtime_tx_guard_take(0)", MIC_SOURCE)
        self.assertIn("realtime_tx_guard_take(0)", CAMERA_SOURCE)
        self.assertIn("audio_arbitration_drops", SOURCE)
        self.assertIn("video_arbitration_drops", SOURCE)
        self.assertIn("esp_transport_poll_write(s_audio_transport, 0)", MIC_SOURCE)
        self.assertIn("esp_transport_poll_write(s_video_transport, 0)", CAMERA_SOURCE)
        self.assertIn("audio_backpressure_drops", SOURCE)
        self.assertIn("video_backpressure_drops", SOURCE)
        self.assertIn("xSemaphoreCreateMutex()", TX_GUARD_SOURCE)
        self.assertIn("realtime_tx_guard_take(0)", SOURCE)

    def test_video_fragments_one_complete_websocket_message(self) -> None:
        self.assertIn(
            "MEDIA_HEADER_SIZE + MEDIA_VIDEO_MAX_PAYLOAD_SIZE", CAMERA_SOURCE
        )
        self.assertIn(
            "memcpy(wire + MEDIA_HEADER_SIZE, frame.data, frame.length);",
            CAMERA_SOURCE,
        )
        self.assertIn("#define VIDEO_FRAGMENT_BYTES 4096U", CAMERA_SOURCE)
        self.assertIn("esp_websocket_client_send_bin_partial(", CAMERA_SOURCE)
        self.assertIn("esp_websocket_client_send_cont_msg(", CAMERA_SOURCE)
        self.assertIn("esp_websocket_client_send_fin(", CAMERA_SOURCE)
        self.assertIn("send_video_message(wire, wire_length)", CAMERA_SOURCE)
        self.assertNotIn("int sent = esp_websocket_client_send_bin(", CAMERA_SOURCE)

    def test_events_and_command_acks_reuse_telemetry_websocket(self) -> None:
        self.assertIn('websocket_send_json("event", body)', SOURCE)
        self.assertIn('websocket_send_json("command_ack", body)', SOURCE)
        self.assertIn('s_server_acked_event == event.event_id', SOURCE)
        self.assertNotIn('http_post("/api/v1/events"', SOURCE)
        self.assertNotIn('"/api/v1/commands/%lu/ack"', SOURCE)
        self.assertIn('"event_ws_failures"', SOURCE)
        self.assertIn('"command_ack_ws_failures"', SOURCE)

    def test_blocked_telemetry_recovery_forces_a_bounded_restart(self) -> None:
        self.assertIn("#define TELEMETRY_RECOVERY_TIMEOUT_MS 30000", SOURCE)
        recovery_branch = SOURCE.index("if (s_ws_recovery_running) {")
        wifi_branch = SOURCE.index(
            "if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT))",
            recovery_branch,
        )
        recovery_source = SOURCE[recovery_branch:wifi_branch]
        self.assertIn(
            "recovery_elapsed_ms >= TELEMETRY_RECOVERY_TIMEOUT_MS",
            recovery_source,
        )
        self.assertIn("!story_restart_guard", recovery_source)
        self.assertIn(
            "s_ws_self_restart_attempts < TELEMETRY_SELF_RESTART_LIMIT",
            recovery_source,
        )
        self.assertIn("esp_restart();", recovery_source)

    def test_soother_command_sets_an_idempotent_motor_state(self) -> None:
        self.assertIn('"soother.set"', SOURCE)
        self.assertIn('"soother_on" : "soother_off"', SOURCE)
        self.assertNotIn('"soother_press:%" PRIu32', SOURCE)
        self.assertIn('add_duplicate_if_object(sensors, "soother", pat);', SOURCE)

    def test_p4_updates_mismatched_c6_with_bounded_embedded_ota(self) -> None:
        self.assertIn('C6_OTA_EXPECTED_VERSION "2.12.12"', SOURCE)
        self.assertIn("C6_OTA_IMAGE_REVISION", SOURCE)
        self.assertIn("C6_OTA_REVISION_NVS_KEY", SOURCE)
        self.assertIn("validate_embedded_c6_firmware", SOURCE)
        self.assertIn("esp_hosted_slave_ota_begin()", SOURCE)
        self.assertIn("esp_hosted_slave_ota_write(", SOURCE)
        self.assertIn("esp_hosted_slave_ota_end()", SOURCE)
        self.assertIn("C6_OTA_MAX_ATTEMPTS 2U", SOURCE)
        self.assertIn('"esp_hosted_ota_result"', SOURCE)

    def test_hosted_sdio_checksum_is_enabled_and_reported(self) -> None:
        self.assertIn("CONFIG_SLAVE_IDF_TARGET_ESP32C6=y", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_ESP32P4_EYE_C6_BOARD=y", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y", SDKCONFIG_DEFAULTS)
        self.assertNotIn("CONFIG_ESP_HOSTED_CP_TARGET_ESP32H2=y", SDKCONFIG_DEFAULTS)
        self.assertNotIn("CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y", SDKCONFIG_DEFAULTS)
        self.assertIn("CONFIG_ESP_HOSTED_SDIO_CHECKSUM=y", SDKCONFIG_DEFAULTS)
        self.assertIn('"esp_hosted_sdio_checksum"', SOURCE)

        cmake = (
            Path(__file__).resolve().parent / "main" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("NOT CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6", cmake)
        self.assertIn("NOT CONFIG_ESP32P4_EYE_C6_BOARD", cmake)
        self.assertIn("NOT CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE", cmake)
        self.assertIn("requires ESP32-C6 over SDIO", cmake)

    def test_story_audio_transfer_is_bounded_and_integrity_checked(self) -> None:
        self.assertIn("#define STORY_CHUNK_BYTES 448U", SOURCE)
        self.assertIn("#define STORY_MAX_BYTES (4U * 1024U * 1024U)", SOURCE)
        self.assertIn("story_sha256_matches", SOURCE)
        self.assertIn("story_wav_valid", SOURCE)
        self.assertIn("story_crc32_update", SOURCE)
        self.assertIn("RA8_TYPE_FILE_BEGIN", SOURCE)
        self.assertIn("RA8_TYPE_FILE_CHUNK", SOURCE)
        self.assertIn("RA8_TYPE_FILE_END", SOURCE)
        self.assertIn("sample_rate == 8000U || sample_rate == 16000U", SOURCE)
        self.assertIn('"story stored on RA8 and queued for playback"', SOURCE)

    def test_dedicated_story_uart_has_bounded_backpressure_and_live_probe(self) -> None:
        self.assertIn("#define STORY_BULK_WINDOW 1U", SOURCE)
        self.assertIn("uart_param_config(STORY_BULK_UART", SOURCE)
        self.assertIn("uart_set_pin(STORY_BULK_UART", SOURCE)
        self.assertIn("uart_driver_install(STORY_BULK_UART", SOURCE)
        self.assertIn("uart_set_rx_full_threshold(STORY_BULK_UART, 1U)", SOURCE)
        self.assertIn("uart_set_rx_timeout(STORY_BULK_UART, 2U)", SOURCE)
        self.assertIn('story_bulk_link_task, "story_uart_link"', SOURCE)
        self.assertIn("story_bulk_handshake", SOURCE)
        self.assertIn("send it proactively", SOURCE)
        self.assertIn('"story_bulk_link_up"', SOURCE)
        self.assertIn('"story_bulk_uart_rx_bytes"', SOURCE)
        self.assertIn('"story_bulk_uart_tx_bytes"', SOURCE)

    def test_story_download_never_runs_in_realtime_network_loop(self) -> None:
        self.assertIn('xTaskCreatePinnedToCore(story_task, "story_transfer"', SOURCE)
        self.assertIn("s_story_queue = xQueueCreate(STORY_QUEUE_LENGTH", SOURCE)
        self.assertIn("s_story_pending_command_id", SOURCE)
        self.assertIn('if (!strcmp(command->command, "story.audio.play"))', SOURCE)
        self.assertIn("#define STORY_DOWNLOAD_ATTEMPTS 3U", SOURCE)
        self.assertIn("#define STORY_RA8_TRANSFER_ATTEMPTS 3U", SOURCE)
        self.assertIn("for (unsigned attempt = 0; attempt < STORY_DOWNLOAD_ATTEMPTS; ++attempt)", SOURCE)
        self.assertIn("for (unsigned attempt = 0; attempt < STORY_RA8_TRANSFER_ATTEMPTS; ++attempt)", SOURCE)
        self.assertIn("queue_command_ack(command.id, ok, message);", SOURCE)

    def test_story_download_quiesces_media_and_uses_bounded_reads(self) -> None:
        self.assertIn("#define STORY_DOWNLOAD_TIMEOUT_MS 60000U", SOURCE)
        self.assertIn("#define STORY_HTTP_READ_BYTES 8192U", SOURCE)
        self.assertIn("story_set_media_suspended(true)", SOURCE)
        self.assertIn("story_set_media_suspended(false)", SOURCE)
        self.assertIn("camera_stream_set_transport_suspended", SOURCE)
        self.assertIn("mic_stream_set_transport_suspended", SOURCE)
        self.assertIn("remaining > STORY_HTTP_READ_BYTES", SOURCE)
        self.assertIn('"story_received_bytes"', SOURCE)

    def test_story_prefers_dedicated_bulk_uart_and_keeps_legacy_fallback(self) -> None:
        self.assertIn("#define STORY_BULK_UART UART_NUM_2", SOURCE)
        self.assertIn("#define STORY_BULK_UART_TX_PIN 50", SOURCE)
        self.assertIn("#define STORY_BULK_UART_RX_PIN 51", SOURCE)
        self.assertIn("#define STORY_BULK_UART_BAUD 1000000", SOURCE)
        self.assertIn("#define STORY_BULK_PAYLOAD_BYTES 4096U", SOURCE)
        self.assertIn("#define STORY_BULK_WINDOW 1U", SOURCE)
        self.assertIn("story_bulk_uart_init()", SOURCE)
        self.assertIn("story_bulk_transfer_to_ra8", SOURCE)
        self.assertIn("STORY_BULK_TYPE_READY", SOURCE)
        self.assertIn("story_bulk_wait_ready", SOURCE)
        self.assertIn("s_story_bulk_handshake_successes", SOURCE)
        self.assertIn("s_story_bulk_handshake_failures", SOURCE)
        self.assertIn("s_story_bulk_fallbacks", SOURCE)
        self.assertIn("fallback reason=handshake_timeout", SOURCE)

        # The original framed transport remains as a compatibility fallback.
        self.assertIn("send_ra8_frame_tracked(frame_type, payload, length", SOURCE)
        self.assertIn("story_exchange(RA8_TYPE_FILE_BEGIN", SOURCE)
        self.assertIn("story_exchange(RA8_TYPE_FILE_CHUNK", SOURCE)
        self.assertIn("story_exchange(RA8_TYPE_FILE_END", SOURCE)

    def test_story_uart_exchange_retries_lost_idempotent_acks(self) -> None:
        self.assertIn("#define STORY_OPERATION_TIMEOUT_MS 150U", SOURCE)
        self.assertIn("#define STORY_ACK_POLL_MS 10U", SOURCE)
        self.assertIn("#define STORY_ACK_RETRY_BACKOFF_MS 5U", SOURCE)
        self.assertIn("#define STORY_OPERATION_ATTEMPTS 20U", SOURCE)
        self.assertIn("#define STORY_CHUNK_PACING_MS 0U", SOURCE)
        self.assertIn("#define STORY_TASK_PRIORITY 7U", SOURCE)
        self.assertIn("STORY_TASK_PRIORITY, NULL, 0", SOURCE)
        self.assertIn("#define STORY_RESET_GRACE_MS 10000U", SOURCE)
        self.assertIn("attempt < STORY_OPERATION_ATTEMPTS", SOURCE)
        self.assertIn("candidate.result == STORY_RESULT_BUSY", SOURCE)
        self.assertIn("!story_reset_guard", SOURCE)
        self.assertIn('"story_exchange_retries"', SOURCE)
        self.assertIn('"story_exchange_timeouts"', SOURCE)
        self.assertIn("RA8 story transfer failed at offset=", SOURCE)


if __name__ == "__main__":
    unittest.main()
