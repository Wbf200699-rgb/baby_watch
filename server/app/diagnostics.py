from __future__ import annotations

from typing import Any


SOURCE_MIC = 1 << 0
SOURCE_THERMAL = 1 << 1
SOURCE_TEMPERATURE = 1 << 2
SOURCE_PRESSURE = 1 << 3
SOURCE_CAMERA = 1 << 4
SOURCE_ENVIRONMENT = 1 << 5

FAULT_THERMAL = 1 << 0
FAULT_TEMPERATURE = 1 << 1
FAULT_PRESSURE = 1 << 2
FAULT_MIC = 1 << 3
FAULT_CAMERA = 1 << 4
FAULT_ENVIRONMENT = 1 << 5

THERMAL_POINT_COUNT = 80 * 64
THERMAL_FRAME_BYTES = 10248
BED_TEMPERATURE_CHANNELS = 5
BED_TEMPERATURE_MASK = (1 << BED_TEMPERATURE_CHANNELS) - 1
ENVIRONMENT_STALE_MS = 15_000
BME688_STALE_MS = 15_000
MATRIX_STALE_MS = 2_000

ENVIRONMENT_STATES = {
    0: "未初始化",
    1: "正在启动",
    2: "等待首帧",
    3: "采样正常",
    4: "未找到传感器",
    5: "CRC 错误",
    6: "I²C 错误",
    7: "采样超时",
    8: "正在恢复",
}

BME688_STATES = {
    0: "未初始化",
    1: "正在启动",
    2: "等待测量",
    3: "采样正常",
    4: "未找到传感器",
    5: "数据/驱动错误",
    6: "I²C 错误",
    7: "采样超时",
    8: "正在恢复",
}

STATUS_RANK = {"ok": 0, "warning": 1, "offline": 2, "fault": 3}

CAMERA_WRITER_STATES = {
    0: "空闲",
    1: "SD 未就绪",
    2: "等待帧",
    3: "创建目录",
    4: "打开失败",
    5: "文件已打开",
    6: "写入数据",
    7: "写入失败",
    8: "文件已保存",
    9: "检查文件名",
    10: "打开临时文件",
    11: "关闭文件",
    12: "提交文件",
    13: "清理临时文件",
}


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _item(
    item_id: str,
    name: str,
    category: str,
    status: str,
    summary: str,
    detail: str,
    *,
    channels: list[dict[str, Any]] | None = None,
    metrics: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "id": item_id,
        "name": name,
        "category": category,
        "status": status,
        "summary": summary,
        "detail": detail,
        "channels": channels or [],
        "metrics": metrics or {},
    }


def _channel(channel_id: str, name: str, status: str, detail: str) -> dict[str, str]:
    return {"id": channel_id, "name": name, "status": status, "detail": detail}


def _matrix_delivery_status(matrix_status: dict[str, Any], kind: str) -> dict[str, Any] | None:
    age_key = f"{kind}_age_ms"
    frames_key = f"{kind}_received_frames"
    fps_key = f"{kind}_fps"
    if not any(key in matrix_status for key in ("matrix_connected", age_key, frames_key, fps_key)):
        return None

    connected = bool(matrix_status.get("matrix_connected"))
    received_frames = _as_int(matrix_status.get(frames_key))
    age_value = matrix_status.get(age_key)
    age_ms = _as_int(age_value) if age_value is not None else None
    try:
        fps = float(matrix_status.get(fps_key) or 0.0)
    except (TypeError, ValueError):
        fps = 0.0

    if not connected:
        status = "offline"
        detail = "矩阵 WSS 未连接"
    elif received_frames <= 0:
        status = "warning"
        detail = "矩阵 WSS 已连接，但本次连接尚未收到完整帧"
    elif age_ms is None:
        status = "warning"
        detail = "已收到矩阵，但缺少帧龄"
    elif age_ms > MATRIX_STALE_MS:
        status = "warning"
        detail = f"最后一帧已过去 {age_ms} ms"
    else:
        status = "ok"
        detail = f"{fps:.1f} FPS · 帧龄 {age_ms} ms · 已收 {received_frames} 帧"

    return {
        "status": status,
        "detail": detail,
        "connected": connected,
        "received_frames": received_frames,
        "age_ms": age_ms,
        "fps": fps,
    }


