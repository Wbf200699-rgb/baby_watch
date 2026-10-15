from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
THREAD = (ROOT / "Multicore_top_CPU0" / "src" / "esp32_uart_thread_entry.c").read_text(
    encoding="utf-8"
)
PROTOCOL = (
    ROOT / "Multicore_top_CPU0" / "src" / "thread_code" / "ra8_esp32_protocol.c"
).read_text(encoding="utf-8")
DRIVER = (
    ROOT / "Multicore_top_CPU0" / "src" / "thread_code" / "esp32_uart_driver.c"
).read_text(encoding="utf-8")
DRIVER_HEADER = (
    ROOT / "Multicore_top_CPU0" / "src" / "thread_code" / "esp32_uart_driver.h"
).read_text(encoding="utf-8")


def test_gateway_ready_is_freshness_gated() -> None:
    assert "RA8_ESP32_GATEWAY_READY_TIMEOUT_MS  (5000U)" in PROTOCOL
    assert "xTaskGetTickCount() - g_protocol_gateway_ready_tick" in PROTOCOL
    assert "ra8_esp32_protocol_gateway_ready() ? 1U : 0U" in PROTOCOL


def test_rx_poll_has_a_bounded_work_budget() -> None:
    assert "RA8_ESP32_RX_POLL_BUDGET_BYTES" in PROTOCOL
    assert "g_protocol_rx_budget_hit_count++" in PROTOCOL


def test_bulk_upload_waits_for_gateway_during_boot_grace() -> None:
    poll = THREAD.index("static void esp32_uart_rx_task(")
    ready = THREAD.index("ra8_esp32_protocol_gateway_ready();", poll)
    fallback = THREAD.index("bool fallback_upload", ready)
    status = THREAD.index("ra8_esp32_protocol_send_status", fallback)
    assert poll < ready < fallback < status
    assert "ESP32_UART_GATEWAY_WAIT_MS        (10000U)" in THREAD
    assert "if (!gateway_ready && !fallback_upload)" in THREAD
    assert "continue;" in THREAD[ready:status]


def test_missing_reverse_link_uses_bounded_matrix_fallback() -> None:
    assert "ESP32_UART_FALLBACK_STATUS_MS      (1000U)" in THREAD
    assert "ESP32_UART_FALLBACK_THERMAL_MS     (1000U)" in THREAD
    assert "ESP32_UART_FALLBACK_PRESSURE_MS    (500U)" in THREAD
    assert "gateway_ready ?" in THREAD
    assert "ESP32_UART_FALLBACK_PRESSURE_MS" in THREAD
    assert "ESP32_UART_FALLBACK_THERMAL_MS" in THREAD


def test_complete_matrices_are_uploaded_after_handshake() -> None:
    assert "ESP32_UART_UPLOAD_PRESSURE_MATRIX (1U)" in THREAD
    assert "ESP32_UART_THERMAL_INTERVAL_MS    (500U)" in THREAD
    assert "ESP32_UART_PRESSURE_INTERVAL_MS   (125U)" in THREAD
    assert "THERMAL_IPC_STATUS_FRAME_OK == p_thm->status" in THREAD
    assert "PRESSURE_IPC_STATUS_SCAN_OK == p_prs->status" in THREAD
    assert "ra8_esp32_protocol_send_thermal_matrix" in THREAD
    assert "ra8_esp32_protocol_send_pressure_matrix" in THREAD


def test_uart_rx_is_decoupled_and_errors_are_non_destructive() -> None:
    assert "ESP32_UART_GATEWAY_RECOVERY_MS" not in THREAD
    assert "ESP32_UART_RX_TASK_PRIORITY        (configMAX_PRIORITIES - 3U)" in THREAD
    assert "esp32_uart_rx_task_start();" in THREAD
    assert "uart_stats.error_count != last_uart_error_count" not in THREAD
    assert "esp32_uart_driver_recover()" not in THREAD
    assert "g_esp32_uart_rx_error_observed = stats.error_count" in THREAD
    assert "esp32_uart_apply_baud()" in DRIVER
    assert "recovery_count++" in DRIVER
    assert '",\\\"recover\\\":"' in PROTOCOL


def test_rx_buffer_preserves_fresh_frames_under_backpressure() -> None:
    assert "ESP32_UART_RX_BUFFER_SIZE    (32768U)" in DRIVER_HEADER
    assert "rx_tail = esp32_uart_next_index" in DRIVER
    overflow = DRIVER.index("stats.rx_overflow_count++")
    store = DRIVER.index("rx_buffer[g_esp32_uart_context.rx_head] = byte", overflow)
    assert overflow < store


def test_sci_error_events_preserve_received_data() -> None:
    rx_case = DRIVER.index("case UART_EVENT_RX_CHAR:")
    framing_case = DRIVER.index("case UART_EVENT_ERR_FRAMING:", rx_case)
    push = DRIVER.index("esp32_uart_push_byte((uint8_t) p_args->data);", framing_case)
    assert rx_case < framing_case < push


def test_protocol_serializes_shared_tx_frame() -> None:
    assert "xSemaphoreCreateBinaryStatic(&g_protocol_tx_lock_storage)" in PROTOCOL
    take = PROTOCOL.index("xSemaphoreTake(g_protocol_tx_lock, timeout_ticks)")
    build = PROTOCOL.index("p_frame[0] = RA8_ESP32_PROTOCOL_MAGIC_MSB", take)
    give = PROTOCOL.index("xSemaphoreGive(g_protocol_tx_lock)", build)
    assert take < build < give
