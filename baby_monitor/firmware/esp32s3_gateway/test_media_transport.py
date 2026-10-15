from pathlib import Path


ROOT = Path(__file__).resolve().parent
CAMERA = (ROOT / "main" / "media" / "camera_stream.c").read_text(encoding="utf-8")
MICROPHONE = (ROOT / "main" / "media" / "mic_stream.c").read_text(encoding="utf-8")
PROTOCOL = (ROOT / "main" / "media" / "media_protocol.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main" / "media" / "media_protocol.h").read_text(encoding="utf-8")
GATEWAY = (ROOT / "main" / "gateway_main.c").read_text(encoding="utf-8")
SDKCONFIG_DEFAULTS = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")


def test_media_sessions_reset_on_every_connection_and_close_path() -> None:
    for source, name in ((CAMERA, "video"), (MICROPHONE, "audio")):
        assert "WEBSOCKET_EVENT_CONNECTED" in source
        assert "WEBSOCKET_EVENT_CLOSED" in source
        assert "reset_control_session();" in source
        assert f"s_{name}_enabled = false;" in source
        assert "s_has_control_sequence = false;" in source
        assert "s_last_control_sequence = 0;" in source
        assert "++s_session_generation;" in source


def test_media_controls_support_fragmented_websocket_text() -> None:
    for source in (CAMERA, MICROPHONE):
        assert "receive_control_fragment" in source
        assert "data->payload_offset == 0" in source
        assert "data->payload_offset + data->data_len" in source
        assert "s_control_text_length == s_control_text_expected" in source


def test_media_links_are_kept_alive_without_subscribers() -> None:
    for source in (CAMERA, MICROPHONE):
        assert "media.heartbeat" in source
        assert "MEDIA_HEARTBEAT_INTERVAL_MS 15000U" in source
        assert ".enable_close_reconnect = true" in source
        assert ".keep_alive_enable = true" in source
        assert ".ping_interval_sec = MEDIA_WSS_PING_INTERVAL_SEC" in source
        assert ".disable_pingpong_discon = true" in source
        assert ".reconnect_timeout_ms = 1000" in source


def test_media_frames_mark_capture_discontinuities() -> None:
    assert "MEDIA_FLAG_DISCONTINUITY" in HEADER
    assert "put_u16_be(header + 6, flags);" in PROTOCOL
    for source in (CAMERA, MICROPHONE):
        assert "mark_discontinuity();" in source
        assert "take_discontinuity();" in source
        assert "frame.sequence != last_sent_sequence + 1U" in source
        assert "discontinuity ? MEDIA_FLAG_DISCONTINUITY : 0" in source


def test_media_queues_remain_bounded_and_independent() -> None:
    assert "#define VIDEO_QUEUE_LENGTH 1" in CAMERA
    assert "#define MIC_QUEUE_LENGTH 8" in MICROPHONE
    assert "xQueueReceive(s_video_queue, &discarded, 0)" in CAMERA
    assert "xQueueReceive(s_audio_queue, &discarded, 0)" in MICROPHONE


def test_media_tls_startup_preserves_internal_sram() -> None:
    assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in SDKCONFIG_DEFAULTS
    assert "# CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC is not set" in SDKCONFIG_DEFAULTS
    assert "mic_stream_is_connected()" in GATEWAY
    assert "now_ms() - audio_wait_started < 8000U" in GATEWAY