def _thermal_diagnostic(
    thermal: dict[str, Any],
    valid_mask: int,
    fault_mask: int,
    matrix_status: dict[str, Any],
) -> dict[str, Any]:
    if not thermal:
        return _item(
            "thermal",
            "红外热成像",
            "sensor",
            "offline",
            "没有状态数据",
            "尚未收到热成像遥测。",
            channels=[_channel("sci1", "SCI1 UART", "offline", "等待 P706/P707 数据")],
        )

    state = _as_int(thermal.get("s"), -1)
    received = _as_int(thermal.get("rx"))
    valid_points = _as_int(thermal.get("valid"))
    crc_rx = thermal.get("crc_rx")
    crc_calc = thermal.get("crc_calc")
    crc_ok = crc_rx is not None and crc_calc is not None and _as_int(crc_rx) == _as_int(crc_calc)
    frame_quality = _as_int(thermal.get("frame_quality"), 0)
    contract_ok = (
        state == 3
        and received == THERMAL_FRAME_BYTES
        and valid_points == THERMAL_POINT_COUNT
        and crc_ok
        and frame_quality == 0
    )
    fusion_ok = bool(valid_mask & SOURCE_THERMAL)
    fusion_fault = bool(fault_mask & FAULT_THERMAL)

    if frame_quality == 1 and not fusion_fault:
        status = "warning"
        summary = "热成像校准/过渡帧已过滤"
        detail = "当前矩阵未进入显示、训练或安全融合，继续沿用上一完整帧。"
    elif contract_ok and fusion_ok and not fusion_fault:
        status = "ok"
        summary = f"{valid_points}/{THERMAL_POINT_COUNT} 像素有效"
        detail = "完整帧、CRC 和融合层有效性均正常。"
    elif fusion_ok and not fusion_fault:
        status = "warning"
        summary = "当前帧异常，沿用上一完整帧"
        detail = "热成像链路正在自动重试；连续超时后才会升级为故障。"
    elif state == 2 and not fusion_fault:
        status = "warning"
        summary = "等待完整帧"
        detail = "串口已启动，尚未形成可用的 80x64 温度帧。"
    else:
        status = "fault"
        problems: list[str] = []
        if state != 3:
            problems.append(f"状态码 {state}")
        if received != THERMAL_FRAME_BYTES:
            problems.append(f"帧长 {received}/{THERMAL_FRAME_BYTES}")
        if not crc_ok:
            problems.append("CRC 不一致")
        if valid_points != THERMAL_POINT_COUNT:
            problems.append(f"有效像素 {valid_points}/{THERMAL_POINT_COUNT}")
        if fusion_fault:
            problems.append("融合层判定故障")
        summary = problems[0] if problems else "数据不可用"
        detail = "；".join(problems) or "热成像数据未通过应用层完整性检查。"

    channel_status = "ok" if contract_ok else status
    channels = [
        _channel(
            "sci1",
            "SCI1 UART / 80x64 阵列",
            channel_status,
            f"接收 {received} B · CRC {'一致' if crc_ok else '异常'} · 有效 {valid_points}",
        )
    ]
    delivery = _matrix_delivery_status(matrix_status, "thermal")
    if delivery is not None:
        channels.append(
            _channel("matrix_upload", "热成像矩阵上传", delivery["status"], delivery["detail"])
        )
        if status == "ok" and delivery["status"] != "ok":
            status = "warning"
            summary = "热成像本地正常，矩阵上传中断"
            detail = f"本地完整帧、CRC 和有效像素正常；{delivery['detail']}。"

    return _item(
        "thermal",
        "红外热成像",
        "sensor",
        status,
        summary,
        detail,
        channels=channels,
        metrics={
            "state": state,
            "received_bytes": received,
            "valid_points": valid_points,
            "expected_points": THERMAL_POINT_COUNT,
            "crc_ok": crc_ok,
            "fusion_valid": fusion_ok,
            "frame_quality": frame_quality,
            "absolute_min_cdeg": _as_int(thermal.get("min_cd")),
            "absolute_max_cdeg": _as_int(thermal.get("max_cd")),
            "p01_cdeg": _as_int(thermal.get("p01_cd")),
            "p99_cdeg": _as_int(thermal.get("p99_cd")),
            "mean_cdeg": _as_int(thermal.get("mean_cd")),
            "transient_drop_count": _as_int(thermal.get("transient_drop_count")),
            "generation_retry_count": _as_int(thermal.get("generation_retry_count")),
            "last_good_sequence": _as_int(thermal.get("last_good_sequence")),
            "mean_abs_diff_cdeg": _as_int(thermal.get("mean_abs_diff_cd")),
            "changed_pixels": _as_int(thermal.get("changed_pixels")),
            "matrix_connected": delivery["connected"] if delivery else None,
            "matrix_received_frames": delivery["received_frames"] if delivery else None,
            "matrix_age_ms": delivery["age_ms"] if delivery else None,
            "matrix_fps": delivery["fps"] if delivery else None,
        },
    )


def _temperature_diagnostic(temperature: dict[str, Any], valid_mask: int, fault_mask: int) -> dict[str, Any]:
    if not temperature:
        return _item(
            "bed_temperature",
            "床体温度",
            "sensor",
            "offline",
            "没有状态数据",
            "尚未收到 TMP117 阵列遥测。",
            channels=[
                _channel(f"ch{i}", f"TMP117 通道 {i}", "offline", "等待扫描")
                for i in range(BED_TEMPERATURE_CHANNELS)
            ],
        )

    state = _as_int(temperature.get("s"), -1)
    connected = _as_int(temperature.get("ok")) & BED_TEMPERATURE_MASK
    errors = _as_int(temperature.get("err")) & BED_TEMPERATURE_MASK
    temperatures = temperature.get("t_mc") if isinstance(temperature.get("t_mc"), list) else []
    addresses = temperature.get("addr") if isinstance(temperature.get("addr"), list) else []
    channels: list[dict[str, Any]] = []
    healthy = 0

    for index in range(BED_TEMPERATURE_CHANNELS):
        bit = 1 << index
        milli_c = _as_int(temperatures[index]) if index < len(temperatures) else None
        address = _as_int(addresses[index]) if index < len(addresses) else None
        address_text = f"0x{address:02X}" if address is not None else "地址未知"
        if errors & bit:
            channel_status = "fault"
            channel_detail = f"扫描错误 · {address_text}"
        elif not connected & bit:
            channel_status = "fault"
            channel_detail = "未连接或未应答"
        elif milli_c is None or not -55000 <= milli_c <= 150000:
            channel_status = "fault"
            channel_detail = f"温度值异常 · {milli_c if milli_c is not None else '-'} m°C"
        else:
            channel_status = "ok"
            healthy += 1
            channel_detail = f"{milli_c / 1000:.3f} °C · {address_text}"
        channels.append(_channel(f"ch{index}", f"TMP117 通道 {index}", channel_status, channel_detail))

    fusion_ok = bool(valid_mask & SOURCE_TEMPERATURE)
    fusion_fault = bool(fault_mask & FAULT_TEMPERATURE)
    all_healthy = healthy == BED_TEMPERATURE_CHANNELS and errors == 0
    if all_healthy and state in (2, 3) and fusion_ok and not fusion_fault:
        status = "ok"
        detail = "5 路 TMP117 均应答，通道位图和融合层状态正常。"
    elif fusion_ok and not fusion_fault:
        status = "warning"
        detail_parts = [f"本轮正常 {healthy}/{BED_TEMPERATURE_CHANNELS}"]
        if errors:
            detail_parts.append(f"错误位图 0x{errors:02X}")
        detail_parts.append("融合层沿用上一轮完整扫描，持续异常后才升级为故障")
        detail = "；".join(detail_parts)
    else:
        status = "fault"
        detail_parts = [f"正常 {healthy}/{BED_TEMPERATURE_CHANNELS}"]
        if errors:
            detail_parts.append(f"错误位图 0x{errors:02X}")
        if not fusion_ok or fusion_fault:
            detail_parts.append("融合层未接受温度阵列")
        if state not in (2, 3):
            detail_parts.append(f"状态码 {state}")
        detail = "；".join(detail_parts)

    return _item(
        "bed_temperature",
        "床体温度",
        "sensor",
        status,
        f"{healthy}/{BED_TEMPERATURE_CHANNELS} 通道正常",
        detail,
        channels=channels,
        metrics={
            "state": state,
            "connected_mask": connected,
            "error_mask": errors,
            "healthy_channels": healthy,
            "fusion_valid": fusion_ok,
        },
    )


def _tick_age_ms(runtime_ms: int | None, last_good_ms: int | None) -> int | None:
    if runtime_ms is None or last_good_ms is None:
        return None
    if not 0 <= runtime_ms <= 0xFFFFFFFF or not 0 <= last_good_ms <= 0xFFFFFFFF:
        return None
    if runtime_ms >= last_good_ms:
        return runtime_ms - last_good_ms
    if last_good_ms - runtime_ms > 0x80000000:
        return (runtime_ms - last_good_ms) & 0xFFFFFFFF
    # A small backwards jump is more likely a device restart or a different tick
    # domain than a 32-bit wrap, so it must not create a false stale alarm.
    return None


def _scd41_environment_diagnostic(
    environment: dict[str, Any],
    valid_mask: int,
    fault_mask: int,
    runtime_ms: int | None,
) -> dict[str, Any]:
    if not environment:
        fusion_fault = bool(fault_mask & FAULT_ENVIRONMENT)
        return _item(
            "environment",
            "环境 CO₂ / 温湿度",
            "sensor",
            "fault" if fusion_fault else "offline",
            "融合层报告环境传感器故障" if fusion_fault else "没有状态数据",
            (
                "环境故障位已置位，但网关遥测中没有 sensors.environment。"
                if fusion_fault
                else "尚未收到 SCD41 环境传感器遥测。"
            ),
            channels=[
                _channel(
                    "i2c",
                    "SCD41 / I²C",
                    "fault" if fusion_fault else "offline",
                    "融合故障位已置位" if fusion_fault else "等待传感器数据",
                )
            ],
        )

    state = _as_int(environment.get("s"), -1)
    state_name = ENVIRONMENT_STATES.get(state, f"未知状态 {state}")
    sequence = _as_int(environment.get("seq"))
    ready = bool(_as_int(environment.get("ready")))
    co2_ppm = _as_int(environment.get("co2"), -1)
    temperature_milli_c = _as_int(environment.get("t_mc"), -100_000)
    humidity_milli_percent = _as_int(environment.get("rh_mp"), -1)
    crc_errors = _as_int(environment.get("crc_err"))
    i2c_errors = _as_int(environment.get("i2c_err"))
    timeout_count = _as_int(environment.get("to"))
    recovery_count = _as_int(environment.get("rec"))
    last_fsp_error = _as_int(environment.get("fsp"))
    last_good_ms = (
        _as_int(environment.get("last"))
        if environment.get("last") is not None
        else None
    )
    age_ms = _tick_age_ms(runtime_ms, last_good_ms)
    stale = age_ms is not None and age_ms > ENVIRONMENT_STALE_MS
    # Validate the encoded SCD4x output domain, not an application alarm
    # threshold. SCD41 accuracy is specified over a narrower CO2 range, but
    # the device can legitimately report values up to 40,000 ppm.
    values_ok = (
        0 <= co2_ppm <= 40_000
        and -45_000 <= temperature_milli_c <= 130_000
        and 0 <= humidity_milli_percent <= 100_000
    )
    fusion_ok = bool(valid_mask & SOURCE_ENVIRONMENT)
    fusion_fault = bool(fault_mask & FAULT_ENVIRONMENT)
    driver_fault = state in (4, 5, 6, 7)

    if fusion_fault or driver_fault or stale or (state == 3 and not values_ok):
        status = "fault"
        problems: list[str] = []
        if driver_fault:
            problems.append(state_name)
        if stale:
            problems.append(f"最后有效样本已过去 {age_ms} ms")
        if state == 3 and not values_ok:
            problems.append("测量值超出 SCD41 有效范围")
        if fusion_fault:
            problems.append("融合层环境传感器故障位已置位")
        summary = problems[0] if problems else "环境传感器故障"
        detail = " · ".join(problems)
    elif state == 0:
        status = "offline"
        summary = "等待 SCD41 初始化"
        detail = "环境传感器邮箱存在，但驱动尚未启动。"
    elif state == 3 and values_ok and fusion_ok:
        status = "ok"
        summary = f"CO₂ {co2_ppm} ppm"
        detail = (
            f"{temperature_milli_c / 1000:.3f} °C · "
            f"{humidity_milli_percent / 1000:.3f} %RH · 融合数据有效"
        )
    elif state in (1, 2, 8):
        status = "warning"
        summary = state_name
        detail = "驱动处于暂态；持续超时或融合故障位出现后才判定为故障。"
    else:
        status = "warning"
        summary = state_name
        detail_parts = []
        if not values_ok:
            detail_parts.append("尚无完整有效测量")
        if not fusion_ok:
            detail_parts.append("融合层尚未接纳环境样本")
        if not ready:
            detail_parts.append("数据就绪标志未置位")
        detail = " · ".join(detail_parts) or "等待下一次状态更新"

    channel_status = status
    return _item(
        "environment",
        "环境 CO₂ / 温湿度",
        "sensor",
        status,
        summary,
        detail,
        channels=[
            _channel(
                "i2c",
                "SCD41 / I²C",
                channel_status,
                f"{state_name} · FSP {last_fsp_error} · "
                f"CRC/I²C/超时/恢复 {crc_errors}/{i2c_errors}/{timeout_count}/{recovery_count}",
            )
        ],
        metrics={
            "state": state,
            "sequence": sequence,
            "ready": ready,
            "co2_ppm": co2_ppm,
            "temperature_milli_c": temperature_milli_c,
            "humidity_milli_percent": humidity_milli_percent,
            "last_good_tick": last_good_ms,
            "sample_age_ms": age_ms,
            "stale": stale,
            "crc_error_count": crc_errors,
            "i2c_error_count": i2c_errors,
            "timeout_count": timeout_count,
            "recovery_count": recovery_count,
            "last_fsp_err": last_fsp_error,
            "fusion_valid": fusion_ok,
            "fusion_fault": fusion_fault,
        },
    )


def _bme688_diagnostic(
    bme688: dict[str, Any],
    runtime_ms: int | None,
) -> dict[str, Any]:
    state = _as_int(bme688.get("s"), -1)
    state_name = BME688_STATES.get(state, f"未知状态 {state}")
    address = _as_int(bme688.get("addr"), -1)
    temperature_milli_c = _as_int(bme688.get("t_mc"), -(2**31))
    humidity_milli_percent = _as_int(bme688.get("rh_mp"), -1)
    pressure_pa = _as_int(bme688.get("p_pa"), -1)
    gas_resistance_ohm = _as_int(bme688.get("gas_ohm"), -1)
    flags = _as_int(bme688.get("flags"))
    error_count = _as_int(bme688.get("err"))
    last_good_ms = (
        _as_int(bme688.get("last"))
        if bme688.get("last") is not None
        else None
    )
    age_ms = _tick_age_ms(runtime_ms, last_good_ms)
    stale = age_ms is not None and age_ms > BME688_STALE_MS
    new_data = bool(flags & 0x01)
    gas_valid = bool(flags & 0x02)
    heater_stable = bool(flags & 0x04)
    address_ok = address in (0x76, 0x77)
    climate_values_ok = (
        -40_000 <= temperature_milli_c <= 85_000
        and 0 <= humidity_milli_percent <= 100_000
        and 30_000 <= pressure_pa <= 110_000
    )
    gas_value_ok = 0 < gas_resistance_ohm <= 0xFFFFFFFF
    driver_fault = state in (4, 5, 6, 7)
    sample_contract_ok = (
        state == 3
        and address_ok
        and climate_values_ok
        and last_good_ms is not None
        and not stale
    )
    gas_measurement_valid = sample_contract_ok and gas_valid and heater_stable and gas_value_ok

    if (
        driver_fault
        or stale
        or (
            state == 3
            and (not address_ok or not climate_values_ok or (gas_valid and not gas_value_ok))
        )
    ):
        status = "fault"
        problems: list[str] = []
        if driver_fault:
            problems.append(state_name)
        if not address_ok:
            problems.append(f"I²C 地址 {address:#04x} 非 0x76/0x77")
        if state == 3 and not climate_values_ok:
            problems.append("温湿度或气压超出 BME688 物理编码范围")
        if state == 3 and gas_valid and not gas_value_ok:
            problems.append("gas_valid 已置位但原始气阻无效")
        if stale:
            problems.append(f"最后有效样本已过去 {age_ms} ms")
        summary = problems[0] if problems else "BME688 故障"
        detail = " · ".join(problems)
    elif state == 0:
        status = "offline"
        summary = "等待 BME688 初始化"
        detail = "BME688 状态对象存在，但驱动尚未启动。"
    elif state in (1, 2, 8):
        status = "warning"
        summary = state_name
        detail = "驱动处于暂态；持续超时或进入故障状态后才判定为故障。"
    elif sample_contract_ok and gas_measurement_valid:
        status = "ok"
        summary = f"气压 {pressure_pa / 100:.2f} hPa"
        detail = (
            f"{temperature_milli_c / 1000:.3f} °C · "
            f"{humidity_milli_percent / 1000:.3f} %RH · "
            f"原始气阻 {gas_resistance_ohm / 1000:.3f} kΩ · "
            "气体加热器稳定"
        )
    else:
        status = "warning"
        if state == 3 and sample_contract_ok and not heater_stable:
            summary = "气体加热器预热中"
        elif state == 3 and sample_contract_ok and not gas_valid:
            summary = "气阻样本尚未有效"
        else:
            summary = state_name
        problems = []
        if last_good_ms is None:
            problems.append("缺少最近有效 tick")
        if not gas_valid:
            problems.append("gas_valid 未置位")
        if not heater_stable:
            problems.append("heater_stable 未置位")
        detail = " · ".join(problems) or "等待下一次状态更新"

    return _item(
        "bme688",
        "BME688 环境传感器",
        "sensor",
        status,
        summary,
        detail,
        channels=[
            _channel(
                "bme688_i2c",
                "BME688 / I²C1",
                status,
                f"{state_name} · 地址 "
                f"{address:#04x} · flags 0x{flags:02X} · 累计错误 {error_count}",
            )
        ],
        metrics={
            "state": state,
            "address": address,
            "temperature_milli_c": temperature_milli_c,
            "humidity_milli_percent": humidity_milli_percent,
            "pressure_pa": pressure_pa,
            "gas_resistance_ohm": gas_resistance_ohm,
            "flags": flags,
            "new_data": new_data,
            "gas_valid": gas_valid,
            "heater_stable": heater_stable,
            "gas_measurement_valid": gas_measurement_valid,
            "climate_values_valid": climate_values_ok,
            "gas_value_valid": gas_value_ok,
            "last_good_tick": last_good_ms,
            "sample_age_ms": age_ms,
            "stale": stale,
            "error_count": error_count,
            "gas_semantics": "raw_resistance_only_no_bsec_voc_or_iaq",
        },
    )


def _environment_diagnostic(
    environment: dict[str, Any],
    valid_mask: int,
    fault_mask: int,
    runtime_ms: int | None,
) -> dict[str, Any]:
    """Combine the legacy SCD41 channel with the optional BME688 channel."""

    bme688 = environment.get("bme688") if isinstance(environment.get("bme688"), dict) else {}
    scd41 = {key: value for key, value in environment.items() if key != "bme688"}
    scd41_item = _scd41_environment_diagnostic(scd41, valid_mask, fault_mask, runtime_ms)
    if not bme688:
        return scd41_item

    bme688_item = _bme688_diagnostic(bme688, runtime_ms)
    status = max(
        (scd41_item["status"], bme688_item["status"]),
        key=lambda candidate: STATUS_RANK[candidate],
    )
    if STATUS_RANK[bme688_item["status"]] > STATUS_RANK[scd41_item["status"]]:
        summary = f"BME688：{bme688_item['summary']}"
    elif scd41_item["status"] != "ok":
        summary = scd41_item["summary"]
    elif bme688_item["status"] == "ok":
        summary = f"{scd41_item['summary']} · {bme688_item['summary']}"
    else:
        summary = bme688_item["summary"]
    return _item(
        "environment",
        "环境 CO₂ / 温湿度 / 气压 / 原始气阻",
        "sensor",
        status,
        summary,
        f"SCD41：{scd41_item['detail']} · BME688：{bme688_item['detail']} · "
        "气阻为原始电阻值；未运行 BSEC，不能解释为 VOC 或 IAQ",
        channels=[*scd41_item["channels"], *bme688_item["channels"]],
        metrics={
            **scd41_item["metrics"],
            "bme688": bme688_item["metrics"],
        },
    )


def _pressure_diagnostic(
    pressure: dict[str, Any],
    matrix: dict[str, Any] | None,
    valid_mask: int,
    fault_mask: int,
    matrix_status: dict[str, Any],
) -> dict[str, Any]:
    if not pressure:
        return _item(
            "pressure",
            "压力矩阵",
            "sensor",
            "offline",
            "没有状态数据",
            "尚未收到 M1616 压力矩阵遥测。",
            channels=[_channel("uart", "M1616 UART", "offline", "等待数据")],
        )

    state = _as_int(pressure.get("s"), -1)
    packet_mask = _as_int(pressure.get("ok")) & 0x07
    last_error = _as_int(pressure.get("fsp"))
    fusion_ok = bool(valid_mask & SOURCE_PRESSURE)
    fusion_fault = bool(fault_mask & FAULT_PRESSURE)
    matrix_ok = False
    matrix_detail = "服务器尚未收到独立 16x16 矩阵帧"
    if matrix:
        rows = _as_int(matrix.get("rows"))
        cols = _as_int(matrix.get("cols"))
        values = matrix.get("values")
        matrix_ok = rows == 16 and cols == 16 and isinstance(values, list) and len(values) == 256
        matrix_detail = f"{rows}x{cols} · {len(values) if isinstance(values, list) else 0} 点"

    if fusion_fault:
        status = "fault"
        summary = "压力数据流故障"
        detail = f"状态码 {state} · FSP {last_error} · 融合故障位 {int(fusion_fault)}"
    elif state in (1, 4) and fusion_ok:
        status = "warning"
        summary = "当前压力帧异常，正在重试"
        detail = f"状态码 {state} · FSP {last_error} · 沿用上一完整帧"
    elif state in (1, 4):
        status = "fault"
        summary = "压力数据流故障"
        detail = f"状态码 {state} · FSP {last_error}"
    elif fusion_ok and state in (2, 3):
        status = "ok"
        summary = "M1616 数据流正常"
        detail = "当前可能正在拼装下一帧；三包完整后组包位图会清零。"
    else:
        status = "warning"
        summary = "等待完整压力帧"
        detail = f"状态码 {state} · 当前组包位图 0x{packet_mask:02X}"

    uart_status = status
    frame_status = "ok" if fusion_ok or matrix_ok else status
    delivery = _matrix_delivery_status(matrix_status, "pressure")
    if delivery is not None:
        frame_status = delivery["status"]
        matrix_detail = delivery["detail"]
        if status == "ok" and delivery["status"] != "ok":
            status = "warning"
            summary = "M1616 本地正常，矩阵上传中断"
            detail = f"RA8 已形成有效压力帧；{delivery['detail']}。"

    return _item(
        "pressure",
        "压力矩阵",
        "sensor",
        status,
        summary,
        detail + " M1616 协议不提供物理行列的独立故障位。",
        channels=[
            _channel("uart", "M1616 UART", uart_status, f"状态 {state} · FSP {last_error}"),
            _channel("matrix", "16x16 矩阵帧", frame_status, matrix_detail),
        ],
        metrics={
            "state": state,
            "packet_mask": packet_mask,
            "last_fsp_err": last_error,
            "fusion_valid": fusion_ok,
            "matrix_shape_ok": matrix_ok,
            "matrix_connected": delivery["connected"] if delivery else None,
            "matrix_received_frames": delivery["received_frames"] if delivery else None,
            "matrix_age_ms": delivery["age_ms"] if delivery else None,
            "matrix_fps": delivery["fps"] if delivery else None,
        },
    )


def _microphone_diagnostic(microphone: dict[str, Any], valid_mask: int, fault_mask: int) -> dict[str, Any]:
    if not microphone:
        return _item(
            "microphone",
            "RA8 麦克风",
            "sensor",
            "offline",
            "没有状态数据",
            "尚未收到麦克风遥测。",
            channels=[_channel("ssie1", "SSIE1 I2S", "offline", "等待采样")],
        )

    state = _as_int(microphone.get("s"), -1)
    reads = _as_int(microphone.get("read"))
    callbacks = _as_int(microphone.get("cb"))
    errors = _as_int(microphone.get("err"))
    last_fsp = _as_int(microphone.get("fsp"))
    fusion_ok = bool(valid_mask & SOURCE_MIC)
    fusion_fault = bool(fault_mask & FAULT_MIC)

    if fusion_fault:
        status = "fault"
        summary = "I2S 采样故障"
        detail = f"状态码 {state} · FSP {last_fsp} · 错误累计 {errors}"
    elif state in (1, 5) and fusion_ok:
        status = "warning"
        summary = "当前音频块异常，正在重试"
        detail = f"状态码 {state} · FSP {last_fsp} · 沿用上一有效音频窗口"
    elif state in (1, 5):
        status = "fault"
        summary = "I2S 采样故障"
        detail = f"状态码 {state} · FSP {last_fsp} · 错误累计 {errors}"
    elif state in (2, 3) and reads > 0 and fusion_ok:
        status = "ok"
        summary = "正在采样" if state == 2 else "正在采样并录音"
        detail = f"读取 {reads} 块 · 回调 {callbacks} 次 · 错误累计 {errors}"
    else:
        status = "warning"
        summary = "麦克风未持续采样"
        detail = f"状态码 {state} · 读取 {reads} 块 · 融合有效 {int(fusion_ok)}"

    return _item(
        "microphone",
        "RA8 麦克风",
        "sensor",
        status,
        summary,
        detail,
        channels=[
            _channel(
                "ssie1",
                "SSIE1 I2S",
                status,
                f"{_as_int(microphone.get('rate'))} Hz · {_as_int(microphone.get('bits'))} bit",
            )
        ],
        metrics={
            "state": state,
            "read_count": reads,
            "callback_count": callbacks,
            "error_count": errors,
            "fusion_valid": fusion_ok,
        },
    )


def _camera_diagnostic(
    camera: dict[str, Any],
    sd_ready: bool,
    valid_mask: int,
    fault_mask: int,
    vision_age_ms: int,
) -> dict[str, Any]:
    if not camera:
        return _item(
            "camera",
            "串口摄像头",
            "sensor",
            "offline",
            "没有状态数据",
            "尚未收到串口摄像头遥测。",
            channels=[_channel("sci3", "SCI3 UART", "offline", "等待 P905/P310 探测")],
        )

    state = _as_int(camera.get("s"), -1)
    probe_attempted = bool(_as_int(camera.get("probe")))
    probe_ok = bool(_as_int(camera.get("probe_ok")))
    version = str(camera.get("ver") or "版本未知")
    last_fsp = _as_int(camera.get("fsp"))
    writer_state = _as_int(camera.get("wst"))
    writer_state_name = (
        "等待图像"
        if writer_state == 1 and sd_ready
        else CAMERA_WRITER_STATES.get(writer_state, f"未知状态 {writer_state}")
    )
    writer_error = _as_int(camera.get("werr"))
    writer_file = str(camera.get("file") or "未生成文件名")
    failed_stage = _as_int(camera.get("fail_stage"))
    timeout_count = _as_int(camera.get("to"))
    writer_ack_timeout = failed_stage == 4 and last_fsp == 20
    writer_waiting_for_sd = writer_state == 1 and not sd_ready
    fusion_ok = bool(valid_mask & SOURCE_CAMERA)
    fusion_fault = bool(fault_mask & FAULT_CAMERA)
    serial_status = "ok" if probe_ok and state in (4, 5) else "fault"

    if fusion_fault:
        status = "fault"
        summary = "摄像头持续无有效帧"
        detail = f"有效帧年龄 {vision_age_ms} ms · 状态码 {state} · FSP {last_fsp}"
    elif (state in (2, 6) or (probe_attempted and not probe_ok)) and fusion_ok:
        status = "warning"
        summary = "本次抓图失败，正在自动重探测"
        detail = f"沿用上一有效帧 · 年龄 {vision_age_ms} ms · 状态码 {state} · FSP {last_fsp}"
        serial_status = "warning"
    elif state in (2, 6) or (probe_attempted and not probe_ok):
        status = "fault"
        summary = "摄像头通信故障"
        detail = f"状态码 {state} · 探测 {int(probe_ok)} · FSP {last_fsp}"
    elif writer_error:
        status = "fault"
        summary = "SD 图像保存故障"
        detail = f"{writer_state_name} · FAT errno {writer_error} · {writer_file}"
    elif writer_ack_timeout:
        status = "fault"
        summary = "JPEG 写入线程无响应"
        detail = (
            f"分块发布阶段超时 · 写入器停在 {writer_state_name} · "
            f"FSP {last_fsp} · 超时累计 {timeout_count} · {writer_file}"
        )
    elif state in (4, 5) and probe_ok:
        status = "ok"
        summary = "已就绪" if state == 4 else "正在抓图"
        detail = f"{version} · 探测成功"
    else:
        status = "warning"
        summary = "正在初始化或探测"
        detail = f"状态码 {state} · 探测 {int(probe_ok)}"

    return _item(
        "camera",
        "串口摄像头",
        "sensor",
        status,
        summary,
        detail,
        channels=[
            _channel("sci3", "SCI3 UART / P905 RX + P310 TX", serial_status, version),
            _channel(
                "sd_writer",
                "JPEG / SD 写入",
                "fault" if writer_error or writer_ack_timeout else ("warning" if writer_waiting_for_sd else "ok"),
                f"{writer_state_name} · errno {writer_error} · 失败阶段 {failed_stage} · {writer_file}",
            ),
        ],
        metrics={
            "state": state,
            "probe_ok": probe_ok,
            "last_fsp_err": last_fsp,
            "uart_error_count": _as_int(camera.get("uart_err")),
            "writer_state": writer_state,
            "writer_error": writer_error,
            "failed_stage": failed_stage,
            "timeout_count": timeout_count,
            "writer_ack_timeout": writer_ack_timeout,
            "writer_waiting_for_sd": writer_waiting_for_sd,
            "saved_count": _as_int(camera.get("saved")),
            "fusion_valid": fusion_ok,
            "fusion_fault": fusion_fault,
            "vision_age_ms": vision_age_ms,
        },
    )


def _link_diagnostic(telemetry: dict[str, Any], ra8_online: bool) -> dict[str, Any]:
    health = telemetry.get("health") if isinstance(telemetry.get("health"), dict) else {}
    matrix_status = health.get("gateway") if isinstance(health.get("gateway"), dict) else {}
    link = health.get("ra8_link") if isinstance(health.get("ra8_link"), dict) else {}
    gateway = health.get("gateway") if isinstance(health.get("gateway"), dict) else {}
    age_ms = _as_int(health.get("ra8_status_age_ms"), -1)
    uart_rx_bytes = _as_int(health.get("uart_rx_bytes", gateway.get("uart_rx_bytes")))
    valid_frames = _as_int(health.get("uart_valid_frames"))
    status_frames = _as_int(health.get("uart_status_frames"))
    ready_tx_count = _as_int(health.get("ready_tx_count", gateway.get("ready_tx_count")))
    ready_tx_errors = _as_int(health.get("ready_tx_errors", gateway.get("ready_tx_errors")))
    uart_active_baud = _as_int(gateway.get("uart_active_baud"))
    uart_baud_locked = bool(gateway.get("uart_baud_locked"))
    uart_baud_switches = _as_int(gateway.get("uart_baud_switches"))
    uart_magic_aa = _as_int(gateway.get("uart_magic_aa"))
    uart_sync_headers = _as_int(gateway.get("uart_sync_headers"))
    uart_sample_hex = str(gateway.get("uart_sample_hex") or "")
    ready_seen_count = _as_int(link.get("ready_n"))
    ready_seen = bool(_as_int(link.get("ready")))
    ready_age_ms = _as_int(link.get("ready_age"), -1)
    status_attempts = _as_int(link.get("st_try"))
    status_successes = _as_int(link.get("st_ok"))
    status_length = _as_int(link.get("st_len"))
    status_detail = _as_int(link.get("st_detail"), -1)
    status_too_long = _as_int(link.get("st_long"))
    status_uart_errors = _as_int(link.get("st_uart"))

    current_payload_degraded = status_detail == 3 or status_length > 4096
    reverse_link_failed = ra8_online and ready_tx_count >= 3 and not ready_seen

    if ready_tx_errors > 0 and ready_tx_count == 0:
        status = "fault"
        summary = "网关无法发送握手帧"
    elif not ra8_online and uart_rx_bytes == 0:
        status = "fault"
        summary = "网关未收到 RA8 UART 字节"
    elif not ra8_online and valid_frames == 0:
        status = "fault"
        summary = "收到 UART 字节但没有有效协议帧"
    elif not ra8_online and status_frames == 0:
        status = "fault"
        summary = "协议有效但没有状态帧"
    elif ra8_online and current_payload_degraded:
        status = "fault"
        summary = "RA8 当前状态包已退化"
    elif reverse_link_failed:
        status = "fault"
        summary = "反向命令链路故障"
    elif ra8_online and ready_tx_count > 0 and not ready_seen:
        status = "warning"
        summary = "正向在线，正在等待反向握手"
    elif ra8_online:
        status = "ok"
        summary = "双向在线" if ready_seen else "正向在线"
    else:
        status = "fault"
        summary = "RA8 状态已超时"

    detail = (
        f"年龄 {age_ms if age_ms >= 0 else '-'} ms · 网关 RX {uart_rx_bytes} B · "
        f"有效/状态帧 {valid_frames}/{status_frames} · ready TX {ready_tx_count}/{ready_tx_errors} · "
        f"RA8 ready {int(ready_seen)}({ready_seen_count})/{ready_age_ms if ready_age_ms >= 0 else '-'} ms · "
        f"status {status_successes}/{status_attempts} · "
        f"长度 {status_length} · 详情级别 {status_detail} · 历史过长 {status_too_long} · "
        f"TX 错误 {status_uart_errors} · "
        f"波特率 {uart_active_baud or '-'}({('锁定' if uart_baud_locked else '扫描')}) · "
        f"同步 {uart_sync_headers}/{uart_magic_aa} · 样本 {uart_sample_hex or '-'}"
        f"{' · 控制命令不可用' if reverse_link_failed else ''}"
    )
    return _item(
        "ra8_link",
        "RA8 通信链路",
        "system",
        status,
        summary,
        detail,
        channels=[
            _channel(
                "s3_to_ra8",
                "P4 GPIO7 TX → RA8 P602 RX",
                "fault" if (ready_tx_errors > 0 and ready_tx_count == 0) or reverse_link_failed else ("ok" if ready_seen else "warning"),
                f"ready 已发送 {ready_tx_count} · 错误 {ready_tx_errors} · RA8 收到 {ready_seen_count}",
            ),
            _channel(
                "ra8_to_s3",
                "RA8 P603 TX → P4 GPIO8 RX",
                "ok" if ra8_online else "fault",
                f"P4 收到 {uart_rx_bytes} B · 有效帧 {valid_frames} · 状态帧 {status_frames} · "
                f"{uart_active_baud or '-'} baud · 同步头 {uart_sync_headers}",
            ),
        ],
        metrics={
            "status_age_ms": age_ms,
            "uart_rx_bytes": uart_rx_bytes,
            "valid_frames": valid_frames,
            "status_frames": status_frames,
            "status_detail": status_detail,
            "ready_tx_count": ready_tx_count,
            "ready_tx_errors": ready_tx_errors,
            "uart_active_baud": uart_active_baud,
            "uart_baud_locked": uart_baud_locked,
            "uart_baud_switches": uart_baud_switches,
            "uart_magic_aa": uart_magic_aa,
            "uart_sync_headers": uart_sync_headers,
            "uart_sample_hex": uart_sample_hex,
            "ready_seen": ready_seen,
            "ready_seen_count": ready_seen_count,
            "ready_age_ms": ready_age_ms,
            "reverse_command_ready": ready_seen,
            "reverse_link_failed": reverse_link_failed,
            "status_attempts": status_attempts,
            "status_successes": status_successes,
            "status_payload_length": status_length,
            "status_payload_too_long": status_too_long,
            "status_uart_errors": status_uart_errors,
            "rx": _as_int(link.get("rx")),
            "errors": _as_int(link.get("err")),
            "overflow": _as_int(link.get("ovf")),
            "firmware_build": _as_int(link.get("fw")),
            "vision_build": _as_int(link.get("vision_fw")),
            "fusion_rule_version": _as_int(link.get("rules")),
            "cry_model_version": str(link.get("cry_fw") or ""),
            "cry_type_model_version": str(link.get("cry_type_fw") or ""),
            "heap_free": _as_int(link.get("heap")),
            "heap_minimum": _as_int(link.get("heap_min")),
            "stack_high_water": link.get("stk") if isinstance(link.get("stk"), list) else [],
        },
    )


def _sd_diagnostic(microphone: dict[str, Any], runtime_ms: int) -> dict[str, Any]:
    if "sdr" not in microphone:
        return _item(
            "sd_card",
            "CPU1 SD 卡",
            "system",
            "warning",
            "没有诊断字段",
            "当前固件未上报 SD 初始化状态。",
            channels=[_channel("sdhi", "SDHI", "warning", "状态未知")],
        )
    ready_claimed = _as_int(microphone.get("sdr")) == 1
    step = _as_int(microphone.get("sds"))
    fail_step = _as_int(microphone.get("sdfs"))
    fsp_error = _as_int(microphone.get("sdfsp"))
    fat_error = _as_int(microphone.get("sdff"))
    failure_count = _as_int(microphone.get("sdfail"))
    explicit_failure = bool(fail_step or fsp_error or fat_error or failure_count)
    incomplete_ready = ready_claimed and step in (4, 5, 6)
    ready = ready_claimed and not incomplete_ready
    stalled = not ready and runtime_ms >= 15000
    if incomplete_ready:
        status = "fault" if stalled else "warning"
        summary = "挂载流程未完整完成"
        detail = f"ready 标志已置位但仍停在阶段 {step} · 文件线程可能被 FAT 初始化锁阻塞"
    elif ready:
        status = "ok"
        summary = "已挂载"
        detail = f"初始化完成 · 阶段 {step} · 尝试 {_as_int(microphone.get('sdtry'))} 次"
    elif explicit_failure:
        status = "fault"
        summary = "初始化失败"
        detail = f"失败阶段 {fail_step or step} · FSP {fsp_error} · FAT {fat_error} · 失败 {failure_count} 次"
    elif stalled:
        status = "fault"
        summary = "初始化长时间未完成"
        detail = f"停在阶段 {step} · 已运行 {runtime_ms / 1000:.1f} 秒 · 尚无明确 FSP/FAT 错误码"
    else:
        status = "warning"
        summary = "正在初始化"
        detail = f"当前阶段 {step} · 已运行 {runtime_ms / 1000:.1f} 秒"
    return _item(
        "sd_card",
        "CPU1 SD 卡",
        "system",
        status,
        summary,
        detail,
        channels=[_channel("sdhi", "SDHI / FAT", status, detail)],
        metrics={
            "ready": ready,
            "ready_claimed": ready_claimed,
            "incomplete_ready": incomplete_ready,
            "step": step,
            "failure_step": fail_step,
            "runtime_ms": runtime_ms,
            "stalled": stalled,
        },
    )


def build_sensor_diagnostics(
    telemetry: dict[str, Any] | None,
    matrices: dict[str, Any] | None,
    *,
    device_online: bool,
) -> dict[str, Any]:
    telemetry = telemetry if isinstance(telemetry, dict) else {}
    matrices = matrices if isinstance(matrices, dict) else {}
    sensors = telemetry.get("sensors") if isinstance(telemetry.get("sensors"), dict) else {}
    inference = telemetry.get("inference") if isinstance(telemetry.get("inference"), dict) else {}
    health = telemetry.get("health") if isinstance(telemetry.get("health"), dict) else {}
    matrix_status = health.get("gateway") if isinstance(health.get("gateway"), dict) else {}
    valid_mask = _as_int(inference.get("valid"))
    fault_mask = _as_int(inference.get("faults"))
    runtime_ms = (
        _as_int(telemetry.get("ra8_tick"))
        if telemetry.get("ra8_tick") is not None
        else None
    )
    vision_evidence = inference.get("vv") if isinstance(inference.get("vv"), list) else []
    vision_age_ms = _as_int(vision_evidence[1], -1) if len(vision_evidence) > 1 else -1
    ra8_online = bool(health.get("ra8_online")) if "ra8_online" in health else bool(telemetry)

    microphone = sensors.get("microphone") if isinstance(sensors.get("microphone"), dict) else {}
    environment = sensors.get("environment") if isinstance(sensors.get("environment"), dict) else {}
    legacy_bme688 = sensors.get("bme688") if isinstance(sensors.get("bme688"), dict) else {}
    if legacy_bme688 and not isinstance(environment.get("bme688"), dict):
        environment = {**environment, "bme688": legacy_bme688}

    items = [
        _link_diagnostic(telemetry, ra8_online),
        _sd_diagnostic(microphone, _as_int(telemetry.get("ra8_tick"))),
        _thermal_diagnostic(
            sensors.get("thermal") if isinstance(sensors.get("thermal"), dict) else {},
            valid_mask,
            fault_mask,
            matrix_status,
        ),
        _temperature_diagnostic(
            sensors.get("bed_temperature") if isinstance(sensors.get("bed_temperature"), dict) else {},
            valid_mask,
            fault_mask,
        ),
        _environment_diagnostic(
            environment,
            valid_mask,
            fault_mask,
            runtime_ms,
        ),
        _pressure_diagnostic(
            sensors.get("pressure") if isinstance(sensors.get("pressure"), dict) else {},
            matrices.get("pressure") if isinstance(matrices.get("pressure"), dict) else None,
            valid_mask,
            fault_mask,
            matrix_status,
        ),
        _microphone_diagnostic(microphone, valid_mask, fault_mask),
        _camera_diagnostic(
            sensors.get("camera") if isinstance(sensors.get("camera"), dict) else {},
            _as_int(microphone.get("sdr")) == 1 and _as_int(microphone.get("sds")) not in (4, 5, 6),
            valid_mask,
            fault_mask,
            vision_age_ms,
        ),
    ]

    if not device_online:
        for item in items:
            previous = item["summary"]
            item["status"] = "offline"
            item["summary"] = "设备离线"
            item["detail"] = f"无法判断当前状态；最后状态：{previous}。"
            for channel in item["channels"]:
                channel["status"] = "offline"
    elif not ra8_online:
        for item in items:
            if item["id"] == "ra8_link":
                continue
            previous = item["summary"]
            item["status"] = "offline"
            item["summary"] = "RA8 离线"
            item["detail"] = f"无法判断当前状态；最后状态：{previous}。"
            for channel in item["channels"]:
                channel["status"] = "offline"

    summary = {name: sum(1 for item in items if item["status"] == name) for name in STATUS_RANK}
    overall = max((item["status"] for item in items), key=lambda status: STATUS_RANK[status], default="offline")
    sensor_items = [item for item in items if item["category"] == "sensor"]
    sensor_summary = {name: sum(1 for item in sensor_items if item["status"] == name) for name in STATUS_RANK}
    return {
        "overall": overall,
        "summary": summary,
        "sensor_summary": sensor_summary,
        "items": items,
    }
