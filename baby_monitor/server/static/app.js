const telemetryHistory = window.TelemetryHistory;
if (!telemetryHistory) throw new Error("telemetry history helper is unavailable");
const thermalDisplay = window.ThermalDisplay;
if (!thermalDisplay) throw new Error("thermal display helper is unavailable");
const matrixDisplay = window.MatrixDisplay;
if (!matrixDisplay) throw new Error("matrix display helper is unavailable");
const {
  HISTORY_WINDOW_MS,
  receivedAtMs,
  mergeTelemetryHistory,
  historyTimeDomain,
  timeToRatio,
} = telemetryHistory;
const {robustRange, smoothRange, median3} = thermalDisplay;
const {buildPressureBaseline, pressureDisplay, pressurePeakHold, thermalCandidateMask} = matrixDisplay;

const state = {
  device: "",
  snapshot: null,
  history: [],
  video: null,
  videoWanted: false,
  videoRetry: null,
  videoRetryDelay: 1000,
  audio: null,
  audioWanted: false,
  audioRetry: null,
  audioRetryDelay: 1000,
  recordingPending: new Set(),
  resourceRecordingPending: false,
  refreshTimer: null,
  refreshPromise: null,
  refreshQueued: false,
  lastHistoryAt: 0,
  historyRequestId: 0,
  telemetryArrivals: [],
  lastChartAt: 0,
  sampleSession: "",
  importSession: "",
  calibrationKey: "",
  environmentThresholdKey: "",
  thermalDisplay: {range: null, lastGoodMatrix: null, lastQuality: null},
  thermalOverlay: {matchedCells: 0, threshold: null, reason: "waiting"},
  pressureDisplay: {
    device: "", baseline: null, calibrationFrames: [], calibrationRemaining: 0,
    calibrationSequence: null, threshold: 0, sensitivity: 4, holdMs: 1000,
    held: null, heldSequence: null, heldAt: 0,
  },
  speakerVolume: {pending: false},
  warmLight: {pending: false, target: null, device: "", commandId: null, requestId: "", timeout: null},
  sootherPat: {pending: false, target: null, device: "", commandId: null, requestId: "", timeout: null},
  ai: {data: null, loading: false, lastLoadedAt: 0, pollTimer: null},
  canvasViews: {},
  realtime: {
    socket: null, retry: null, retryDelay: 1000, fallback: null,
    fallbackTimer: null, heartbeat: null, heartbeatSequence: 0,
    lastMessageAt: 0, liveStatePromise: null,
    pending: new Map(), renderScheduled: false,
  },
};

const fusionLabels = {
  0: "正常", 1: "靠近床缘", 2: "离床", 3: "温度过高", 4: "温度过低",
  5: "传感器故障", 6: "哭声", 7: "口鼻遮挡", 8: "站立", 9: "活动中",
  10: "睡眠", 11: "清醒", 12: "空气质量异常", 13: "疑似发热",
};
const alarmLevelLabels = ["L0 正常", "L1 提醒", "L2 警告", "L3 紧急"];
const fusionContextNames = ["在床状态", "睡眠/清醒", "床面活动", "口鼻视野", "床缘状态", "哭声状态", "哭声原因", "体表温度", "床面温度", "照护者", "系统健康"];
const fusionContextLabels = [
  ["未知", "在床", "离床", "不确定"],
  ["未知", "睡眠", "清醒", "过渡"],
  ["未知", "安静", "翻身", "活跃"],
  ["未知", "清楚", "部分遮挡", "已遮挡", "不可见"],
  ["未知", "中央", "靠近床缘", "抓扶床缘", "床外"],
  ["未知", "无哭声", "哭声", "持续哭声"],
  ["未知", "饥饿", "腹痛", "不适", "疲倦", "需拍嗝"],
  ["未知", "正常", "偏高", "偏低"],
  ["未知", "正常", "偏高", "偏低"],
  ["未知", "不在场", "在场"],
  ["正常", "降级", "监测盲区"],
];
const incidentLabels = ["无", "在床证据冲突", "传感器降级", "监测盲区", "口鼻不可见", "口鼻部分遮挡", "口鼻遮挡", "靠近床缘", "床缘攀爬", "离床", "哭声", "持续哭声", "温度过高", "温度过低", "摄像头冻结", "环境空气质量", "疑似发热", "室内 CO₂ 浓度过高", "室温过高", "湿度过高", "湿度过低", "疑似 VOC 升高（原始气阻降低）", "视觉监测盲区/疑似完全覆盖"];
const incidentLifecycleLabels = ["清除", "候选", "活动", "升级", "恢复观察", "已解除"];
const comfortActionBits = {lightOff: 1 << 0, lightOn: 1 << 1, pat: 1 << 2, caregiverCheck: 1 << 3};
const comfortReasonLabels = {
  [1 << 0]: "婴儿已睡眠", [1 << 1]: "确认哭声", [1 << 2]: "持续哭声",
  [1 << 3]: "活动安全事件", [1 << 4]: "监测盲区", [1 << 5]: "婴儿不在床",
};
const comfortBlockLabels = {
  [1 << 0]: "存在高风险事件", [1 << 1]: "没有可靠在床证据", [1 << 2]: "监测存在盲区",
};
const visionOverlayStaleMs = 12000;
const visionOverlayExpireMs = 30000;
const cryTypeLabels = {
  hungry: "疑似饥饿",
  belly_pain: "疑似腹痛",
  discomfort: "疑似不适",
  tired: "疑似疲倦",
  burping: "疑似需要拍嗝",
  unknown: "未知",
};
const visionStatusLabels = {
  0: "视觉线程启动",
  1: "等待摄像头帧",
  2: "正在解码 JPEG",
  3: "正在预处理",
  4: "正在加载模型 / NPU",
  5: "NPU 推理中",
  6: "推理完成",
  7: "JPEG 超过缓冲上限",
  8: "JPEG 解码失败",
  9: "NPU 推理失败",
  10: "OSPI 模型存储失败",
};
const visionSceneLabels = ["婴儿", "头部/面部", "毛毯", "软物体", "照护者"];
const visionOcclusionLabels = ["口鼻清楚", "部分遮盖", "完全遮盖", "头部不可见"];
const visionBehaviorLabels = ["躺卧", "坐姿", "爬行/跪姿", "站立", "未知"];
const aiStoryStatusLabels = {
  queued: "等待生成", generating: "正在生成文本", synthesizing: "正在合成语音",
  ready: "已生成", queued_for_device: "正在传给 RA8", queued_for_playback: "已排队播放",
  failed: "生成失败",
};
const aiAdvisoryLevelLabels = {normal: "状态平稳", attention: "需要关注", urgent: "紧急关注"};
const resetReasons = {
  0: "未知复位", 1: "上电复位", 2: "外部复位", 3: "软件复位", 4: "异常崩溃",
  5: "中断看门狗", 6: "任务看门狗", 7: "其他看门狗", 8: "深度睡眠唤醒",
  9: "电压过低", 10: "SDIO 复位", 11: "USB 复位", 12: "JTAG 复位",
  13: "熔丝错误", 14: "电源毛刺", 15: "CPU 锁死",
};
const environmentStateLabels = {
  0: "未初始化", 1: "正在启动", 2: "等待首帧", 3: "采样正常", 4: "未找到传感器",
  5: "CRC 错误", 6: "I²C 错误", 7: "采样超时", 8: "正在恢复",
};
const bme688StateLabels = {
  0: "未初始化", 1: "正在启动", 2: "等待测量", 3: "采样正常", 4: "未找到传感器",
  5: "数据/驱动错误", 6: "I²C 错误", 7: "采样超时", 8: "正在恢复",
};
const diagnosticStatusLabels = {
  ok: "正常", warning: "注意", fault: "故障", offline: "离线",
};
const groupLabels = {
  thermal: "热成像", bed_temperature: "床面温度", environment: "环境传感器",
  bme688: "BME688（迁移别名）",
  pressure: "压力矩阵",
  microphone: "RA8 麦克风", audio_player: "扬声器", camera: "RA8 摄像头",
  inference: "多模态推理", safety: "安全状态", ra8_link: "RA8 串口",
  esp32: "ESP32-P4X-EYE", gateway: "网关传输", health: "系统健康",
  vision: "RA8 视觉模型",
};
const collectionNames = {
  camera: "串口摄像头",
  temperature: "温度矩阵",
  thermal: "热成像",
  pressure: "压力矩阵",
  audio: "麦克风音频",
  resource: "CPU / NPU 性能参数",
};
const modelDataKinds = new Set(["camera", "audio"]);
const workflowNames = {
  model_dataset: "正式模型数据",
  reference_signal: "算法参考信号",
  fusion_candidate: "融合候选",
  legacy: "历史会话",
};
const modelFamilyNames = {
  vision: "视觉模型",
  cry_audio: "哭声模型",
  rule_reference: "规则设计",
  unspecified: "未指定",
};
const micWriterStates = {
  0: "写入器空闲",
  1: "SD 未就绪",
  2: "麦克风共享区异常",
  3: "等待录音数据",
  4: "正在创建目录",
  5: "文件打开失败",
  6: "SD 文件已打开",
  7: "SD 写入中",
  8: "SD 写入失败",
  9: "文件已关闭",
};
const cameraWriterStates = {
  0: "写入器空闲",
  1: "SD 未就绪",
  2: "等待下一帧",
  3: "正在创建目录",
  4: "文件打开失败",
  5: "JPEG 已打开",
  6: "JPEG 写入中",
  7: "JPEG 写入失败",
  8: "JPEG 已保存",
  9: "正在检查文件名",
  10: "正在打开 JPEG 临时文件",
  11: "正在关闭 JPEG 文件",
  12: "正在提交 JPEG 文件",
  13: "正在清理未完成文件",
};
const fatErrnoNames = {
  5: "FAT I/O 错误",
};
const sdInitSteps = {
  0: "等待初始化",
  1: "打开 SDHI",
  2: "识别 SD 卡",
  3: "创建 FAT 磁盘",
  4: "挂载分区",
  5: "注册 /sd0",
  6: "读取容量",
  7: "检查卡状态",
  8: "打开测试文件",
  9: "写入测试文件",
  10: "关闭测试文件",
  11: "初始化完成",
  12: "运行期 FAT I/O 恢复",
};
const qualityNames = {
  unreviewed: "未复核", good: "可用", bad: "剔除", uncertain: "不确定",
};
const splitNames = {
  unassigned: "未划分", train: "训练集", validation: "验证集", test: "测试集",
};
const collectionTaskNames = {
  vision_scene: "视觉场景目标",
  vision_occlusion: "视觉口鼻遮挡",
  vision_behavior: "视觉姿态行为",
  cry_detection: "哭声二分类",
  cry_type: "哭声类型",
  rule_reference: "融合规则参考",
};
const collectionTaskLabelOptions = {
  vision_scene: ["infant", "empty_bed", "blanket", "soft_object", "caregiver"],
  vision_occlusion: ["clear", "partial", "covered", "not_visible"],
  vision_behavior: ["lying", "sitting", "crawling_kneeling", "standing", "unknown"],
  cry_detection: ["crying", "non_crying"],
  cry_type: ["hungry", "belly_pain", "discomfort", "tired", "burping", "unknown"],
  rule_reference: ["empty_bed", "infant_center", "infant_edge", "standing", "face_partial", "face_covered", "heater_off", "heater_on"],
};
const modelTaskNames = {
  cry_classification: "哭声分类",
  vision_classification: "视觉分类",
  vision_detection: "视觉检测",
  fusion: "融合（候选）",
  classification: "分类",
  detection: "检测",
};
const $ = id => document.getElementById(id);
const esc = value => String(value ?? "").replace(/[&<>"']/g, char => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
}[char]));
const get = (object, path) => path.split(".").reduce((value, key) => value?.[key], object);

async function api(path, options) {
  const response = await fetch(path, options);
  if (!response.ok) throw new Error(await response.text());
  return response.json();
}

document.querySelectorAll(".tab").forEach(button => {
  button.onclick = () => {
    const openingMedia = button.dataset.tab === "media";
    const openingAi = button.dataset.tab === "ai";
    if (!openingMedia) {
      if (state.videoWanted) stopVideo();
      if (state.audioWanted) stopAudio();
    }
    document.querySelectorAll(".tab,.view").forEach(item => item.classList.remove("active"));
    button.classList.add("active");
    $(button.dataset.tab).classList.add("active");
    if (openingMedia && !state.videoWanted) startVideo();
    if (openingAi) loadAi(true).catch(console.error);
  };
});

$("device").onchange = async () => {
  const reopenVideo = state.videoWanted;
  stopVideo();
  stopAudio();
  state.device = $("device").value;
  state.historyRequestId++;
  state.history = [];
  state.lastHistoryAt = 0;
  state.telemetryArrivals = [];
  clearWarmLightPending();
  $("warm-light-result").textContent = "";
  clearSootherPatPending();
  $("soother-pat-result").textContent = "";
  state.sampleSession = "";
  state.importSession = "";
  state.calibrationKey = "";
  state.environmentThresholdKey = "";
  state.thermalDisplay = {range: null, lastGoodMatrix: null, lastQuality: null};
  state.thermalOverlay = {matchedCells: 0, threshold: null, reason: "waiting"};
  state.ai.data = null;
  state.ai.lastLoadedAt = 0;
  loadPressureCalibration();
  $("sd-import-panel").hidden = true;
  await refresh(true);
  if (reopenVideo && $("media").classList.contains("active")) startVideo();
};

function statusClass(element, kind) {
  element.classList.remove("ok", "danger", "warning", "offline");
  if (kind) element.classList.add(kind);
}

function formatTime(value) {
  if (!value) return "-";
  const date = new Date(value);
  return Number.isNaN(date.valueOf()) ? value : date.toLocaleString();
}

function ageText(value) {
  if (!value) return "无数据";
  const milliseconds = Date.now() - new Date(value).valueOf();
  if (!Number.isFinite(milliseconds)) return "时间未知";
  if (milliseconds < 1500) return "刚刚";
  if (milliseconds < 60000) return `${(milliseconds / 1000).toFixed(1)} 秒前`;
  return `${(milliseconds / 60000).toFixed(1)} 分钟前`;
}

function compact(value) {
  if (value === null || value === undefined) return "-";
  if (Array.isArray(value)) {
    if (value.length <= 8) return value.join(", ");
    const numbers = value.filter(item => typeof item === "number");
    return numbers.length === value.length
      ? `[${value.length} 项] 最小 ${Math.min(...numbers)} · 最大 ${Math.max(...numbers)}`
      : `[${value.length} 项]`;
  }
  if (typeof value === "object") return JSON.stringify(value);
  if (typeof value === "boolean") return value ? "是" : "否";
  return String(value);
}

function formatBytes(value) {
  const bytes = Number(value || 0);
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function flatten(object, prefix = "", result = []) {
  Object.entries(object || {}).forEach(([key, value]) => {
    const path = prefix ? `${prefix}.${key}` : key;
    if (value && typeof value === "object" && !Array.isArray(value)) flatten(value, path, result);
    else result.push([path, value]);
  });
  return result;
}

function pressureCalibrationKey() {
  return state.device ? `baby-monitor:pressure-baseline:${state.device}` : "";
}

function resetPressureHold() {
  const display = state.pressureDisplay;
  display.held = null;
  display.heldSequence = null;
  display.heldAt = 0;
}

function updatePressureCalibrationState() {
  const display = state.pressureDisplay;
  const status = $("pressure-calibration-state");
  if (!status) return;
  if (display.calibrationRemaining > 0) {
    status.textContent = `请保持空床，采集中 ${display.calibrationFrames.length}/20 帧`;
    return;
  }
  status.textContent = Array.isArray(display.baseline)
    ? `空床基线已启用 · ${display.baseline.length} 点`
    : "未校准，显示原始低端变化";
}

function loadPressureCalibration() {
  const display = state.pressureDisplay;
  display.device = state.device;
  display.baseline = null;
  display.calibrationFrames = [];
  display.calibrationRemaining = 0;
  display.calibrationSequence = null;
  display.threshold = 0;
  display.sensitivity = 4;
  display.holdMs = 1000;
  resetPressureHold();
  const key = pressureCalibrationKey();
  if (key) try {
    const saved = JSON.parse(localStorage.getItem(key) || "null");
    if (Array.isArray(saved?.baseline) && saved.baseline.length === 256) {
      display.baseline = saved.baseline.map(Number);
    }
    if (Number.isFinite(Number(saved?.threshold))) display.threshold = Number(saved.threshold);
    if (Number.isFinite(Number(saved?.sensitivity))) display.sensitivity = Math.max(1, Number(saved.sensitivity));
    if (Number.isFinite(Number(saved?.holdMs))) display.holdMs = Math.max(0, Number(saved.holdMs));
  } catch (error) {
    console.warn("压力空床基线读取失败", error);
  }
  if ($("pressure-threshold")) $("pressure-threshold").value = String(display.threshold);
  if ($("pressure-threshold-value")) $("pressure-threshold-value").value = String(display.threshold);
  if ($("pressure-sensitivity")) $("pressure-sensitivity").value = String(display.sensitivity);
  if ($("pressure-sensitivity-value")) $("pressure-sensitivity-value").value = `${display.sensitivity}×`;
  if ($("pressure-hold")) $("pressure-hold").value = String(display.holdMs);
  if ($("pressure-hold-value")) $("pressure-hold-value").value = `${(display.holdMs / 1000).toFixed(2).replace(/0+$/, "").replace(/\.$/, "")} s`;
  updatePressureCalibrationState();
}

function savePressureCalibration() {
  const key = pressureCalibrationKey();
  if (!key) return;
  try {
    localStorage.setItem(key, JSON.stringify({
      baseline: state.pressureDisplay.baseline,
      threshold: state.pressureDisplay.threshold,
      sensitivity: state.pressureDisplay.sensitivity,
      holdMs: state.pressureDisplay.holdMs,
      calibrated_at: new Date().toISOString(),
    }));
  } catch (error) {
    console.warn("压力空床基线保存失败", error);
  }
}

function collectPressureCalibration(matrix, values) {
  const display = state.pressureDisplay;
  if (display.calibrationRemaining <= 0 || values.length !== 256) return;
  const sequence = Number(matrix.sequence);
  if (Number.isFinite(sequence) && sequence === display.calibrationSequence) return;
  display.calibrationSequence = Number.isFinite(sequence) ? sequence : Date.now();
  display.calibrationFrames.push([...values]);
  display.calibrationRemaining--;
  if (display.calibrationRemaining === 0) {
    display.baseline = buildPressureBaseline(display.calibrationFrames);
    display.calibrationFrames = [];
    resetPressureHold();
    savePressureCalibration();
  }
  updatePressureCalibrationState();
}

function canvasTooltip(canvas) {
  const parent = canvas.parentElement;
  let tooltip = parent.querySelector(`.canvas-tooltip[data-for="${canvas.id}"]`);
  if (!tooltip) {
    tooltip = document.createElement("div");
    tooltip.className = "canvas-tooltip";
    tooltip.dataset.for = canvas.id;
    parent.appendChild(tooltip);
  }
  return tooltip;
}

function inspectCanvas(event) {
  const canvas = event.currentTarget;
  const view = state.canvasViews[canvas.id];
  if (!view) return;
  const rect = canvas.getBoundingClientRect();
  const canvasX = (event.clientX - rect.left) * canvas.width / Math.max(1, rect.width);
  const canvasY = (event.clientY - rect.top) * canvas.height / Math.max(1, rect.height);
  let lines = [];
  if (view.type === "matrix") {
    const col = Math.max(0, Math.min(view.cols - 1, Math.floor(canvasX / canvas.width * view.cols)));
    const row = Math.max(0, Math.min(view.rows - 1, Math.floor(canvasY / canvas.height * view.rows)));
    const index = row * view.cols + col;
    const raw = Number(view.raw[index]);
    if (view.unit === "deci_c") lines = [`行 ${row + 1} · 列 ${col + 1}`, `${(raw * 0.1).toFixed(1)} °C · 原始 ${raw}`];
    else {
      const corrected = Number(view.display[index]);
      const baseline = Number(view.baseline?.[index]);
      lines = [`行 ${row + 1} · 列 ${col + 1}`, `原始 ${raw} · 校正 ${corrected}`];
      if (Number.isFinite(baseline)) lines.push(`空床基线 ${baseline.toFixed(1)} · 阈值 ${view.threshold}`);
    }
    const output = $(`${canvas.id}-hover`);
    if (output) output.value = lines.join(" · ");
  } else if (view.type === "chart") {
    const ratio = Math.max(0, Math.min(1, (canvasX - view.pad.l) / view.plotWidth));
    let index = Math.round(ratio * Math.max(0, view.length - 1));
    if (view.timeDomain && view.timestamps?.length) {
      const target = view.timeDomain.start + ratio * (view.timeDomain.end - view.timeDomain.start);
      index = view.timestamps.reduce((best, timestamp, candidate) => (
        Number.isFinite(timestamp)
          && Math.abs(timestamp - target) < Math.abs((view.timestamps[best] ?? Infinity) - target)
          ? candidate : best
      ), 0);
    }
    const timestamp = view.timestamps?.[index];
    if (Number.isFinite(timestamp)) lines.push(new Date(timestamp).toLocaleTimeString());
    view.series.forEach((item, seriesIndex) => {
      const value = Number(item.values[index]);
      if (Number.isFinite(value)) lines.push(
        `${item.label || `序列 ${seriesIndex + 1}`} ${value.toFixed(view.decimals)}${view.unit}`,
      );
    });
  }
  const tooltip = canvasTooltip(canvas);
  if (!lines.length) {
    tooltip.classList.remove("ready");
    return;
  }
  tooltip.textContent = lines.join("\n");
  const parentRect = canvas.parentElement.getBoundingClientRect();
  tooltip.style.left = `${Math.max(4, Math.min(canvas.parentElement.clientWidth - 170, event.clientX - parentRect.left + 12))}px`;
  tooltip.style.top = `${Math.max(4, event.clientY - parentRect.top + 12)}px`;
  tooltip.classList.add("ready");
}

function registerCanvasInspector(canvas) {
  if (canvas.dataset.inspectorBound) return;
  canvas.dataset.inspectorBound = "1";
  canvas.addEventListener("pointermove", inspectCanvas);
  canvas.addEventListener("pointerleave", () => canvasTooltip(canvas).classList.remove("ready"));
}

function updateMatrixCaption(id, matrix, minimum = null, maximum = null, details = {}) {
  if (!matrix?.values?.length) return;
  const existingView = state.canvasViews[id] || {};
  details = {...(existingView.details || {}), ...details};
  const values = matrix.values.map(Number);
  const low = Number.isFinite(minimum) ? minimum : Number.isFinite(existingView.minimum) ? existingView.minimum
    : (Number.isFinite(matrix.minimum) ? matrix.minimum : Math.min(...values));
  const high = Number.isFinite(maximum) ? maximum : Number.isFinite(existingView.maximum) ? existingView.maximum
    : (Number.isFinite(matrix.maximum) ? matrix.maximum : Math.max(...values));
  const scale = matrix.unit === "deci_c" ? 0.1 : 1;
  const suffix = matrix.unit === "deci_c" ? " °C" : ` ${matrix.unit || "raw"}`;
  const receivedMs = Date.parse(matrix.received_at || "");
  const ageMs = Number.isFinite(receivedMs) ? Math.max(0, Date.now() - receivedMs)
    : Number(matrix.frame_age_ms);
  const delayed = Number.isFinite(ageMs) && ageMs > 2000;
  const fps = Number(matrix.fps || 0).toFixed(1);
  const drops = Number(matrix.drops || 0);
  const view = existingView;
  const rawLow = Number.isFinite(details.rawMinimum) ? details.rawMinimum
    : (view.raw?.length ? Math.min(...view.raw) : low);
  const rawHigh = Number.isFinite(details.rawMaximum) ? details.rawMaximum
    : (view.raw?.length ? Math.max(...view.raw) : high);
  const diagnostics = id === "thermal"
    ? `显示色阶 ${(low * scale).toFixed(1)} 至 ${(high * scale).toFixed(1)} °C · `
      + `原始 ${(rawLow * scale).toFixed(1)} 至 ${(rawHigh * scale).toFixed(1)} °C · `
      + `P1/P99 ${details.p01Text || "等待遥测"} · ${details.qualityText || "质量字段等待中"} · `
      + `过渡丢弃 ${details.transientDrops ?? "-"} · 共享重试 ${details.generationRetries ?? "-"} · `
    : `校正显示 ${(low * scale).toFixed(scale === 1 ? 0 : 1)} 至 `
      + `${(high * scale).toFixed(scale === 1 ? 0 : 1)}${suffix} · `
      + `原始 ${rawLow.toFixed(0)} 至 ${rawHigh.toFixed(0)} · `
      + `显示阈值 ${view.threshold ?? state.pressureDisplay.threshold} · `
      + `低压增强 ${view.sensitivity ?? state.pressureDisplay.sensitivity}× · `
      + `峰值保持 ${((view.holdMs ?? state.pressureDisplay.holdMs) / 1000).toFixed(2)} s · `
      + `${view.baseline ? "空床基线已启用" : "未校准"} · `;
  $(`${id}-range`).textContent = `${matrix.rows}×${matrix.cols} · `
    + diagnostics
    + `序列 ${matrix.sequence ?? "-"} · ${fps} FPS · 丢帧 ${drops} · `
    + `${delayed ? "数据延迟" : ageText(matrix.received_at)}`;
}

function heatmap(id, matrix) {
  const canvas = $(id);
  registerCanvasInspector(canvas);
  const context = canvas.getContext("2d");
  context.clearRect(0, 0, canvas.width, canvas.height);
  if (!matrix?.values?.length) {
    delete state.canvasViews[id];
    context.fillStyle = "#819096";
    context.font = "14px sans-serif";
    context.fillText("等待矩阵数据", 18, 28);
    $(`${id}-range`).textContent = "无数据";
    return;
  }
  const thermal = get(state.snapshot, "telemetry.sensors.thermal") || {};
  const qualityValue = Number(thermal.frame_quality);
  const qualityKnown = Number.isFinite(qualityValue);
  const qualityNormal = !qualityKnown || qualityValue === 0;
  let displayMatrix = matrix;
  if (id === "thermal") {
    state.thermalDisplay.lastQuality = qualityKnown ? qualityValue : null;
    if (qualityNormal) state.thermalDisplay.lastGoodMatrix = matrix;
    else if (state.thermalDisplay.lastGoodMatrix) displayMatrix = state.thermalDisplay.lastGoodMatrix;
    else {
      context.clearRect(0, 0, canvas.width, canvas.height);
      $(`${id}-range`).textContent = qualityValue === 1
        ? "热成像校准/过渡帧已过滤，等待第一帧正常画面"
        : "当前热成像帧无效，等待第一帧正常画面";
      return;
    }
  }
  const rawValues = displayMatrix.values.map(Number);
  let values = rawValues;
  let pressureView = null;
  if (id === "pressure") {
    if (state.pressureDisplay.device !== state.device) loadPressureCalibration();
    collectPressureCalibration(displayMatrix, rawValues);
    pressureView = pressureDisplay(
      rawValues,
      state.pressureDisplay.baseline,
      state.pressureDisplay.threshold,
    );
    const sequence = String(displayMatrix.sequence ?? "");
    if (sequence !== state.pressureDisplay.heldSequence) {
      const now = Date.now();
      const elapsed = state.pressureDisplay.heldAt
        ? now - state.pressureDisplay.heldAt : state.pressureDisplay.holdMs;
      state.pressureDisplay.held = pressurePeakHold(
        pressureView.corrected,
        state.pressureDisplay.held,
        elapsed,
        state.pressureDisplay.holdMs,
      );
      state.pressureDisplay.heldSequence = sequence;
      state.pressureDisplay.heldAt = now;
    }
    values = state.pressureDisplay.held || pressureView.corrected;
  }
  const rawMinimum = Math.min(...rawValues);
  const rawMaximum = Math.max(...rawValues);
  let minimum = Number.isFinite(displayMatrix.minimum) ? displayMatrix.minimum : rawMinimum;
  let maximum = Number.isFinite(displayMatrix.maximum) ? displayMatrix.maximum : rawMaximum;
  if (id === "thermal") {
    const currentRange = robustRange(values);
    if (qualityNormal || !state.thermalDisplay.range) {
      state.thermalDisplay.range = smoothRange(state.thermalDisplay.range, currentRange);
    }
    minimum = state.thermalDisplay.range?.minimum ?? minimum;
    maximum = state.thermalDisplay.range?.maximum ?? maximum;
  } else if (pressureView) {
    minimum = pressureView.minimum;
    maximum = Math.max(1, ...values);
  }
  const range = Math.max(1, maximum - minimum);
  const cellWidth = canvas.width / displayMatrix.cols;
  const cellHeight = canvas.height / displayMatrix.rows;
  values.forEach((value, index) => {
    let ratio = Math.max(0, Math.min(1, (value - minimum) / range));
    if (id === "pressure") {
      ratio = Math.pow(Math.max(0, ratio), 1 / Math.max(1, state.pressureDisplay.sensitivity));
    }
    const hue = id === "thermal" ? 240 - ratio * 240 : 270 - ratio * 220;
    context.fillStyle = `hsl(${hue} 82% ${24 + ratio * 34}%)`;
    context.fillRect(
      (index % displayMatrix.cols) * cellWidth,
      Math.floor(index / displayMatrix.cols) * cellHeight,
      cellWidth + 0.5,
      cellHeight + 0.5,
    );
  });
  state.canvasViews[id] = {
    type: "matrix", rows: Number(displayMatrix.rows), cols: Number(displayMatrix.cols),
    raw: rawValues, display: values, unit: displayMatrix.unit,
    baseline: pressureView?.hasBaseline ? state.pressureDisplay.baseline : null,
    threshold: pressureView?.threshold ?? 0,
    sensitivity: state.pressureDisplay.sensitivity,
    holdMs: state.pressureDisplay.holdMs,
    minimum, maximum,
  };
  const p01 = Number(thermal.p01_cd);
  const p99 = Number(thermal.p99_cd);
  const qualityLabels = ["正常", "校准/过渡帧已过滤", "当前帧无效，保留上一画面"];
  const captionDetails = {
    rawMinimum,
    rawMaximum,
    p01Text: Number.isFinite(p01) && Number.isFinite(p99)
      ? `${(p01 * 0.1).toFixed(1)} 至 ${(p99 * 0.1).toFixed(1)} °C`
      : "旧固件未提供",
    qualityText: qualityLabels[qualityValue] || "质量字段未提供",
    transientDrops: Number(thermal.transient_drop_count || 0),
    generationRetries: Number(thermal.generation_retry_count || 0),
  };
  state.canvasViews[id].details = captionDetails;
  updateMatrixCaption(id, displayMatrix, minimum, maximum, captionDetails);
}

function lineChart(id, series, options = {}) {
  const canvas = $(id);
  registerCanvasInspector(canvas);
  const context = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;
  const pad = {l: 46, r: 12, t: 14, b: 25};
  context.clearRect(0, 0, width, height);
  context.fillStyle = "#fff";
  context.fillRect(0, 0, width, height);
  const all = series.flatMap(item => item.values).filter(Number.isFinite);
  if (!all.length) {
    delete state.canvasViews[id];
    context.fillStyle = "#8a979c";
    context.font = "12px sans-serif";
    context.fillText("等待数据", pad.l, pad.t + 18);
    return;
  }
  let minimum = options.min ?? Math.min(...all);
  let maximum = options.max ?? Math.max(...all);
  if (minimum === maximum) { minimum -= 1; maximum += 1; }
  const plotWidth = width - pad.l - pad.r;
  const plotHeight = height - pad.t - pad.b;
  const timestamps = Array.isArray(options.timestamps) ? options.timestamps : null;
  const timeDomain = timestamps
    ? historyTimeDomain(timestamps.map(received_at => ({received_at})), options.windowMs)
    : null;
  context.strokeStyle = "#e1e7e9";
  context.fillStyle = "#6b797f";
  context.font = "11px sans-serif";
  context.lineWidth = 1;
  for (let line = 0; line <= 4; line++) {
    const y = pad.t + plotHeight * line / 4;
    context.beginPath(); context.moveTo(pad.l, y); context.lineTo(width - pad.r, y); context.stroke();
    const value = maximum - (maximum - minimum) * line / 4;
    context.fillText(`${value.toFixed(options.decimals ?? 0)}${options.unit || ""}`, 2, y + 4);
  }
  series.forEach(item => {
    context.strokeStyle = item.color;
    context.lineWidth = 2;
    context.beginPath();
    let started = false;
    item.values.forEach((value, index) => {
      if (!Number.isFinite(value)) { started = false; return; }
      let xRatio = item.values.length < 2 ? 1 : index / (item.values.length - 1);
      if (timeDomain && Number.isFinite(timestamps[index])) {
        xRatio = timeToRatio(timestamps[index], timeDomain.end, options.windowMs);
        if (!Number.isFinite(xRatio) || xRatio < 0 || xRatio > 1) {
          started = false;
          return;
        }
      }
      const x = pad.l + xRatio * plotWidth;
      const y = pad.t + (maximum - value) / (maximum - minimum) * plotHeight;
      if (started) context.lineTo(x, y); else { context.moveTo(x, y); started = true; }
    });
    context.stroke();
  });
  state.canvasViews[id] = {
    type: "chart", series, timestamps, timeDomain, pad, plotWidth,
    length: Math.max(0, ...series.map(item => item.values.length)),
    decimals: Number(options.decimals ?? 0), unit: options.unit || "",
  };
}

function microphoneActiveSlotPeak(microphone) {
  if (!microphone || microphone.wslot == null) return Number(microphone?.peak);
  const slotPeak = Number(microphone.wslot) === 1 ? microphone.wopk : microphone.wepk;
  return Number(slotPeak ?? microphone.peak);
}

function bme688FromTelemetry(telemetry) {
  const value = get(telemetry, "sensors.environment.bme688")
    || get(telemetry, "sensors.bme688");
  return value && typeof value === "object" && !Array.isArray(value) ? value : {};
}

function tickAgeMs(runtime, lastGood) {
  const now = Number(runtime);
  const last = Number(lastGood);
  if (!Number.isInteger(now) || !Number.isInteger(last)
      || now < 0 || now > 0xFFFFFFFF || last < 0 || last > 0xFFFFFFFF) return null;
  if (now >= last) return now - last;
  if (last - now > 0x80000000) return (now - last) >>> 0;
  return null;
}

function thermalTrendSeries(history, robustField, legacyField) {
  const raw = history.map(row => {
    const thermal = get(row, "sensors.thermal") || {};
    const quality = Number(thermal.frame_quality);
    if (Number.isFinite(quality) && quality !== 0) return Number.NaN;
    const value = thermal[robustField] ?? thermal[legacyField];
    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric / 10 : Number.NaN;
  });
  return median3(raw);
}

function updateCharts() {
  const history = state.history;
  const timestamps = history.map(receivedAtMs);
  const timeOptions = {timestamps, windowMs: HISTORY_WINDOW_MS};
  const thermalMin = thermalTrendSeries(history, "p01_cd", "min_cd");
  const thermalMax = thermalTrendSeries(history, "p99_cd", "max_cd");
  lineChart("temperature-chart", [
    {label: "P1", values: thermalMin, color: "#2584a6"},
    {label: "P99", values: thermalMax, color: "#d34e40"},
  ], {...timeOptions, unit: "°", decimals: 1});
  lineChart("microphone-chart", [{
    label: "峰值", values: history.map(row => microphoneActiveSlotPeak(get(row, "sensors.microphone"))), color: "#8b55a8",
  }], {...timeOptions, min: 0});
  lineChart("pressure-chart", [{
    label: "总载荷",
    values: history.map(row => Number(get(row, "sensors.pressure.total") ?? get(row, "inference.p_total"))),
    color: "#087f67",
  }], {...timeOptions, min: 0});
  lineChart("wifi-chart", [{label: "RSSI", values: history.map(row => Number(row.wifi_rssi)), color: "#d08116"}], {
    ...timeOptions, min: -100, max: -20, unit: " dBm",
  });
  lineChart("ra8-utilization-chart", [
    {
      label: "CPU0",
      values: history.map(row => Number(get(row, "health.ra8_resources.cpu0_usage_permille")) / 10),
      color: "#087f67",
    },
    {
      label: "CPU1",
      values: history.map(row => Number(get(row, "health.ra8_resources.cpu1_usage_permille")) / 10),
      color: "#2584a6",
    },
    {
      label: "NPU",
      values: history.map(row => Number(get(row, "health.ra8_resources.npu_usage_permille")) / 10),
      color: "#d08116",
    },
  ], {...timeOptions, min: 0, max: 100, unit: " %", decimals: 1});
  lineChart("ra8-heap-chart", [
    {
      label: "CPU0",
      values: history.map(row => Number(get(row, "health.ra8_resources.cpu0_heap_free")) / 1024),
      color: "#087f67",
    },
    {
      label: "CPU1",
      values: history.map(row => Number(get(row, "health.ra8_resources.cpu1_heap_free")) / 1024),
      color: "#8b55a8",
    },
  ], {...timeOptions, min: 0, unit: " KiB", decimals: 1});
  lineChart("bme688-temperature-chart", [{
    label: "温度", values: history.map(row => Number(bme688FromTelemetry(row).t_mc) / 1000), color: "#d34e40",
  }], {...timeOptions, unit: " °C", decimals: 1});
  lineChart("bme688-humidity-chart", [{
    label: "湿度", values: history.map(row => Number(bme688FromTelemetry(row).rh_mp) / 1000), color: "#2584a6",
  }], {...timeOptions, min: 0, max: 100, unit: " %", decimals: 1});
  lineChart("bme688-pressure-chart", [{
    label: "气压", values: history.map(row => Number(bme688FromTelemetry(row).p_pa) / 100), color: "#087f67",
  }], {...timeOptions, unit: " hPa", decimals: 1});
  lineChart("bme688-gas-chart", [{
    label: "气阻", values: history.map(row => Number(bme688FromTelemetry(row).gas_ohm) / 1000), color: "#8b55a8",
  }], {...timeOptions, min: 0, unit: " kΩ", decimals: 1});
  if (history.length) {
    $("history-range").textContent = `${formatTime(history[0].received_at)} 至 `
      + `${formatTime(history.at(-1).received_at)} · ${history.length} 点`;
  }
}

function renderParameters(telemetry) {
  const groups = {...(telemetry.sensors || {}), inference: telemetry.inference || {}, ...(telemetry.health || {})};
  $("parameter-groups").innerHTML = Object.entries(groups).map(([name, value]) => {
    const rows = flatten(value).map(([key, item]) => `<dt>${esc(key)}</dt><dd>${esc(compact(item))}</dd>`).join("");
    return `<article class="parameter-group"><h2>${esc(groupLabels[name] || name)}</h2>`
      + `<dl class="parameter-list">${rows || "<dt>状态</dt><dd>无数据</dd>"}</dl></article>`;
  }).join("");
  $("raw-json").textContent = JSON.stringify(telemetry, null, 2);
}

function renderSensorDiagnostics(diagnostics) {
  const items = diagnostics?.items || [];
  const sensorSummary = diagnostics?.sensor_summary || {};
  const systemSummary = diagnostics?.summary || {};
  const overall = diagnostics?.overall || "offline";
  const summary = `传感器正常 ${sensorSummary.ok || 0} · 故障 ${sensorSummary.fault || 0}`
    + ` · 注意 ${sensorSummary.warning || 0} · 离线 ${sensorSummary.offline || 0}`;
  $("sensor-health-summary").textContent = summary;
  statusClass($("sensor-health-summary"), overall === "fault" ? "danger" : overall);

  if (!items.length) {
    $("sensor-health-grid").innerHTML = '<article class="sensor-health-card status-offline">等待设备诊断数据</article>';
    return;
  }

  $("sensor-health-grid").innerHTML = items.map(item => {
    const status = diagnosticStatusLabels[item.status] ? item.status : "offline";
    const stateClass = status === "fault" ? "danger" : status;
    const channels = (item.channels || []).map(channel => {
      const channelStatus = diagnosticStatusLabels[channel.status] ? channel.status : "offline";
      return `<li><i class="sensor-channel-dot status-${esc(channelStatus)}"></i>`
        + `<span class="sensor-channel-name">${esc(channel.name)}</span>`
        + `<span class="sensor-channel-detail">${esc(channel.detail)}</span></li>`;
    }).join("");
    return `<article class="sensor-health-card status-${esc(status)}">`
      + `<div class="sensor-health-title"><h2>${esc(item.name)}</h2>`
      + `<strong class="sensor-health-state ${esc(stateClass)}">${esc(diagnosticStatusLabels[status])}</strong></div>`
      + `<p class="sensor-health-summary">${esc(item.summary)}</p>`
      + `<p class="sensor-health-detail">${esc(item.detail)}</p>`
      + `<ul class="sensor-channel-list">${channels}</ul></article>`;
  }).join("");

  if (Number(systemSummary.fault || 0) > Number(sensorSummary.fault || 0)) {
    $("sensor-health-summary").title = "采集链路或 SD 卡还存在系统级故障";
  } else {
    $("sensor-health-summary").removeAttribute("title");
  }
}

function updateMedia(snapshot) {
  const media = snapshot.media || {};
  const video = media.video;
  const videoDiagnostics = media.video_diagnostics || {};
  const audio = media.audio;
  const audioDiagnostics = media.audio_diagnostics || {};
  const videoUploaders = Number(media.video_uploaders || 0);
  const videoListeners = Number(media.video_wss_listeners || 0);
  const uploaders = Number(media.audio_uploaders || 0);
  const listeners = Number(media.audio_wss_listeners || 0);
  $("media-state").textContent = `视频 ${videoUploaders ? "在线" : "离线"} · 音频 ${uploaders ? "在线" : "离线"}`;
  $("s3-video-state").textContent = videoUploaders ? "WSS 在线" : "未连接";
  statusClass($("s3-video-state"), videoUploaders ? "ok" : "warning");
  $("s3-video-detail").textContent = `WSS 上传器 ${videoUploaders} · ${Number(videoDiagnostics.received_fps || 0).toFixed(1)} FPS · `
    + `队列 ${videoDiagnostics.queue_depth || 0}/${videoDiagnostics.queue_limit || 1} · `
    + `${video ? ageText(video.received_at) : "无 JPEG 帧"}`;
  $("video-listener-state").textContent = state.videoWanted ? "预览中" : "未开启";
  statusClass($("video-listener-state"), state.videoWanted ? "ok" : "");
  $("video-listener-detail").textContent = `服务器 WSS 订阅者 ${videoListeners}`;
  if (!state.videoWanted) {
    $("video-frame-state").textContent = String(video?.frame_count || 0);
    $("video-frame-detail").textContent = `序列 ${video?.sequence ?? "-"} · ${video?.gap_count || 0} 帧间断`
      + ` · 无效 ${videoDiagnostics.invalid_frames || 0}`;
  }
  $("video-format-state").textContent = video?.width && video?.height ? `${video.width}×${video.height}` : "-";
  $("video-format-detail").textContent = video
    ? `${formatBytes(video.bytes || 0)} · JPEG quality ${video.quality ?? "-"} · ${video.transport || "未知通道"}`
    : "等待第一帧";
  $("s3-audio-state").textContent = uploaders ? "WSS 在线" : "未连接";
  statusClass($("s3-audio-state"), uploaders ? "ok" : "warning");
  $("s3-audio-detail").textContent = `WSS 上传器 ${uploaders} · ${Number(audioDiagnostics.received_fps || 0).toFixed(1)} FPS · `
    + `队列 ${audioDiagnostics.queue_depth || 0}/${audioDiagnostics.queue_limit || 8} · `
    + `${audio ? ageText(audio.received_at) : "无音频帧"}`;
  $("audio-listener-state").textContent = state.audioWanted ? "监听中" : "未开启";
  statusClass($("audio-listener-state"), state.audioWanted ? "ok" : "");
  $("audio-listener-detail").textContent = `服务器订阅者 ${listeners}`;
  if (!state.audioWanted) {
    $("audio-frame-state").textContent = String(audio?.frame_count || 0);
    $("audio-frame-detail").textContent = `序列 ${audio?.sequence ?? "-"} · ${audio?.gap_count || 0} 帧间断`;
  }
  $("audio-detail").textContent = audio
    ? `${audio.sample_rate} Hz · ${audio.channels} 声道 · PCM${audio.bits} · ${audio.samples || 320} 样本/帧`
    : "16 kHz · 单声道 · PCM16 · 20 ms/帧";
}

function recordingBasename(kind) {
  const input = $(`recording-${kind}-filename`);
  let value = input.value.trim();
  const suffix = kind === "audio" ? /\.wav$/i : /\.(?:jpe?g)$/i;
  value = value.replace(suffix, "");
  if (!value) return "";
  if (!/^[A-Za-z0-9][A-Za-z0-9_-]{0,7}$/.test(value)) {
    throw new Error("文件名只能包含 1–8 个英文字母、数字、下划线或连字符");
  }
  return value;
}

function updateRecordingPath(kind) {
  const folder = kind === "audio" ? "MIC" : "CAMERA";
  const extension = kind === "audio" ? "WAV" : "JPG";
  let basename = "";
  try {
    basename = recordingBasename(kind);
    $(`recording-${kind}-filename`).classList.remove("invalid");
  } catch (_) {
    $(`recording-${kind}-filename`).classList.add("invalid");
  }
  $(`recording-${kind}-path`).textContent = `/${folder}/${basename || "自动编号"}.${extension}`;
}

function generatedRecordingName(kind) {
  const stamp = Date.now().toString(36).toUpperCase().slice(-7).padStart(7, "0");
  return `${kind === "audio" ? "M" : "C"}${stamp}`;
}

function renderRecordingTests(snapshot) {
  const telemetry = snapshot.telemetry || {};
  const health = telemetry.health || {};
  const device = (snapshot.devices || []).find(item => item.device_id === state.device) || {};
  const microphone = get(telemetry, "sensors.microphone") || {};
  const camera = get(telemetry, "sensors.camera") || {};
  const ra8Online = health.ra8_online === true;
  const audioActive = Number(microphone.rec || 0) === 1;
  const cameraActive = Number(camera.rec || 0) === 1;
  const audioWriterState = Number(microphone.wst ?? -1);
  const cameraWriterState = Number(camera.wst ?? -1);
  const cameraWriterAckTimeout = Number(camera.fail_stage || 0) === 4 && Number(camera.fsp || 0) === 20;
  const hasSdDiagnostics = Object.prototype.hasOwnProperty.call(microphone, "sdr");
  const sdReady = hasSdDiagnostics && Number(microphone.sdr || 0) === 1;
  const sdStep = Number(microphone.sds || 0);
  const sdFailureStep = Number(microphone.sdfs || 0);
  const sdIncompleteReady = sdReady && [4, 5, 6].includes(sdStep);
  const sdUsable = sdReady && !sdIncompleteReady;
  const effectiveCameraWriterState = cameraWriterState === 1 && sdUsable ? 2 : cameraWriterState;

  $("recording-page-state").textContent = device.online
    ? (ra8Online ? "设备与 RA8 在线" : "网关在线，RA8 离线") : "设备离线";
  $("recording-device-state").textContent = device.online ? "在线" : "离线";
  $("recording-device-detail").textContent = state.device
    ? `${state.device} · ${ageText(device.last_seen)}` : "未选择设备";
  statusClass($("recording-device-state"), device.online ? "ok" : "danger");
  $("recording-ra8-state").textContent = ra8Online ? "在线" : "离线";
  $("recording-ra8-detail").textContent = `状态帧 ${health.uart_status_frames ?? "-"} · CRC ${health.uart_crc_errors ?? "-"}`;
  statusClass($("recording-ra8-state"), ra8Online ? "ok" : "danger");

  $("recording-sd-state").textContent = hasSdDiagnostics
    ? (sdIncompleteReady ? "初始化阻塞" : (sdUsable ? "已挂载" : "正在自恢复")) : "等待新固件";
  $("recording-sd-detail").textContent = hasSdDiagnostics
    ? (sdIncompleteReady
      ? `${sdInitSteps[sdStep] || `阶段 ${sdStep}`} · ready 提前置位，文件线程尚不可用`
      : (sdUsable
      ? `${sdInitSteps[sdStep] || `阶段 ${sdStep}`} · 初始化 ${microphone.sdtry ?? 0} 次 · 恢复 ${microphone.sdrec ?? 0} 次`
      : `${sdInitSteps[sdFailureStep || sdStep] || `阶段 ${sdFailureStep || sdStep}`} · FSP ${microphone.sdfsp ?? 0} · FAT ${microphone.sdff ?? 0}`))
    : "烧录本次 CPU0/CPU1 固件后显示详细错误";
  statusClass($("recording-sd-state"), hasSdDiagnostics ? (sdUsable ? "ok" : "danger") : "");

  $("recording-audio-state").textContent = micWriterStates[audioWriterState] || "等待状态";
  $("recording-audio-detail").textContent = `写入 ${formatBytes(microphone.wbytes)} · 错误 ${microphone.werr ?? 0}`;
  statusClass($("recording-audio-state"), Number(microphone.werr || 0)
    ? "danger" : audioActive ? "ok" : "");
  $("recording-audio-active").textContent = audioActive ? "正在录音" : "未录音";
  statusClass($("recording-audio-active"), audioActive ? "ok" : "");
  $("recording-audio-file").textContent = microphone.file ? `/MIC/${microphone.file}` : "-";
  $("recording-audio-seconds").textContent = `${microphone.rec_sec ?? 0} 秒`;
  $("recording-audio-bytes").textContent = `${formatBytes(microphone.rec_bytes)} / ${formatBytes(microphone.wbytes)}`;
  $("recording-audio-command").textContent = `${microphone.ack ?? "-"} / ${microphone.cmd_seq ?? "-"}`;
  $("recording-audio-error").textContent = String(microphone.werr ?? 0);
  statusClass($("recording-audio-error"), Number(microphone.werr || 0) ? "danger" : "");

  $("recording-camera-state").textContent = cameraWriterStates[effectiveCameraWriterState] || "等待状态";
  $("recording-camera-detail").textContent = cameraWriterAckTimeout
    ? `JPEG 分块写入超时 · 已保存 ${camera.saved ?? 0}`
    : `已保存 ${camera.saved ?? 0} · 写入 ${formatBytes(camera.wbytes)}`;
  statusClass($("recording-camera-state"), Number(camera.werr || 0) || Number(camera.frame_err || 0) || cameraWriterAckTimeout
    ? "danger" : cameraActive ? "ok" : "");
  $("recording-camera-active").textContent = cameraActive ? "连续采集中" : "未连续采集";
  statusClass($("recording-camera-active"), cameraActive ? "ok" : "");
  $("recording-camera-file").textContent = camera.file ? `/CAMERA/${camera.file}` : "-";
  $("recording-camera-count").textContent = `${camera.frames ?? 0} / ${camera.saved ?? 0} 帧`;
  $("recording-camera-jpeg").textContent = `SOI ${camera.soi ?? "-"} / EOI ${camera.eoi ?? "-"}`;
  $("recording-camera-command").textContent = `${camera.ack ?? "-"} / ${camera.cmd_seq ?? "-"}`;
  $("recording-camera-error").textContent = `${camera.frame_err ?? 0} / ${camera.werr ?? 0}`;
  statusClass($("recording-camera-error"), Number(camera.frame_err || 0) || Number(camera.werr || 0) || cameraWriterAckTimeout
    ? "danger" : "");

  document.querySelectorAll("[data-recording-kind]").forEach(button => {
    const kind = button.dataset.recordingKind;
    const action = button.dataset.recordingAction;
    const pending = state.recordingPending.has(kind);
    const active = kind === "audio" ? audioActive : cameraActive;
    button.disabled = !state.device || pending || ((action === "start" || action === "capture") && active);
  });

  const actionNames = {start: "开始", stop: "停止", capture: "单张拍摄"};
  const kindNames = {audio: "麦克风", camera: "摄像头"};
  const recordingCommands = (snapshot.commands || []).filter(item => item.command?.args?.purpose === "media_test");
  $("recording-command-rows").innerHTML = recordingCommands.map(item => {
    const command = item.command || {};
    const kind = command.args?.kinds?.[0] || "-";
    const action = command.command === "collection.capture"
      ? "capture" : String(command.command || "").split(".").at(-1);
    return `<tr><td>${item.id}</td><td>${esc(formatTime(item.created_at))}</td>`
      + `<td>${esc(kindNames[kind] || kind)}</td><td>${esc(actionNames[action] || action)}</td>`
      + `<td><code>${esc(command.args?.filename || "自动编号")}</code></td><td>${esc(item.status)}</td>`
      + `<td><code>${esc(compact(item.result))}</code></td></tr>`;
  }).join("") || `<tr><td colspan="7" class="not-applicable">还没有录制测试命令</td></tr>`;
}

async function sendRecordingControl(kind, action) {
  const resultElement = $(`recording-${kind}-result`);
  if (!state.device) {
    resultElement.textContent = "请先选择设备";
    resultElement.classList.add("error");
    return;
  }

  let filename = "";
  try {
    if (action !== "stop") filename = recordingBasename(kind);
  } catch (error) {
    resultElement.textContent = error.message;
    resultElement.classList.add("error");
    return;
  }

  state.recordingPending.add(kind);
  resultElement.classList.remove("error");
  resultElement.textContent = "正在排队命令";
  renderRecordingTests(state.snapshot || {});
  try {
    const result = await api(`/api/dashboard/recordings/${kind}/${action}`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({device_id: state.device, filename}),
    });
    resultElement.textContent = result.expected_path
      ? `命令 #${result.id} 已排队 · ${result.expected_path}`
      : `停止命令 #${result.id} 已排队`;
    await refresh();
  } catch (error) {
    resultElement.textContent = `下发失败：${error.message}`;
    resultElement.classList.add("error");
  } finally {
    state.recordingPending.delete(kind);
    renderRecordingTests(state.snapshot || {});
  }
}

function telemetryRate() {
  const arrivals = state.telemetryArrivals;
  if (arrivals.length < 2) return null;
  const elapsed = arrivals.at(-1) - arrivals[0];
  return elapsed > 0 ? (arrivals.length - 1) * 1000 / elapsed : null;
}

function visionScoreRows(labels, values) {
  return labels.map((label, index) => {
    const raw = Math.max(0, Math.min(1000, Number(values?.[index] || 0)));
    const percent = raw / 10;
    return `<div class="vision-score-row"><span>${esc(label)}</span>`
      + `<span class="vision-score-track"><i style="width:${percent.toFixed(1)}%"></i></span>`
      + `<b>${percent.toFixed(1)}%</b></div>`;
  }).join("");
}

function renderVisionBoxes(vision, detections, hasVision) {
  const preview = $("vision-box-preview");
  const layer = $("vision-box-layer");
  const empty = $("vision-box-empty");
  const imageWidth = Math.max(0, Number(vision.w || 0));
  const imageHeight = Math.max(0, Number(vision.h || 0));
  const colors = ["#4dd0a8", "#56a8ff", "#ffca5c", "#e98bff", "#ff7c7c"];

  preview.style.aspectRatio = imageWidth > 0 && imageHeight > 0
    ? `${imageWidth} / ${imageHeight}` : "4 / 3";
  layer.innerHTML = imageWidth > 0 && imageHeight > 0
    ? detections.map(item => {
      const values = Array.isArray(item) ? item : [];
      const classId = Number(values[0] ?? -1);
      const score = Math.max(0, Math.min(1000, Number(values[1] || 0)));
      const x = Math.max(0, Math.min(imageWidth, Number(values[2] || 0)));
      const y = Math.max(0, Math.min(imageHeight, Number(values[3] || 0)));
      const width = Math.max(0, Math.min(imageWidth - x, Number(values[4] || 0)));
      const height = Math.max(0, Math.min(imageHeight - y, Number(values[5] || 0)));
      const color = colors[classId] || "#b9c5c9";
      const label = visionSceneLabels[classId] || `类别 ${classId}`;
      return `<i class="vision-detection-box" style="--box-color:${color};`
        + `left:${(x * 100 / imageWidth).toFixed(3)}%;top:${(y * 100 / imageHeight).toFixed(3)}%;`
        + `width:${(width * 100 / imageWidth).toFixed(3)}%;height:${(height * 100 / imageHeight).toFixed(3)}%">`
        + `<span>${esc(label)} ${(score / 10).toFixed(1)}%</span></i>`;
    }).join("") : "";
  empty.hidden = detections.length > 0;
  empty.textContent = !hasVision
    ? "等待第一帧坐标"
    : imageWidth <= 0 || imageHeight <= 0
      ? "尚未收到有效图像尺寸"
      : "当前帧没有超过阈值的目标框";
}

function renderVisionModel(telemetry) {
  const primaryVision = telemetry.vision && typeof telemetry.vision === "object"
    ? telemetry.vision : {};
  const fallbackVision = telemetry.inference?.v &&
    typeof telemetry.inference.v === "object" ? telemetry.inference.v : {};
  const vision = Object.keys(primaryVision).length > 0 ? primaryVision : fallbackVision;
  const hasVision = Object.keys(vision).length > 0;
  const status = Number(vision.s ?? 0);
  const readyMask = Number(vision.r ?? 0);
  const readyCount = [1, 2, 4].filter(bit => (readyMask & bit) !== 0).length;
  const timings = Array.isArray(vision.t) ? vision.t.map(value => Number(value || 0)) : [];
  const errors = Array.isArray(vision.e) ? vision.e.map(value => Number(value || 0)) : [];
  const sceneScores = Array.isArray(vision.ss) ? vision.ss : [];
  const occlusion = Array.isArray(vision.oc) ? vision.oc : [];
  const occlusionConfidence = Number(occlusion[1] || 0);
  const occlusionValid = Number(vision.ov ?? (occlusionConfidence > 0 ? 1 : 0)) !== 0;
  const behavior = Array.isArray(vision.bh) ? vision.bh : [];
  const detections = Array.isArray(vision.d) ? vision.d : [];
  const statusText = hasVision ? (visionStatusLabels[status] || `未知状态 ${status}`) : "等待视觉遥测";
  const statusTone = [7, 8, 9, 10].includes(status) ? "danger"
    : status === 6 ? "ok" : hasVision ? "warning" : "";

  $("vision-live-state").textContent = statusText;
  statusClass($("vision-live-state"), statusTone);
  $("vision-pipeline-state").textContent = statusText;
  statusClass($("vision-pipeline-state"), statusTone);
  $("vision-pipeline-detail").textContent = !hasVision
    ? "需要烧录带视觉链路的 CPU0 固件，并由网关转发 vision 字段"
    : status === 1
      ? "模型已启动；到“录制测试”拍摄一张即可触发推理"
      : `视觉状态序列 ${vision.q ?? 0} · 最近帧 ${vision.f ?? 0}`;
  $("vision-model-ready").textContent = `${readyCount} / 3（掩码 ${readyMask}）`;
  $("vision-frame-count").textContent = `${vision.f ?? 0} / ${vision.n ?? 0}`;
  $("vision-image-size").textContent = Number(vision.w || 0) && Number(vision.h || 0)
    ? `${vision.w}×${vision.h} · ${formatBytes(vision.jb || 0)}` : "-";
  $("vision-ospi-state").textContent = Number(vision.os || 0)
    ? `错误 ${vision.os}` : readyMask ? `正常 · 加载 ${vision.ml ?? 0} ms` : "等待初始化";

  $("vision-scene-scores").innerHTML = visionScoreRows(visionSceneLabels, sceneScores);
  const occlusionClass = Number(occlusion[0] ?? 3);
  $("vision-occlusion-class").textContent = occlusionValid
    ? (visionOcclusionLabels[occlusionClass] || `类别 ${occlusionClass}`)
    : "视觉证据不足";
  $("vision-occlusion-detail").textContent = occlusionValid
    ? `置信度 ${(occlusionConfidence / 10).toFixed(1)}%`
    : "未获得有效头部 ROI，不据此触发口鼻告警";
  statusClass($("vision-occlusion-class"), !occlusionValid ? ""
    : occlusionClass === 2 ? "danger"
    : [1, 3].includes(occlusionClass) ? "warning" : "");
  $("vision-occlusion-scores").innerHTML =
    visionScoreRows(visionOcclusionLabels, occlusion.slice(2));

  const behaviorClass = Number(behavior[0] ?? 4);
  const behaviorConfidence = Number(behavior[1] || 0);
  $("vision-behavior-class").textContent =
    visionBehaviorLabels[behaviorClass] || `类别 ${behaviorClass}`;
  $("vision-behavior-detail").textContent = `置信度 ${(behaviorConfidence / 10).toFixed(1)}%`;
  statusClass($("vision-behavior-class"), behaviorClass === 3 ? "warning" : "");
  $("vision-behavior-scores").innerHTML =
    visionScoreRows(visionBehaviorLabels, behavior.slice(2));

  $("vision-detection-count").textContent = `${vision.dc ?? detections.length} 个`;
  renderVisionBoxes(vision, detections, hasVision);
  $("vision-detection-rows").innerHTML = detections.length
    ? detections.map(item => {
      const values = Array.isArray(item) ? item : [];
      const classId = Number(values[0] ?? -1);
      return `<tr><td>${esc(visionSceneLabels[classId] || `类别 ${classId}`)}</td>`
        + `<td>${(Number(values[1] || 0) / 10).toFixed(1)}%</td>`
        + `<td>${Number(values[2] || 0)}</td><td>${Number(values[3] || 0)}</td>`
        + `<td>${Number(values[4] || 0)}</td><td>${Number(values[5] || 0)}</td></tr>`;
    }).join("")
    : `<tr><td colspan="6">${hasVision ? "当前帧没有超过阈值的目标框" : "等待第一帧"}</td></tr>`;

  $("vision-total-time").textContent = timings[5] == null ? "-" : `${timings[5]} ms`;
  $("vision-time-decode").textContent = timings[0] == null ? "-" : `${timings[0]} ms`;
  $("vision-time-preprocess").textContent = timings[1] == null ? "-" : `${timings[1]} ms`;
  $("vision-time-scene").textContent = timings[2] == null ? "-" : `${timings[2]} ms`;
  $("vision-time-occlusion").textContent = timings[3] == null ? "-" : `${timings[3]} ms`;
  $("vision-time-behavior").textContent = timings[4] == null ? "-" : `${timings[4]} ms`;
  $("vision-error-count").textContent = errors.length
    ? `JPEG ${errors[0]} / NPU ${errors[1]} / 丢帧 ${errors[2]}` : "-";
  $("vision-fusion-state").textContent = Number(vision.fe || 0)
    ? "视觉结果已允许进入融合层。"
    : "原始视觉结果尚未接入融合告警。完成跨场次验证后再启用融合，避免弱数据直接触发安全告警。";
}

function renderResourceRecording() {
  const session = state.snapshot?.resource_recording || null;
  const active = session?.status === "active";
  const count = Number(session?.sample_count || 0);
  let text = session ? `${active ? "记录中" : "上次记录已停止"} · ${count} 个样本` : "性能参数未记录";
  if (active && session.started_at) {
    const seconds = Math.max(0, Math.floor((Date.now() - new Date(session.started_at).getTime()) / 1000));
    text += ` · ${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
  }
  $("resource-recording-state").textContent = text;
  statusClass($("resource-recording-state"), active ? "ok" : "");
  $("resource-recording-start").disabled = !state.device || active || state.resourceRecordingPending;
  $("resource-recording-stop").disabled = !active || state.resourceRecordingPending;
  const download = $("resource-recording-download");
  download.hidden = !session?.download_url;
  if (session?.download_url) download.href = session.download_url;
}

async function controlResourceRecording(action) {
  if (!state.device || state.resourceRecordingPending) return;
  state.resourceRecordingPending = true;
  renderResourceRecording();
  try {
    const result = await api(`/api/dashboard/resource-recordings/${action}`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({device_id: state.device}),
    });
    state.snapshot.resource_recording = result.session || null;
    renderResourceRecording();
    renderCollections();
  } catch (error) {
    $("resource-recording-state").textContent = `操作失败：${error.message}`;
    statusClass($("resource-recording-state"), "danger");
  } finally {
    state.resourceRecordingPending = false;
    renderResourceRecording();
  }
}

function renderCryModel(telemetry) {
  const inference = telemetry.inference || {};
  const microphone = telemetry.sensors?.microphone || {};
  const link = telemetry.health?.ra8_link || {};
  const cryReady = Number(inference.cry_ready || 0) === 1;
  const typeReady = Number(inference.cry_type_ready || 0) === 1;
  const cryConfirmed = Number(inference.cry_ok || 0) === 1;
  const typeValid = Number(inference.cry_type_ok || 0) === 1;
  const raw = Math.max(0, Math.min(1000, Number(inference.cry_raw || 0)));
  const typeConfidence = Math.max(0, Math.min(1000, Number(inference.cry_type_conf || 0)));
  const typeMargin = Math.max(0, Math.min(1000, Number(inference.cry_type_margin || 0)));
  const typeName = typeValid ? String(inference.cry_type || "unknown") : "unknown";
  const readyCount = Number(cryReady) + Number(typeReady);
  const inputHealthy = Number(microphone.read || 0) > 0 && Number(microphone.err || 0) === 0;

  $("cry-live-state").textContent = readyCount === 2 ? "两个声音模型已就绪"
    : readyCount ? `声音模型 ${readyCount} / 2 就绪` : "等待声音模型";
  statusClass($("cry-live-state"), readyCount === 2 ? "ok" : readyCount ? "warning" : "");
  $("cry-pipeline-state").textContent = readyCount === 2 ? "推理运行中" : "模型未完全就绪";
  statusClass($("cry-pipeline-state"), readyCount === 2 ? "ok" : "warning");
  $("cry-pipeline-detail").textContent = `二分类窗口 ${inference.cry_n ?? 0} · 类型有效窗口 ${inference.cry_type_n ?? 0}/8`;
  $("cry-model-ready").textContent = `${readyCount} / 2`;
  $("cry-model-version").textContent = link.cry_fw || "未上报";
  $("cry-type-model-version").textContent = link.cry_type_fw || "未上报";
  $("cry-inference-count").textContent = `${inference.cry_n ?? 0} 次`;

  $("cry-model-class").textContent = !cryReady ? "等待模型"
    : cryConfirmed ? "检测到哭声" : "未确认哭声";
  $("cry-model-detail").textContent = cryReady
    ? `连续命中 ${inference.cry_run ?? 0} · 释放保持 ${inference.cry_hold ?? 0}`
    : "等待 RA8 哭声二分类模型";
  statusClass($("cry-model-class"), !cryReady ? "" : cryConfirmed ? "warning" : "ok");
  $("cry-binary-scores").innerHTML = visionScoreRows(
    ["非哭声", "哭声"], [1000 - raw, raw]);

  $("cry-type-model-class").textContent = typeReady
    ? (cryTypeLabels[typeName] || typeName) : "等待模型";
  $("cry-type-model-detail").textContent = typeReady
    ? `置信度 ${(typeConfidence / 10).toFixed(1)}% · 类间差 ${(typeMargin / 10).toFixed(1)}%`
    : "等待 RA8 哭声类型模型";
  statusClass($("cry-type-model-class"), typeValid ? "warning" : "");
  $("cry-type-model-scores").innerHTML = visionScoreRows(
    ["饥饿", "腹痛", "不适", "疲倦", "需拍嗝"],
    [inference.cry_type_hu, inference.cry_type_bp, inference.cry_type_dc,
      inference.cry_type_ti, inference.cry_type_bu]);

  $("cry-input-state").textContent = inputHealthy ? "采样正常"
    : Number(microphone.read || 0) > 0 ? "采样有错误" : "等待采样";
  statusClass($("cry-input-state"), inputHealthy ? "ok"
    : Number(microphone.err || 0) > 0 ? "danger" : "");
  $("cry-input-detail").textContent = `状态 ${microphone.s ?? "-"} · FSP ${microphone.fsp ?? "-"}`;
  $("cry-input-level").textContent = `${microphone.peak ?? "-"} / ${microphone.mean ?? "-"}`;
  $("cry-input-counters").textContent = `${microphone.read ?? 0} / ${microphone.err ?? 0}`;
  $("cry-inference-time").textContent = `${inference.cry_ms ?? 0} ms`;
}

function fusionComfort(inference) {
  const raw = Array.isArray(inference?.comfort) ? inference.comfort : [];
  const values = raw.slice(0, 5).map(value => Number(value || 0));
  while (values.length < 5) values.push(0);
  return {
    available: raw.length >= 5,
    actions: values[0], reasons: values[1], confidence: values[2],
    requiresConfirmation: values[3] === 1, blocks: values[4],
  };
}

function bitLabels(mask, labels) {
  return Object.entries(labels).filter(([bit]) => mask & Number(bit)).map(([, label]) => label);
}

function renderComfortRecommendation(inference) {
  const comfort = fusionComfort(inference);
  const container = document.querySelector(".comfort-advisory");
  const summary = $("comfort-recommendation");
  const detail = $("comfort-recommendation-detail");
  container.classList.remove("warning", "danger");
  if (!comfort.available) {
    summary.textContent = "等待融合协议 v7";
    detail.textContent = "建议仅供用户确认，不会自动驱动继电器";
    return;
  }

  const actions = [];
  if (comfort.actions & comfortActionBits.lightOn) actions.push("开启暖光灯");
  if (comfort.actions & comfortActionBits.lightOff) actions.push("关闭暖光灯");
  if (comfort.actions & comfortActionBits.pat) actions.push("拍一次安抚器");
  if (comfort.actions & comfortActionBits.caregiverCheck) actions.push("照护者立即查看");
  const reasons = bitLabels(comfort.reasons, comfortReasonLabels);
  const blocks = bitLabels(comfort.blocks, comfortBlockLabels);
  summary.textContent = actions.length ? actions.join(" · ") : "当前无需执行安抚动作";
  detail.textContent = `${reasons.length ? `依据：${reasons.join("、")}` : "没有活动建议依据"}`
    + ` · 置信度 ${comfort.confidence}%`
    + (blocks.length ? ` · 不建议拍一拍：${blocks.join("、")}` : "")
    + (comfort.requiresConfirmation ? " · 需用户确认" : "");
  if (comfort.actions & comfortActionBits.caregiverCheck) container.classList.add("danger");
  else if (comfort.actions) container.classList.add("warning");
}

function renderFusionDecision(inference) {
  const contexts = Array.isArray(inference.ctx) ? inference.ctx : [];
  const alarm = Array.isArray(inference.alarm) ? inference.alarm : [];
  const incidents = Array.isArray(inference.ai) ? inference.ai : [];
  const evidence = Array.isArray(inference.ex) ? inference.ex : [];
  const severity = Number(inference.severity || 0);
  const fallbackLevel = severity === 0 ? 0 : severity === 1 ? 1 : severity <= 3 ? 2 : 3;
  const level = Number(alarm[0] ?? fallbackLevel);
  const primaryType = Number(alarm[1] ?? 0);
  const activeCount = Number(alarm[4] ?? incidents.length);

  renderComfortRecommendation(inference);

  $("alarm-level").textContent = alarmLevelLabels[level] || `L${level}`;
  statusClass($("alarm-level"), level >= 3 ? "danger" : level >= 1 ? "warning" : "ok");
  $("alarm-primary").textContent = primaryType
    ? `主要事件：${incidentLabels[primaryType] || `事件 ${primaryType}`}` : "无活动事件";
  $("incident-count").textContent = String(activeCount);
  $("incident-summary").textContent = incidents.length
    ? incidents.slice(0, 3).map(item => incidentLabels[Number(item?.[0])] || `事件 ${item?.[0]}`).join(" · ")
    : "系统状态稳定";
  statusClass($("incident-count"), level >= 3 ? "danger" : level >= 1 ? "warning" : "ok");
  $("fusion-schema").textContent = `融合协议 v${Number(inference.sv || 1)}`;
  $("fusion-evidence-sequence").textContent = `证据 ${Number(inference.ev || 0)}`;

  $("fusion-contexts").innerHTML = fusionContextNames.map((name, index) => {
    const code = Number(contexts[index] ?? 0);
    const label = fusionContextLabels[index]?.[code] || `状态 ${code}`;
    const warning = (index === 3 && code >= 2) || (index === 4 && code >= 2)
      || (index === 5 && code >= 2) || (index === 10 && code >= 1);
    const danger = (index === 3 && code >= 3) || (index === 4 && code >= 3)
      || (index === 5 && code >= 3) || (index === 10 && code >= 2);
    return `<div class="fusion-context-item ${danger ? "danger" : warning ? "warning" : ""}">`
      + `<span>${esc(name)}</span><strong>${esc(label)}</strong></div>`;
  }).join("");

  $("fusion-incidents").innerHTML = incidents.length ? incidents.map(item => {
    const type = Number(item?.[0] ?? 0);
    const lifecycle = Number(item?.[1] ?? 0);
    const incidentLevel = Number(item?.[2] ?? 0);
    const confidence = Number(item?.[4] ?? 0);
    return `<div class="fusion-incident-item level-${incidentLevel}"><b>L${incidentLevel}</b>`
      + `<span>${esc(incidentLabels[type] || `事件 ${type}`)} · ${esc(incidentLifecycleLabels[lifecycle] || lifecycle)}</span>`
      + `<strong>${confidence}%</strong></div>`;
  }).join("") : "<p>无活动事件</p>";

  if (evidence.length >= 17) {
    const scoreRows = [
      ["综合在床", evidence[0]], ["压力在床", evidence[2]], ["视觉在床", evidence[3]],
      ["热源在床", evidence[4]], ["活动", evidence[5]], ["口鼻风险", evidence[6]],
      ["床缘风险", evidence[7]],
    ];
    $("fusion-presence-summary").textContent = `${Number(evidence[1] || 0)} 路阳性 · ${scoreRows[0][1] / 10}%`;
    $("fusion-evidence").innerHTML = scoreRows.map(([name, raw]) => {
      const value = Math.max(0, Math.min(1000, Number(raw || 0)));
      const warning = (name === "口鼻风险" || name === "床缘风险") && value >= 500;
      return `<div class="fusion-evidence-item ${warning ? "warning" : ""}"><span>${esc(name)}</span>`
        + `<i><em style="width:${(value / 10).toFixed(1)}%"></em></i><strong>${(value / 10).toFixed(1)}%</strong></div>`;
    }).join("") + `<div class="fusion-evidence-detail">热源 ${Number(evidence[8] || 0)} 点 · `
      + `位置 ${Number(evidence[9] || 0) / 10}%, ${Number(evidence[10] || 0) / 10}% · `
      + `均温 ${(Number(evidence[11] || 0) / 10).toFixed(1)}°C</div>`
      + `<div class="fusion-evidence-detail">床温 左/中/右 `
      + `${(Number(evidence[12] || 0) / 1000).toFixed(1)} / ${(Number(evidence[13] || 0) / 1000).toFixed(1)} / `
      + `${(Number(evidence[14] || 0) / 1000).toFixed(1)}°C · 温差 ${(Number(evidence[15] || 0) / 1000).toFixed(1)}°C</div>`
      + (evidence.length >= 21
        ? `<div class="fusion-evidence-detail">热源床面位置 ${(Number(evidence[17] || 0) / 10).toFixed(1)}% · `
          + `热床缘 ${(Number(evidence[18] || 0) / 10).toFixed(1)}% · `
          + `压/热位置一致 ${(Number(evidence[19] || 0) / 10).toFixed(1)}% · `
          + `标定 0x${Number(evidence[20] || 0).toString(16).toUpperCase().padStart(8, "0")}</div>`
        : "");
  } else {
    $("fusion-presence-summary").textContent = "等待 v5 数据";
    $("fusion-evidence").innerHTML = "<p>当前 RA8 固件未提供可解释融合证据</p>";
  }
}

function overlayMapPoint(matrix, x, y, clamp = true) {
  const denominator = matrix[6] * x + matrix[7] * y + matrix[8];
  if (!Number.isFinite(denominator) || Math.abs(denominator) < 1e-8) return null;
  const point = [
    (matrix[0] * x + matrix[1] * y + matrix[2]) / denominator,
    (matrix[3] * x + matrix[4] * y + matrix[5]) / denominator,
  ];
  return clamp ? point.map(value => Math.max(0, Math.min(1, value))) : point;
}

function thermalColor(ratio) {
  const value = Math.max(0, Math.min(1, ratio));
  const channel = offset => Math.round(255 * Math.max(0, Math.min(1, 1.5 - Math.abs(4 * value - offset))));
  return [channel(3), channel(2), channel(1)];
}

function liveVideoBounds() {
  const image = $("video-preview");
  const stage = $("video-stage");
  if (!image.classList.contains("ready") || !image.naturalWidth || !image.naturalHeight) return null;
  const scale = Math.min(stage.clientWidth / image.naturalWidth, stage.clientHeight / image.naturalHeight);
  const width = image.naturalWidth * scale;
  const height = image.naturalHeight * scale;
  return {left: (stage.clientWidth - width) / 2, top: (stage.clientHeight - height) / 2, width, height};
}

function positionLiveOverlay(element, bounds) {
  element.style.left = `${bounds.left}px`;
  element.style.top = `${bounds.top}px`;
  element.style.width = `${bounds.width}px`;
  element.style.height = `${bounds.height}px`;
}

function renderLiveThermalOverlay(bounds = liveVideoBounds()) {
  const canvas = $("live-thermal-overlay");
  const calibration = state.snapshot?.overlay_calibration?.thermal || {};
  const current = state.snapshot?.matrices?.thermal;
  const matrix = state.thermalDisplay.lastGoodMatrix || current;
  const visionOverlay = state.snapshot?.vision_overlay || {};
  const babyBoxes = (visionOverlay.boxes || []).filter(box => {
    const classId = Number(box.class_id);
    return (classId === 0 || classId === 1) && Number(box.width) > 0 && Number(box.height) > 0;
  });
  const matrixAge = Date.now() - Date.parse(matrix?.received_at || "");
  if (!bounds || !calibration.enabled || !matrix?.values?.length
      || visionOverlay.stale || visionOverlay.expired || !babyBoxes.length
      || (Number.isFinite(matrixAge) && matrixAge > 3000)) {
    canvas.classList.remove("ready");
    state.thermalOverlay = {
      matchedCells: 0, threshold: null,
      reason: !calibration.enabled ? "disabled" : !babyBoxes.length ? "no_baby_box"
        : visionOverlay.stale || visionOverlay.expired ? "vision_stale" : "thermal_stale",
    };
    return state.thermalOverlay;
  }
  const rows = Number(matrix.rows || 0);
  const cols = Number(matrix.cols || 0);
  const values = matrix.values.map(Number);
  const transform = Array.isArray(calibration.matrix) && calibration.matrix.length === 9
    ? calibration.matrix.map(Number) : null;
  if (!rows || !cols || values.length !== rows * cols || !transform?.every(Number.isFinite)) {
    canvas.classList.remove("ready");
    state.thermalOverlay = {matchedCells: 0, threshold: null, reason: "invalid_matrix"};
    return state.thermalOverlay;
  }

  positionLiveOverlay(canvas, bounds);
  canvas.width = Math.max(1, Math.round(bounds.width));
  canvas.height = Math.max(1, Math.round(bounds.height));
  const mappedCenters = values.map((_, index) => overlayMapPoint(
    transform,
    ((index % cols) + 0.5) / cols,
    (Math.floor(index / cols) + 0.5) / rows,
    false,
  ));
  const allowed = mappedCenters.map(point => point && babyBoxes.some(box => {
    const margin = 0.025;
    return point[0] >= Number(box.x) - margin
      && point[0] <= Number(box.x) + Number(box.width) + margin
      && point[1] >= Number(box.y) - margin
      && point[1] <= Number(box.y) + Number(box.height) + margin;
  }));
  const candidates = thermalCandidateMask(values, rows, cols, allowed, {
    percentile: 0.70,
    minimumDelta: 5,
    minimumComponent: 2,
  });
  if (!candidates.count) {
    canvas.classList.remove("ready");
    state.thermalOverlay = {matchedCells: 0, threshold: candidates.threshold, reason: "no_thermal_match"};
    return state.thermalOverlay;
  }

  const range = robustRange(values);
  const span = Math.max(1, range.maximum - range.minimum);
  const context = canvas.getContext("2d");
  context.setTransform(1, 0, 0, 1, 0, 0);
  context.clearRect(0, 0, canvas.width, canvas.height);
  context.globalAlpha = Math.max(0.05, Math.min(0.8, Number(calibration.opacity || 0.32)));
  candidates.mask.forEach((visible, index) => {
    if (!visible) return;
    const row = Math.floor(index / cols);
    const col = index % cols;
    const corners = [
      overlayMapPoint(transform, col / cols, row / rows, false),
      overlayMapPoint(transform, (col + 1) / cols, row / rows, false),
      overlayMapPoint(transform, (col + 1) / cols, (row + 1) / rows, false),
      overlayMapPoint(transform, col / cols, (row + 1) / rows, false),
    ];
    if (corners.some(point => !point)) return;
    const [red, green, blue] = thermalColor((values[index] - range.minimum) / span);
    context.fillStyle = `rgb(${red} ${green} ${blue})`;
    context.beginPath();
    context.moveTo(corners[0][0] * canvas.width, corners[0][1] * canvas.height);
    corners.slice(1).forEach(point => context.lineTo(point[0] * canvas.width, point[1] * canvas.height));
    context.closePath();
    context.fill();
  });
  context.globalAlpha = 1;
  canvas.classList.add("ready");
  state.thermalOverlay = {
    matchedCells: candidates.count,
    threshold: candidates.threshold,
    reason: "matched",
  };
  return state.thermalOverlay;
}

function buildBrowserVisionOverlay(telemetry, calibration) {
  const vision = telemetry?.vision || {};
  const inference = telemetry?.inference || {};
  const matrix = Array.isArray(calibration?.matrix) && calibration.matrix.length === 9
    ? calibration.matrix.map(Number) : [1, 0, 0, 0, 1, 0, 0, 0, 1];
  const width = Math.max(0, Number(vision.w || 0));
  const height = Math.max(0, Number(vision.h || 0));
  const detections = Array.isArray(vision.d) ? vision.d : [];
  const boxes = [];
  if (calibration?.enabled !== false && width && height) detections.slice(0, 8).forEach(raw => {
    if (!Array.isArray(raw) || raw.length < 6) return;
    const [classId, score, x, y, boxWidth, boxHeight] = raw.map(Number);
    if (!(boxWidth > 0 && boxHeight > 0)) return;
    const points = [
      overlayMapPoint(matrix, x / width, y / height),
      overlayMapPoint(matrix, (x + boxWidth) / width, y / height),
      overlayMapPoint(matrix, (x + boxWidth) / width, (y + boxHeight) / height),
      overlayMapPoint(matrix, x / width, (y + boxHeight) / height),
    ];
    if (points.some(point => !point)) return;
    const xs = points.map(point => point[0]);
    const ys = points.map(point => point[1]);
    const left = Math.min(...xs), top = Math.min(...ys);
    boxes.push({
      class_id: classId, score_x1000: score, x: left, y: top,
      width: Math.max(0, Math.max(...xs) - left), height: Math.max(0, Math.max(...ys) - top),
    });
  });
  const evidence = Array.isArray(inference.vv) ? inference.vv : [];
  const age = Number(evidence[1] || 0);
  return {
    enabled: calibration?.enabled !== false,
    calibration_status: calibration?.status || "identity_unverified",
    stale: Number(vision.q || 0) === 0 || age > visionOverlayStaleMs,
    expired: Number(vision.q || 0) === 0 || age > visionOverlayExpireMs,
    source_frame: { sequence: Number(vision.q || 0), age_ms: age },
    boxes,
  };
}

function layoutLiveVisionOverlay() {
  const layer = $("live-vision-overlay");
  const thermalLayer = $("live-thermal-overlay");
  const overlay = state.snapshot?.vision_overlay || {};
  const bounds = liveVideoBounds();
  if (!bounds) {
    layer.classList.remove("ready");
    thermalLayer.classList.remove("ready");
    return state.thermalOverlay;
  }
  const thermalStatus = renderLiveThermalOverlay(bounds);
  if (!overlay.enabled || overlay.expired) {
    layer.classList.remove("ready");
    return thermalStatus;
  }
  positionLiveOverlay(layer, bounds);
  const colors = ["#4dd0a8", "#56a8ff", "#ffca5c", "#e98bff", "#ff7c7c"];
  layer.innerHTML = (overlay.boxes || []).map(box => {
    const classId = Number(box.class_id ?? -1);
    const color = colors[classId] || "#f2f5f6";
    return `<i class="live-detection-box" style="--box-color:${color};left:${Number(box.x) * 100}%;top:${Number(box.y) * 100}%;width:${Number(box.width) * 100}%;height:${Number(box.height) * 100}%">`
      + `<span>${esc(visionSceneLabels[classId] || `类别 ${classId}`)} ${(Number(box.score_x1000 || 0) / 10).toFixed(1)}%</span></i>`;
  }).join("");
  layer.classList.toggle("stale", Boolean(overlay.stale));
  layer.classList.add("ready");
  return thermalStatus;
}

function renderLiveVisionOverlay() {
  const overlay = state.snapshot?.vision_overlay || {};
  const runtime = $("overlay-runtime");
  const thermalStatus = layoutLiveVisionOverlay();
  runtime.classList.toggle("stale", Boolean(overlay.stale));
  if (!overlay.enabled) runtime.textContent = "检测框已关闭";
  else if (overlay.expired) runtime.textContent = "检测框超过 30 秒，等待新推理";
  else if (overlay.stale) runtime.textContent = `显示上次检测框 · ${(Number(overlay.source_frame?.age_ms || 0) / 1000).toFixed(1)} 秒前`;
  else if (!(overlay.boxes || []).length) runtime.textContent = "本帧未检出目标";
  else runtime.textContent = `${overlay.boxes.length} 个检测框 · ${overlay.calibration_status === "calibrated" ? "已校准" : "同坐标"}`;
  if (state.snapshot?.overlay_calibration?.thermal?.enabled) {
    runtime.textContent += thermalStatus?.matchedCells
      ? ` · 热融合 ${thermalStatus.matchedCells} 点 ≥ ${(Number(thermalStatus.threshold) * 0.1).toFixed(1)} °C`
      : " · 热融合等待视觉与温区共同命中";
  }
}

function renderOverlayCalibration(calibration) {
  calibration ||= { enabled: true, mode: "identity", source_points: [[0, 0], [1, 0], [1, 1], [0, 1]], target_points: [[0, 0], [1, 0], [1, 1], [0, 1]] };
  const key = `${state.device}:${JSON.stringify(calibration)}`;
  if (state.calibrationKey === key) return;
  state.calibrationKey = key;
  $("overlay-enabled").checked = calibration.enabled !== false;
  $("overlay-mode").value = calibration.mode || "identity";
  $("overlay-calibration-status").textContent = calibration.status === "calibrated" ? "四点映射已校准" : "同坐标预览";
  const source = calibration.source_points || [[0, 0], [1, 0], [1, 1], [0, 1]];
  const target = calibration.target_points || [[0, 0], [1, 0], [1, 1], [0, 1]];
  $("overlay-point-rows").innerHTML = [0, 1, 2, 3].map(index => `<tr><td>P${index + 1}</td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-overlay-point="${index}" data-overlay-side="source" data-overlay-axis="0" value="${Number(source[index]?.[0] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-overlay-point="${index}" data-overlay-side="source" data-overlay-axis="1" value="${Number(source[index]?.[1] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-overlay-point="${index}" data-overlay-side="target" data-overlay-axis="0" value="${Number(target[index]?.[0] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-overlay-point="${index}" data-overlay-side="target" data-overlay-axis="1" value="${Number(target[index]?.[1] ?? 0)}"></td></tr>`).join("");
  const thermal = calibration.thermal || {
    enabled: false, opacity: 0.32,
    source_points: [[0.25, 0.25], [0.75, 0.75]],
    target_points: [[0.25, 0.25], [0.75, 0.75]],
  };
  $("thermal-overlay-enabled").checked = thermal.enabled === true;
  $("thermal-overlay-opacity").value = Number(thermal.opacity || 0.32);
  $("thermal-overlay-opacity-value").value = `${Math.round(Number(thermal.opacity || 0.32) * 100)}%`;
  const thermalSource = thermal.source_points || [[0.25, 0.25], [0.75, 0.75]];
  const thermalTarget = thermal.target_points || [[0.25, 0.25], [0.75, 0.75]];
  $("thermal-overlay-point-rows").innerHTML = [0, 1].map(index => `<tr><td>T${index + 1}</td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-thermal-point="${index}" data-thermal-side="source" data-thermal-axis="0" value="${Number(thermalSource[index]?.[0] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-thermal-point="${index}" data-thermal-side="source" data-thermal-axis="1" value="${Number(thermalSource[index]?.[1] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-thermal-point="${index}" data-thermal-side="target" data-thermal-axis="0" value="${Number(thermalTarget[index]?.[0] ?? 0)}"></td>`
    + `<td><input type="number" min="0" max="1" step="0.001" data-thermal-point="${index}" data-thermal-side="target" data-thermal-axis="1" value="${Number(thermalTarget[index]?.[1] ?? 0)}"></td></tr>`).join("");
}

function clearWarmLightPending() {
  clearTimeout(state.warmLight.timeout);
  state.warmLight.pending = false;
  state.warmLight.target = null;
  state.warmLight.device = "";
  state.warmLight.commandId = null;
  state.warmLight.requestId = "";
  state.warmLight.timeout = null;
}

function ra8CommandLinkState(telemetry) {
  const health = telemetry?.health || {};
  const gateway = health.gateway || {};
  const link = health.ra8_link || {};
  const readyTx = Number(health.ready_tx_count ?? gateway.ready_tx_count ?? 0);
  const readyRx = Number(link.ready_n ?? 0);
  const readyAge = Number(link.ready_age ?? 0xFFFFFFFF);
  const ready = Number(link.ready ?? 0) === 1;
  return {
    ready,
    readyTx,
    readyRx,
    readyAge,
    failed: health.ra8_online === true && readyTx >= 3 && !ready,
  };
}

function renderRa8CommandLinkMessage(element, commandLink) {
  if (commandLink.ready) {
    if (element.dataset.linkFault === "1") element.textContent = "";
    delete element.dataset.linkFault;
    return;
  }
  element.dataset.linkFault = "1";
  element.textContent = commandLink.failed
    ? `无法下发：P4 GPIO7 TX → RA8 P602 RX 未握手（${commandLink.readyTx}/${commandLink.readyRx}）`
    : `等待反向命令链路握手（${commandLink.readyTx}/${commandLink.readyRx}）`;
  statusClass(element, commandLink.failed ? "danger" : "warning");
}

function renderWarmLight(telemetry, device) {
  const light = telemetry.sensors?.warm_light || {};
  const toggle = $("warm-light-toggle");
  const status = $("warm-light-state");
  const detail = $("warm-light-detail");
  const result = $("warm-light-result");
  const stateCode = Number(light.s);
  const ready = stateCode === 1;
  const actualOn = Number(light.on) === 1;
  const ra8Online = telemetry.health?.ra8_online === true;
  const commandLink = ra8CommandLinkState(telemetry);
  const pending = state.warmLight.pending && state.warmLight.device === state.device;

  if (pending && ready && actualOn === state.warmLight.target) {
    const commandId = state.warmLight.commandId;
    const target = state.warmLight.target;
    clearWarmLightPending();
    result.textContent = `${target ? "开启" : "关闭"}已由设备确认${commandId ? ` · 命令 ${commandId}` : ""}`;
    statusClass(result, "ok");
  }

  const stillPending = state.warmLight.pending && state.warmLight.device === state.device;
  toggle.checked = stillPending ? Boolean(state.warmLight.target) : actualOn;
  toggle.disabled = !device.online || !ra8Online || !ready || !commandLink.ready || stillPending;
  toggle.setAttribute("aria-busy", stillPending ? "true" : "false");

  if (stillPending) {
    status.textContent = state.warmLight.target ? "正在开启" : "正在关闭";
    statusClass(status, "warning");
  } else if (!device.online) {
    status.textContent = "设备离线";
    statusClass(status, "offline");
  } else if (!ra8Online) {
    status.textContent = "RA8 离线";
    statusClass(status, "danger");
  } else if (stateCode === 2) {
    status.textContent = "GPIO 故障";
    statusClass(status, "danger");
  } else if (!ready) {
    status.textContent = "未上报";
    statusClass(status, "warning");
  } else if (!commandLink.ready) {
    status.textContent = commandLink.failed ? "反向链路故障" : "等待反向握手";
    statusClass(status, commandLink.failed ? "danger" : "warning");
  } else {
    status.textContent = actualOn ? "已开启" : "已关闭";
    statusClass(status, "ok");
  }

  detail.textContent = ready
    ? `P${light.pin ?? 711} · 命令序列 ${light.cmd_seq ?? 0} · 错误 ${light.err ?? 0} · ready ${commandLink.readyTx}/${commandLink.readyRx}`
    : stateCode === 2
      ? `P${light.pin ?? 711} · FSP ${light.fsp ?? "-"} · 错误 ${light.err ?? 0}`
      : "P711 · 等待 RA8 上报";

  if (!stillPending && device.online && ra8Online && ready) {
    renderRa8CommandLinkMessage(result, commandLink);
  }
}

function warmLightConfirmationTimeout(requestId) {
  if (!state.warmLight.pending || state.warmLight.requestId !== requestId) return;
  const target = state.warmLight.target;
  const commandId = state.warmLight.commandId;
  clearWarmLightPending();
  const result = $("warm-light-result");
  const commandLink = ra8CommandLinkState(state.snapshot?.telemetry || {});
  result.textContent = commandLink.ready
    ? `${target ? "开启" : "关闭"}命令${commandId ? ` ${commandId}` : ""}未收到设备状态确认`
    : `命令${commandId ? ` ${commandId}` : ""}仅送达网关；P4 GPIO7 TX → RA8 P602 RX 未握手`;
  statusClass(result, "warning");
  renderWarmLight(state.snapshot?.telemetry || {},
    state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  refresh().catch(console.error);
}

async function setWarmLight(on) {
  if (!state.device || state.warmLight.pending) return;
  const commandLink = ra8CommandLinkState(state.snapshot?.telemetry || {});
  if (!commandLink.ready) {
    renderRa8CommandLinkMessage($("warm-light-result"), commandLink);
    renderWarmLight(state.snapshot?.telemetry || {},
      state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
    return;
  }
  const requestId = `web-light-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
  state.warmLight.pending = true;
  state.warmLight.target = Boolean(on);
  state.warmLight.device = state.device;
  state.warmLight.commandId = null;
  state.warmLight.requestId = requestId;
  $("warm-light-result").textContent = "正在下发";
  statusClass($("warm-light-result"), "warning");
  renderWarmLight(state.snapshot?.telemetry || {},
    state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  state.warmLight.timeout = setTimeout(() => warmLightConfirmationTimeout(requestId), 8000);

  try {
    const response = await api(`/api/mobile/v1/devices/${encodeURIComponent(state.device)}/commands`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({action: "light.set", args: {on: Boolean(on)}, request_id: requestId}),
    });
    if (!state.warmLight.pending || state.warmLight.requestId !== requestId) return;
    state.warmLight.commandId = response.command?.id ?? null;
    $("warm-light-result").textContent = `命令 ${state.warmLight.commandId ?? "-"} 已排队，等待设备确认`;
    scheduleRefresh();
  } catch (error) {
    if (state.warmLight.requestId !== requestId) return;
    clearWarmLightPending();
    $("warm-light-result").textContent = `下发失败：${error.message}`;
    statusClass($("warm-light-result"), "danger");
    renderWarmLight(state.snapshot?.telemetry || {},
      state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  }
}

function clearSootherPatPending() {
  clearTimeout(state.sootherPat.timeout);
  state.sootherPat.pending = false;
  state.sootherPat.target = null;
  state.sootherPat.device = "";
  state.sootherPat.commandId = null;
  state.sootherPat.requestId = "";
  state.sootherPat.timeout = null;
}

function renderSootherPat(telemetry, device) {
  const soother = telemetry.sensors?.soother || {};
  const toggle = $("soother-toggle");
  const status = $("soother-pat-state");
  const detail = $("soother-pat-detail");
  const result = $("soother-pat-result");
  const stateCode = Number(soother.s);
  const ready = stateCode === 1;
  const actualOn = Number(soother.on) === 1;
  const ra8Online = telemetry.health?.ra8_online === true;
  const commandLink = ra8CommandLinkState(telemetry);
  const pending = state.sootherPat.pending && state.sootherPat.device === state.device;

  if (pending && ready && actualOn === state.sootherPat.target) {
    const commandId = state.sootherPat.commandId;
    const target = state.sootherPat.target;
    clearSootherPatPending();
    result.textContent = `${target ? "开启" : "关闭"}已由设备确认${commandId ? ` · 命令 ${commandId}` : ""}`;
    statusClass(result, "ok");
  }

  const stillPending = state.sootherPat.pending && state.sootherPat.device === state.device;
  toggle.checked = stillPending ? Boolean(state.sootherPat.target) : actualOn;
  toggle.disabled = !device.online || !ra8Online || !ready || !commandLink.ready || stillPending;
  toggle.setAttribute("aria-busy", stillPending ? "true" : "false");

  if (stillPending) {
    status.textContent = state.sootherPat.target ? "正在开启" : "正在关闭";
    statusClass(status, "warning");
  } else if (!device.online) {
    status.textContent = "设备离线";
    statusClass(status, "offline");
  } else if (!ra8Online) {
    status.textContent = "RA8 离线";
    statusClass(status, "danger");
  } else if (stateCode === 2) {
    status.textContent = "GPIO 故障";
    statusClass(status, "danger");
  } else if (!ready) {
    status.textContent = "未上报";
    statusClass(status, "warning");
  } else if (!commandLink.ready) {
    status.textContent = commandLink.failed ? "反向链路故障" : "等待反向握手";
    statusClass(status, commandLink.failed ? "danger" : "warning");
  } else {
    status.textContent = actualOn ? "已开启" : "已关闭";
    statusClass(status, "ok");
  }

  detail.textContent = ready
    ? `P${soother.pin ?? 914} · 命令序列 ${soother.cmd_seq ?? 0} · 错误 ${soother.err ?? 0} · ready ${commandLink.readyTx}/${commandLink.readyRx}`
    : stateCode === 2
      ? `P${soother.pin ?? 914} · FSP ${soother.fsp ?? "-"} · 错误 ${soother.err ?? 0}`
      : "P914 · 等待 RA8 上报";

  if (!stillPending && device.online && ra8Online && ready) {
    renderRa8CommandLinkMessage(result, commandLink);
  }
}

function sootherPatConfirmationTimeout(requestId) {
  if (!state.sootherPat.pending || state.sootherPat.requestId !== requestId) return;
  const target = state.sootherPat.target;
  const commandId = state.sootherPat.commandId;
  clearSootherPatPending();
  const result = $("soother-pat-result");
  const commandLink = ra8CommandLinkState(state.snapshot?.telemetry || {});
  result.textContent = commandLink.ready
    ? `${target ? "开启" : "关闭"}命令${commandId ? ` ${commandId}` : ""}未收到设备状态确认`
    : `命令${commandId ? ` ${commandId}` : ""}仅送达网关；P4 GPIO7 TX → RA8 P602 RX 未握手`;
  statusClass(result, "warning");
  renderSootherPat(state.snapshot?.telemetry || {},
    state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  refresh().catch(console.error);
}

async function setSoother(on) {
  if (!state.device || state.sootherPat.pending) return;
  const commandLink = ra8CommandLinkState(state.snapshot?.telemetry || {});
  if (!commandLink.ready) {
    renderRa8CommandLinkMessage($("soother-pat-result"), commandLink);
    renderSootherPat(state.snapshot?.telemetry || {},
      state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
    return;
  }
  const requestId = `web-soother-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
  state.sootherPat.pending = true;
  state.sootherPat.target = Boolean(on);
  state.sootherPat.device = state.device;
  state.sootherPat.commandId = null;
  state.sootherPat.requestId = requestId;
  $("soother-pat-result").textContent = "正在下发";
  statusClass($("soother-pat-result"), "warning");
  renderSootherPat(state.snapshot?.telemetry || {},
    state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  state.sootherPat.timeout = setTimeout(() => sootherPatConfirmationTimeout(requestId), 8000);

  try {
    const response = await api(`/api/mobile/v1/devices/${encodeURIComponent(state.device)}/commands`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({action: "soother.set", args: {on: Boolean(on)}, request_id: requestId}),
    });
    if (!state.sootherPat.pending || state.sootherPat.requestId !== requestId) return;
    state.sootherPat.commandId = response.command?.id ?? null;
    $("soother-pat-result").textContent = `命令 ${state.sootherPat.commandId ?? "-"} 已排队，等待设备确认`;
    scheduleRefresh();
  } catch (error) {
    if (state.sootherPat.requestId !== requestId) return;
    clearSootherPatPending();
    $("soother-pat-result").textContent = `下发失败：${error.message}`;
    statusClass($("soother-pat-result"), "danger");
    renderSootherPat(state.snapshot?.telemetry || {},
      state.snapshot?.devices?.find(item => item.device_id === state.device) || {});
  }
}

function renderEnvironmentThresholds(policy, liveEvaluation) {
  policy ||= {};
  const thresholds = policy.thresholds || {};
  const evaluation = liveEvaluation || policy.evaluation || {};
  const key = `${state.device}:${thresholds.co2_max_ppm}:${thresholds.temperature_max_milli_c}:`
    + `${thresholds.humidity_max_milli_percent}:${thresholds.humidity_min_milli_percent}:${thresholds.gas_min_ohm}:`
    + `${thresholds.body_temperature_max_deci_c}`;
  if (state.environmentThresholdKey !== key
      && !$('environment-threshold-form').contains(document.activeElement)) {
    state.environmentThresholdKey = key;
    $("threshold-co2").value = thresholds.co2_max_ppm ?? 2000;
    $("threshold-temperature").value = Number(thresholds.temperature_max_milli_c ?? 30000) / 1000;
    $("threshold-humidity").value = Number(thresholds.humidity_max_milli_percent ?? 70000) / 1000;
    $("threshold-humidity-min").value = Number(thresholds.humidity_min_milli_percent ?? 30000) / 1000;
    $("threshold-gas").value = thresholds.gas_min_ohm ?? 10000;
    $("threshold-body-temperature").value = Number(thresholds.body_temperature_max_deci_c ?? 375) / 10;
  }
  const exceeded = Array.isArray(evaluation.exceeded) ? evaluation.exceeded : [];
  const stateElement = $("environment-threshold-state");
  if (!evaluation.baby_in_bed) {
    stateElement.textContent = "婴儿不在床或尚未确认，不触发";
    statusClass(stateElement, "");
  } else if (evaluation.active) {
    const names = {co2: "CO₂", temperature: "环境温度", humidity: "湿度", gas_resistance: "气阻", body_temperature: "疑似发热"};
    stateElement.textContent = `已越限：${exceeded.map(item => names[item.name] || item.name).join("、")}`;
    statusClass(stateElement, "danger");
  } else {
    stateElement.textContent = "婴儿在床，当前未越限";
    statusClass(stateElement, "ok");
  }
}

async function saveEnvironmentThresholds(event) {
  event.preventDefault();
  if (!state.device) return;
  const result = $("environment-threshold-result");
  result.textContent = "正在保存";
  try {
    const response = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/environment-thresholds`, {
      method: "PUT",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({
        co2_max_ppm: Math.round(Number($("threshold-co2").value)),
        temperature_max_milli_c: Math.round(Number($("threshold-temperature").value) * 1000),
        humidity_max_milli_percent: Math.round(Number($("threshold-humidity").value) * 1000),
        humidity_min_milli_percent: Math.round(Number($("threshold-humidity-min").value) * 1000),
        gas_min_ohm: Math.round(Number($("threshold-gas").value)),
        body_temperature_max_deci_c: Math.round(Number($("threshold-body-temperature").value) * 10),
      }),
    });
    state.snapshot.environment_policy = response;
    state.environmentThresholdKey = "";
    renderEnvironmentThresholds(response, response.evaluation);
    result.textContent = response.hardware_synced
      ? "RA8 硬件阈值已确认"
      : `已下发命令 ${response.command_id ?? "-"}，等待 RA8 回报生效`;
    statusClass(result, response.hardware_synced ? "ok" : "");
  } catch (error) {
    result.textContent = `保存失败：${error.message}`;
    statusClass(result, "danger");
  }
}

function renderTelemetry() {
  if (!state.snapshot) return;
  const snapshot = state.snapshot;
  const device = snapshot.devices.find(item => item.device_id === state.device) || {};
  const telemetry = snapshot.telemetry || {};
  const health = telemetry.health || {};
  const inference = telemetry.inference || {};
  const esp32 = health.esp32 || {};
  const gateway = health.gateway || {};
  const telemetryTransport = {...(snapshot.telemetry_status || {}), ...gateway};
  const matrixTransport = {...(snapshot.matrix_status || {}), ...gateway};
  const link = health.ra8_link || {};
  const metadata = device.metadata || {};
  const environment = telemetry.sensors?.environment || {};
  const bme688 = bme688FromTelemetry(telemetry);
  const audioPlayer = telemetry.sensors?.audio_player || {};
  renderEnvironmentThresholds(snapshot.environment_policy, telemetry.environment_policy);

  const reportedVolume = Number(audioPlayer.vol);
  if (Number.isFinite(reportedVolume) && !state.speakerVolume.pending
      && document.activeElement !== $("speaker-volume")) {
    const volume = Math.max(0, Math.min(100, Math.round(reportedVolume)));
    $("speaker-volume").value = volume;
    $("speaker-volume-value").value = `${volume}%`;
  }

  renderWarmLight(telemetry, device);
  renderSootherPat(telemetry, device);

  renderVisionModel(telemetry);
  renderCryModel(telemetry);
  renderFusionDecision(inference);
  renderOverlayCalibration(snapshot.overlay_calibration);
  snapshot.vision_overlay = buildBrowserVisionOverlay(telemetry, snapshot.overlay_calibration);
  renderLiveVisionOverlay();

  $("device-state").textContent = device.online ? "在线" : "离线";
  statusClass($("device-state"), device.online ? "ok" : "danger");
  $("last-seen").textContent = formatTime(device.last_seen);
  const environmentPresent = Object.keys(environment).some(key => key !== "bme688");
  const environmentState = Number(environment.s ?? 0);
  const environmentCo2 = Number(environment.co2);
  const environmentTemperature = Number(environment.t_mc);
  const environmentHumidity = Number(environment.rh_mp);
  const environmentValuesOk = Number.isFinite(environmentCo2)
    && environmentCo2 >= 0 && environmentCo2 <= 40000
    && Number.isFinite(environmentTemperature)
    && environmentTemperature >= -45000 && environmentTemperature <= 130000
    && Number.isFinite(environmentHumidity)
    && environmentHumidity >= 0 && environmentHumidity <= 100000;
  const environmentFusionValid = Boolean(Number(inference.valid || 0) & (1 << 5));
  const environmentFault = Boolean(Number(inference.faults || 0) & (1 << 5))
    || [4, 5, 6, 7].includes(environmentState);
  const environmentOk = environmentState === 3 && environmentValuesOk
    && environmentFusionValid && !environmentFault;
  const environmentStatus = !environmentPresent || environmentState === 0
    ? "offline" : environmentFault ? "danger" : environmentOk ? "ok" : "warning";
  $("environment-co2").textContent = Number.isFinite(environmentCo2)
    ? `${environmentCo2} ppm` : "-";
  $("environment-co2-detail").textContent = environmentPresent
    ? `SCD41 · ${environmentStateLabels[environmentState] || `状态 ${environmentState}`} · 序列 ${environment.seq ?? "-"}`
    : "等待 SCD41 遥测";
  statusClass($("environment-co2"), environmentStatus);
  $("environment-climate").textContent = Number.isFinite(environmentTemperature)
    && Number.isFinite(environmentHumidity)
    ? `${(environmentTemperature / 1000).toFixed(2)} °C · ${(environmentHumidity / 1000).toFixed(1)} %RH`
    : "-";
  $("environment-climate-detail").textContent = environmentPresent
    ? `本轮发现新样本 ${Number(environment.ready || 0) ? "是" : "否"} · 最近有效 tick ${environment.last ?? "-"}`
    : "等待环境温湿度";
  statusClass($("environment-climate"), environmentStatus);
  const bme688Present = Object.keys(bme688).length > 0;
  const bme688State = Number(bme688.s ?? 0);
  const bme688Temperature = Number(bme688.t_mc);
  const bme688Humidity = Number(bme688.rh_mp);
  const bme688Pressure = Number(bme688.p_pa);
  const bme688Gas = Number(bme688.gas_ohm);
  const bme688Flags = Number(bme688.flags || 0);
  const bme688NewData = Boolean(bme688Flags & 0x01);
  const bme688GasValid = Boolean(bme688Flags & 0x02);
  const bme688HeaterStable = Boolean(bme688Flags & 0x04);
  const bme688Age = tickAgeMs(telemetry.ra8_tick, bme688.last);
  const bme688Stale = bme688Age != null && bme688Age > 15000;
  const bme688ClimateValuesOk = Number.isFinite(bme688Temperature)
    && bme688Temperature >= -40000 && bme688Temperature <= 85000
    && Number.isFinite(bme688Humidity)
    && bme688Humidity >= 0 && bme688Humidity <= 100000
    && Number.isFinite(bme688Pressure)
    && bme688Pressure >= 30000 && bme688Pressure <= 110000;
  const bme688GasValueOk = Number.isFinite(bme688Gas) && bme688Gas > 0;
  const bme688Fault = [4, 5, 6, 7].includes(bme688State)
    || (bme688State === 3 && (
      !bme688ClimateValuesOk || (bme688GasValid && !bme688GasValueOk)
    )) || bme688Stale;
  const bme688SampleOk = bme688State === 3 && bme688ClimateValuesOk
    && bme688.last != null && !bme688Stale;
  const bme688ClimateStatus = !bme688Present || bme688State === 0
    ? "offline" : bme688Fault ? "danger" : bme688SampleOk ? "ok" : "warning";
  const bme688GasStatus = bme688ClimateStatus !== "ok"
    ? bme688ClimateStatus
    : bme688GasValid && bme688HeaterStable && bme688GasValueOk ? "ok" : "warning";
  $("bme688-climate").textContent = Number.isFinite(bme688Temperature)
    && Number.isFinite(bme688Humidity)
    ? `${(bme688Temperature / 1000).toFixed(2)} °C · ${(bme688Humidity / 1000).toFixed(1)} %RH`
    : "-";
  $("bme688-climate-detail").textContent = bme688Present
    ? `${bme688StateLabels[bme688State] || `状态 ${bme688State}`} · 地址 ${
      Number.isFinite(Number(bme688.addr)) ? `0x${Number(bme688.addr).toString(16).toUpperCase()}` : "-"
    } · 最近有效 tick ${bme688.last ?? "-"}`
    : "等待 BME688 遥测";
  statusClass($("bme688-climate"), bme688ClimateStatus);
  $("bme688-pressure").textContent = Number.isFinite(bme688Pressure)
    ? `${(bme688Pressure / 100).toFixed(2)} hPa` : "-";
  $("bme688-pressure-detail").textContent = bme688Present
    ? `本轮新样本 ${bme688NewData ? "是" : "否"} · 样本年龄 ${
      bme688Age == null ? "-" : `${bme688Age} ms`
    } · 累计驱动错误 ${bme688.err ?? 0}`
    : "等待 BME688 遥测";
  statusClass($("bme688-pressure"), bme688ClimateStatus);
  $("bme688-gas").textContent = Number.isFinite(bme688Gas) && bme688Gas > 0
    ? `${(bme688Gas / 1000).toFixed(3)} kΩ` : "-";
  $("bme688-gas-detail").textContent = bme688Present
    ? `gas_valid ${bme688GasValid ? "是" : "否"} · heater_stable ${
      bme688HeaterStable ? "是" : "否"
    } · 原始电阻，未运行 BSEC，不是 VOC/IAQ`
    : "等待 BME688 原始气阻；不等同于 VOC/IAQ";
  statusClass($("bme688-gas"), bme688GasStatus);
  const alarmLevel = Number(Array.isArray(inference.alarm) ? inference.alarm[0] || 0 : 0);
  const primaryIncidentType = Number(Array.isArray(inference.alarm) ? inference.alarm[1] || 0 : 0);
  const primaryIncidentLifecycle = Number(Array.isArray(inference.alarm) ? inference.alarm[3] || 0 : 0);
  const preciseSafetyLabel = alarmLevel > 0 && primaryIncidentType > 0
    ? incidentLabels[primaryIncidentType] || `事件 ${primaryIncidentType}`
    : inference.label ?? fusionLabels[inference.state] ?? "等待推理";
  $("safety").textContent = primaryIncidentLifecycle === 4
    ? `${preciseSafetyLabel}（恢复观察）` : preciseSafetyLabel;
  statusClass($("safety"), alarmLevel >= 3 || Number(inference.severity) > 3
    ? "danger" : alarmLevel >= 1 || Number(inference.severity) > 0 ? "warning" : "ok");
  $("confidence").textContent = inference.confidence == null && inference.conf == null
    ? "-" : `置信度 ${inference.confidence ?? inference.conf}% · 级别 ${inference.severity ?? 0}`
      + (primaryIncidentType > 0 ? ` · ${incidentLifecycleLabels[primaryIncidentLifecycle] || ""}` : "");
  const cryReady = Number(inference.cry_ready || 0) === 1;
  const cryConfirmed = Number(inference.cry_ok || 0) === 1;
  const cryProbability = Number(inference.cry_raw || 0);
  $("cry-detection").textContent = !cryReady
    ? "等待模型" : cryConfirmed ? "检测到哭声" : "未确认哭声";
  $("cry-detection-detail").textContent = cryReady
    ? `当前 ${(cryProbability / 10).toFixed(1)}% · 最近 8 窗命中 ${inference.cry_run ?? 0}`
      + ` · 释放保持 ${inference.cry_hold ?? 0}`
    : "等待烧录带防抖模型的 RA8 固件";
  statusClass($("cry-detection"), !cryReady ? "" : cryConfirmed ? "warning" : "ok");

  const cryTypeReady = Number(inference.cry_type_ready || 0) === 1;
  const cryTypeValid = Number(inference.cry_type_ok || 0) === 1;
  const cryType = cryTypeValid ? String(inference.cry_type || "unknown") : "unknown";
  $("cry-type").textContent = cryTypeReady ? (cryTypeLabels[cryType] || cryType) : "等待模型";
  $("cry-type-detail").textContent = cryTypeReady
    ? `实验结果 · 有效窗口 ${inference.cry_type_n ?? 0}/8 · `
      + `置信 ${(Number(inference.cry_type_conf || 0) / 10).toFixed(1)}% · `
      + `差值 ${(Number(inference.cry_type_margin || 0) / 10).toFixed(1)}%`
    : "等待烧录带类型分类的 RA8 固件";
  statusClass($("cry-type"), cryTypeValid ? "warning" : "");
  $("local-ip").textContent = esp32.local_ip || metadata.local_ip || "等待设备上报";
  $("network-detail").textContent = `网关 ${esp32.gateway_ip || metadata.gateway_ip || "-"} · `
    + `RSSI ${telemetry.wifi_rssi ?? device.wifi_rssi ?? "-"} dBm`;
  $("observed-ip").textContent = device.ip_address || "-";
  $("mac-address").textContent = `MAC ${esp32.mac || metadata.mac || "-"}`;
  const ra8Online = health.ra8_online === true;
  $("ra8-state").textContent = ra8Online ? "在线" : "离线";
  statusClass($("ra8-state"), ra8Online ? "ok" : "danger");
  $("ra8-detail").textContent = `状态帧 ${health.uart_status_frames ?? "-"} · `
    + `全部帧 ${health.uart_valid_frames ?? "-"} · CRC ${health.uart_crc_errors ?? "-"} · `
    + `RA8 溢出 ${link.ovf ?? "-"}`;
  $("firmware").textContent = telemetry.firmware || device.firmware || "-";
  const resetReason = esp32.reset_reason ?? metadata.reset_reason;
  $("rssi").textContent = `ESP 堆 ${esp32.free_heap ?? "-"} · PSRAM ${esp32.free_psram ?? "-"} · `
    + `启动 ${esp32.boot_count ?? metadata.boot_count ?? "-"} · ${resetReasons[resetReason] || "复位待上报"}`;
  const rate = telemetryRate();
  $("telemetry-rate").textContent = rate == null ? "等待" : `${rate.toFixed(1)} Hz`;
  $("telemetry-age").textContent = ageText(telemetry.received_at);
  const isWss = gateway.transport === "wss";
  $("gateway-http").textContent = isWss
    ? (gateway.ws_connected ? "WSS 在线" : "WSS 断开")
    : (gateway.last_http_code == null ? "等待" : `HTTP ${gateway.last_http_code}`);
  $("gateway-detail").textContent = `${isWss ? "发送" : "最近"} ${gateway.last_http_ms ?? "-"} ms · `
    + `最大 ${gateway.max_http_ms ?? "-"} ms · 失败 ${gateway.http_failures ?? "-"}/${gateway.http_requests ?? "-"}`;
  const serverTelemetryConnected = telemetryTransport.telemetry_connected === true;
  $("server-telemetry-state").textContent = serverTelemetryConnected
    ? `${Number(telemetryTransport.telemetry_fps || 0).toFixed(1)} Hz` : "未连接";
  statusClass($("server-telemetry-state"), serverTelemetryConnected ? "ok" : "danger");
  $("server-telemetry-detail").textContent = `ACK P95 ${telemetryTransport.telemetry_ack_p95_ms ?? "-"} ms · `
    + `DB P95 ${telemetryTransport.telemetry_database_p95_ms ?? "-"} ms · `
    + `队列 ${telemetryTransport.telemetry_queue_depth ?? 0}/${telemetryTransport.telemetry_queue_limit ?? 2} · `
    + `丢帧 ${telemetryTransport.telemetry_drops ?? 0} · 重连 ${telemetryTransport.telemetry_reconnects ?? 0}`;
  const matrixConnected = matrixTransport.matrix_connected === true;
  $("server-matrix-state").textContent = matrixConnected
    ? `P ${Number(matrixTransport.pressure_fps || 0).toFixed(1)} / T ${Number(matrixTransport.thermal_fps || 0).toFixed(1)} Hz`
    : "未连接";
  statusClass($("server-matrix-state"), matrixConnected ? "ok" : "danger");
  $("server-matrix-detail").textContent = `ACK P/T ${matrixTransport.pressure_ack_p95_ms ?? "-"}/${matrixTransport.thermal_ack_p95_ms ?? "-"} ms · `
    + `队列 ${matrixTransport.pressure_queue_depth ?? 0}/${matrixTransport.thermal_queue_depth ?? 0} · `
    + `丢帧 ${matrixTransport.matrix_drops ?? 0} · 非法 ${matrixTransport.matrix_invalid_frames ?? 0} · `
    + `重连 ${matrixTransport.matrix_reconnects ?? 0}`;
  renderSensorDiagnostics(snapshot.sensor_diagnostics);
  renderParameters(telemetry);
}

function renderEventRows() {
  if (!state.snapshot) return;
  $("event-rows").innerHTML = (state.snapshot.events || []).map(event => {
    const payload = event.payload || {};
    const sources = payload.sources || payload.payload?.sources || [];
    return `<tr><td>${esc(formatTime(event.received_at))}</td><td>${esc(event.event_type)}</td>`
      + `<td>${esc(event.state)}</td><td>${event.severity}</td><td>${Number(event.confidence).toFixed(1)}%</td>`
      + `<td>${esc(Array.isArray(sources) ? sources.join(", ") : sources)}</td>`
      + `<td>${event.acknowledged ? "已确认" : `<button onclick="ack(${event.id})">确认</button>`}</td></tr>`;
  }).join("");
}

function renderCollections() {
  if (!state.snapshot) return;
  const snapshot = state.snapshot;
  renderEventRows();
  $("log-count").textContent = `${(snapshot.logs || []).length} 条`;
  $("log-rows").innerHTML = (snapshot.logs || []).map(log => `<tr><td>${esc(formatTime(log.received_at))}</td>`
    + `<td>${esc(log.level)}</td><td>${esc(log.component)}</td><td>${esc(log.message)}</td>`
    + `<td><code>${esc(compact(log.context))}</code></td></tr>`).join("");
  $("command-rows").innerHTML = (snapshot.commands || []).map(command => `<tr><td>${command.id}</td>`
    + `<td>${esc(formatTime(command.created_at))}</td><td><code>${esc(compact(command.command))}</code></td>`
    + `<td>${esc(command.status)}</td><td>${command.delivery_attempts ?? 0}</td>`
    + `<td><code>${esc(compact(command.result))}</code></td></tr>`).join("");
  const sessions = (snapshot.sessions || []).filter(session => !session.metadata?.device_id
    || session.metadata.device_id === state.device);
  document.querySelectorAll("#collection-channels article[data-kind]").forEach(article => {
    const kind = article.dataset.kind;
    const activeSession = sessions.find(session => session.status === "active"
      && session.metadata?.device_id === state.device
      && (session.metadata?.active_kinds || []).includes(kind));
    article.classList.toggle("active", Boolean(activeSession));
    let detail = activeSession
      ? `采集中 · ${activeSession.target_labels?.[0] || activeSession.name}` : "未采集";
    if (activeSession?.started_at) {
      const elapsedSeconds = Math.max(0, Math.floor((Date.now() - new Date(activeSession.started_at).getTime()) / 1000));
      detail += ` · ${Math.floor(elapsedSeconds / 60)}:${String(elapsedSeconds % 60).padStart(2, "0")}`;
    }
    if (kind === "audio" && activeSession) {
      const mic = get(snapshot, "telemetry.sensors.microphone") || {};
      const writerState = Number(mic.wst ?? -1);
      detail += ` · ${micWriterStates[writerState] || "等待设备状态"}`;
      if (Number(mic.werr || 0)) detail += ` (${mic.werr})`;
    }
    if (kind === "camera" && activeSession) {
      const camera = get(snapshot, "telemetry.sensors.camera") || {};
      const writerState = Number(camera.wst ?? -1);
      const effectiveWriterState = writerState === 1 && sdUsable ? 2 : writerState;
      const frameError = Number(camera.frame_err || 0);
      const writerError = Number(camera.werr || 0);
      const writerAckTimeout = Number(camera.fail_stage || 0) === 4 && Number(camera.fsp || 0) === 20;
      const blocked = [1, 3, 4, 7].includes(effectiveWriterState) || frameError !== 0 || writerError !== 0 || writerAckTimeout;
      if (blocked) {
        detail = `采集受阻 · ${activeSession.target_labels?.[0] || activeSession.name}`;
      }
      detail += ` · ${cameraWriterStates[effectiveWriterState] || "等待设备状态"}`;
      detail += ` · 已保存 ${camera.saved ?? 0}`;
      if (frameError || writerError) {
        detail += ` · 帧错误 ${frameError} · 文件错误 ${writerError}`;
        if (fatErrnoNames[writerError]) detail += `（${fatErrnoNames[writerError]}）`;
      }
      if (writerAckTimeout) detail += ` · JPEG 写入线程超时 ${camera.to ?? 0} 次`;
    }
    article.querySelector("small").textContent = detail;
    article.querySelector('[data-action="start"]').disabled = Boolean(activeSession);
    article.querySelector('[data-action="stop"]').disabled = !activeSession;
  });
  const statusNames = {draft: "未开始", active: "采集中", complete: "已完成"};
  $("session-list").innerHTML = sessions.map(session => {
    const active = session.metadata?.active_kinds || [];
    const workflow = session.metadata?.workflow || "legacy";
    const isModel = workflow === "model_dataset";
    const dataKind = session.metadata?.data_kind || "";
    const controls = active.map(kind => `<button class="ok" `
      + `onclick="sessionAction('${session.session_id}','stop','${kind}')">停止 ${collectionNames[kind] || kind}</button>`).join("");
    const labels = session.target_labels || [];
    const progress = isModel
      ? `SD 待导入 ${session.pending_artifact_count ?? 0} · 原文件 ${session.imported_artifact_count ?? 0} · `
        + `训练/验证/测试 ${session.train_count ?? 0}/${session.validation_count ?? 0}/${session.test_count ?? 0}`
      : `参考样本 ${session.sample_count ?? 0} · 场景已标记 ${session.labeled_count ?? 0}`;
    const importButton = isModel && modelDataKinds.has(dataKind)
      ? `<button onclick="openImport('${session.session_id}')">导入 SD 文件</button>` : "";
    return `<article class="session-card ${isModel ? "model-session" : "reference-session"}"><div>`
      + `<div class="session-title"><strong>${esc(session.name)}</strong><span class="workflow-badge ${isModel ? "model" : "reference"}">`
      + `${esc(workflowNames[workflow] || workflow)}</span></div><p>${esc(session.description || "无实验备注")}</p>`
      + `<p>${esc(collectionNames[dataKind] || dataKind || "综合")} · ${esc(modelFamilyNames[session.metadata?.model_family] || "历史数据")} · `
      + `${esc(collectionTaskNames[session.metadata?.task] || "未指定任务")} · 标签 ${esc(labels.join(", ") || (isModel ? "未设置" : "可选"))} · `
      + `${esc(splitNames[session.metadata?.default_split] || "未划分")} · ${esc(statusNames[session.status] || session.status)}</p>`
      + `<p class="session-progress">${esc(progress)}${active.length ? ` · 活动 ${esc(active.join(", "))}` : ""}</p></div>`
      + `<div class="actions">${controls}<button onclick="viewSamples('${session.session_id}')">样本与标注</button>`
      + `${importButton}<a class="button-link" href="/api/dashboard/sessions/${session.session_id}/export">导出数据集</a></div></article>`;
  }).join("") || `<div class="empty-state">还没有采集会话。先在上方选择一种数据开始采集。</div>`;
  $("model-list").innerHTML = (snapshot.models || []).map(model => `<article><div><strong>${esc(model.name)} `
    + `${esc(model.version)}</strong><p>${esc(model.target)} · ${esc(modelTaskNames[model.task] || model.task)} · ${esc(model.sha256).slice(0, 16)}</p>`
    + `</div><a href="/api/dashboard/models/${model.model_id}/download">下载</a></article>`).join("");
}

function renderSnapshot() {
  renderTelemetry();
  renderResourceRecording();
  heatmap("thermal", state.snapshot?.matrices?.thermal);
  const pressureMatrix = state.snapshot?.matrices?.pressure;
  heatmap("pressure", pressureMatrix);
  if (!pressureMatrix?.values?.length) {
    const pressure = get(state.snapshot, "telemetry.sensors.pressure") || {};
    const gateway = get(state.snapshot, "telemetry.health.gateway") || {};
    const pressureStates = {0: "空", 1: "打开失败", 2: "正常", 3: "部分帧", 4: "错误"};
    $("pressure-range").textContent = `暂无矩阵 · RA8 ${pressureStates[pressure.s] || `状态 ${pressure.s ?? "-"}`} · `
      + `ok ${pressure.ok ?? "-"} / err ${pressure.err ?? "-"} · `
      + `失败步 ${pressure.fs ?? "-"} / 通道 ${pressure.ch ?? "-"} / FSP ${pressure.fsp ?? "-"} · `
      + `ESP 待上传 ${gateway.pressure_pending ? "是" : "否"}`;
  }
  updateMedia(state.snapshot || {});
  renderRecordingTests(state.snapshot || {});
  renderCollections();
}

function renderActivity() {
  $("activity-feed").innerHTML = state.history.slice(-40).reverse().map(row => {
    const online = get(row, "health.ra8_online");
    const frames = get(row, "health.uart_valid_frames");
    const stateValue = get(row, "inference.state");
    return `<div><time>${esc(new Date(row.received_at).toLocaleTimeString())}</time>`
      + `seq=${esc(row.sequence)} ra8=${online ? "up" : "down"} frames=${esc(frames ?? "-")} `
      + `state=${esc(fusionLabels[stateValue] ?? stateValue ?? "-")}</div>`;
  }).join("");
}

async function loadHistory(targetDevice = state.device) {
  if (!targetDevice) return;
  const requestId = ++state.historyRequestId;
  const rows = await api(`/api/dashboard/history?device_id=${encodeURIComponent(targetDevice)}&limit=240`);
  if (targetDevice !== state.device || requestId !== state.historyRequestId) return;
  state.history = mergeTelemetryHistory(rows, state.history);
  state.lastHistoryAt = Date.now();
  updateCharts();
  renderActivity();
}

async function refresh(forceHistory = false) {
  if (state.refreshPromise) {
    state.refreshQueued = state.refreshQueued || forceHistory;
    return state.refreshPromise;
  }
  state.refreshPromise = (async () => {
    const devices = await api("/api/dashboard/devices");
    if (!state.device && devices[0]) state.device = devices[0].device_id;
    if (state.pressureDisplay.device !== state.device) loadPressureCalibration();
    $("device").innerHTML = devices.map(device => `<option value="${esc(device.device_id)}" `
      + `${device.device_id === state.device ? "selected" : ""}>${esc(device.device_id)}</option>`).join("");
    if (!state.device) return;
    const targetDevice = state.device;
    const snapshot = await api(`/api/dashboard/snapshot?device_id=${encodeURIComponent(targetDevice)}`);
    if (targetDevice !== state.device) return;
    state.snapshot = snapshot;
    renderSnapshot();
    if (forceHistory || Date.now() - state.lastHistoryAt > 5000) await loadHistory(targetDevice);
  })();
  try {
    await state.refreshPromise;
  } finally {
    state.refreshPromise = null;
    if (state.refreshQueued) {
      const queuedHistory = state.refreshQueued;
      state.refreshQueued = false;
      setTimeout(() => refresh(queuedHistory).catch(console.error), 0);
    }
  }
}

function scheduleRefresh() {
  clearTimeout(state.refreshTimer);
  state.refreshTimer = setTimeout(() => refresh().catch(console.error), 500);
}

function appendLiveTelemetry(telemetry) {
  if (!state.snapshot) return;
  state.snapshot.telemetry = telemetry;
  const device = state.snapshot.devices.find(item => item.device_id === state.device);
  if (device) { device.online = true; device.last_seen = telemetry.received_at; }
  state.telemetryArrivals.push(Date.now());
  if (state.telemetryArrivals.length > 12) state.telemetryArrivals.shift();
  state.history = mergeTelemetryHistory(state.history, [telemetry]);
  renderTelemetry();
  const thermalQuality = Number(get(telemetry, "sensors.thermal.frame_quality"));
  const normalizedQuality = Number.isFinite(thermalQuality) ? thermalQuality : null;
  if (normalizedQuality !== state.thermalDisplay.lastQuality && state.snapshot.matrices?.thermal) {
    heatmap("thermal", state.snapshot.matrices.thermal);
  }
  renderRecordingTests(state.snapshot);
  if (Date.now() - state.lastChartAt > 1000) {
    state.lastChartAt = Date.now();
    updateCharts();
    renderActivity();
  }
}

async function refreshDashboardLiveState() {
  if (!state.snapshot || !state.device || state.realtime.liveStatePromise) return;
  const targetDevice = state.device;
  state.realtime.liveStatePromise = api(
    `/api/dashboard/devices/${encodeURIComponent(targetDevice)}/live-state`,
  );
  try {
    const live = await state.realtime.liveStatePromise;
    if (!state.snapshot || targetDevice !== state.device) return;
    const device = state.snapshot.devices.find(item => item.device_id === targetDevice);
    if (device && live.device) Object.assign(device, live.device);
    state.snapshot.telemetry_status = live.telemetry_status || {};
    state.snapshot.matrix_status = live.matrix_status || {};
    state.snapshot.sensor_diagnostics = live.sensor_diagnostics || [];
    state.snapshot.resource_recording = live.resource_recording || null;
    renderTelemetry();
    renderResourceRecording();
  } catch (error) {
    console.error("lightweight dashboard state refresh failed", error);
  } finally {
    state.realtime.liveStatePromise = null;
  }
}

function applyLiveMessage(message) {
  if (message.device_id && message.device_id !== state.device) return;
  if (message.kind === "telemetry") {
    appendLiveTelemetry(message.data);
  } else if (message.kind === "matrix" && state.snapshot) {
    state.snapshot.matrices ||= {};
    state.snapshot.matrices[message.data.kind] = message.data;
    heatmap(message.data.kind, message.data);
    if (message.data.kind === "thermal") renderLiveVisionOverlay();
  } else if (message.kind === "media") {
    scheduleRefresh();
  } else if (message.kind === "event" && state.snapshot) {
    const event = {
      ...(message.data || {}),
      received_at: message.data?.received_at || message.at,
      payload: message.data || {},
    };
    const events = Array.isArray(state.snapshot.events) ? state.snapshot.events : [];
    state.snapshot.events = [event, ...events.filter(item => item.id !== event.id)].slice(0, 60);
    renderEventRows();
  } else if (message.kind === "ai_story" || message.kind === "ai_advisory") {
    if ($("ai").classList.contains("active")) loadAi(true).catch(console.error);
  } else if (message.kind === "resource_recording" && state.snapshot) {
    state.snapshot.resource_recording = message.data?.session || null;
    renderResourceRecording();
    scheduleRefresh();
  } else {
    scheduleRefresh();
  }
}

function liveMessageKey(message) {
  if (message.kind === "matrix") return `matrix:${message.data?.kind || "unknown"}`;
  if (["telemetry", "media", "matrix_status", "device"].includes(message.kind)) return message.kind;
  return `${message.kind || "unknown"}:${message.device_id || ""}:${message.data?.id || ""}`;
}

function queueLiveMessage(message) {
  if (!message || message.kind === "heartbeat" || message.kind === "heartbeat_ack") return;
  state.realtime.pending.set(liveMessageKey(message), message);
  if (state.realtime.renderScheduled) return;
  state.realtime.renderScheduled = true;
  requestAnimationFrame(() => {
    state.realtime.renderScheduled = false;
    const pending = [...state.realtime.pending.values()];
    state.realtime.pending.clear();
    const priority = message => message.kind === "telemetry" ? 0
      : message.kind === "matrix" && message.data?.kind === "pressure" ? 1
        : message.kind === "matrix" ? 2 : 3;
    pending.sort((left, right) => priority(left) - priority(right)).forEach(applyLiveMessage);
  });
}

function stopRealtimeFallback() {
  clearTimeout(state.realtime.fallbackTimer);
  state.realtime.fallbackTimer = null;
  state.realtime.fallback?.close();
  state.realtime.fallback = null;
}

function startRealtimeFallback() {
  if (state.realtime.fallback || state.realtime.socket?.readyState === WebSocket.OPEN) return;
  const events = new EventSource("/api/dashboard/stream");
  state.realtime.fallback = events;
  events.onopen = () => {
    state.realtime.lastMessageAt = Date.now();
    $("connection").textContent = "实时连接（兼容通道）";
    statusClass($("connection-dot"), "warning");
  };
  events.onerror = () => {
    $("connection").textContent = "正在重连";
    statusClass($("connection-dot"), "danger");
  };
  events.addEventListener("update", event => {
    state.realtime.lastMessageAt = Date.now();
    try { queueLiveMessage(JSON.parse(event.data)); } catch (error) { console.error(error); }
  });
}

function connectDashboardRealtime() {
  const current = state.realtime.socket;
  if (current && (current.readyState === WebSocket.OPEN || current.readyState === WebSocket.CONNECTING)) return;
  clearTimeout(state.realtime.retry);
  const socket = new WebSocket(mediaWebSocketUrl("/api/dashboard/realtime/ws"));
  state.realtime.socket = socket;
  state.realtime.fallbackTimer = setTimeout(startRealtimeFallback, 2500);
  socket.onopen = () => {
    if (state.realtime.socket !== socket) return;
    stopRealtimeFallback();
    state.realtime.retryDelay = 1000;
    state.realtime.lastMessageAt = Date.now();
    $("connection").textContent = "实时连接";
    statusClass($("connection-dot"), "ok");
    clearInterval(state.realtime.heartbeat);
    state.realtime.heartbeat = setInterval(() => {
      if (socket.readyState !== WebSocket.OPEN) return;
      if (Date.now() - state.realtime.lastMessageAt > 12000) {
        socket.close(4000, "realtime timeout");
        return;
      }
      socket.send(JSON.stringify({
        type: "dashboard.ping",
        sequence: ++state.realtime.heartbeatSequence,
      }));
    }, 5000);
  };
  socket.onmessage = event => {
    state.realtime.lastMessageAt = Date.now();
    try { queueLiveMessage(JSON.parse(event.data)); } catch (error) { console.error(error); }
  };
  socket.onerror = () => socket.close();
  socket.onclose = () => {
    if (state.realtime.socket !== socket) return;
    state.realtime.socket = null;
    clearInterval(state.realtime.heartbeat);
    state.realtime.heartbeat = null;
    startRealtimeFallback();
    $("connection").textContent = "正在重连";
    statusClass($("connection-dot"), "danger");
    const delay = state.realtime.retryDelay;
    state.realtime.retryDelay = Math.min(10000, delay * 2);
    state.realtime.retry = setTimeout(connectDashboardRealtime, delay);
  };
}

function mediaWebSocketUrl(path) {
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${location.host}${path}`;
}

function parseVideoFrame(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 36) throw new Error("视频帧不足 32 字节头和 JPEG 数据");
  const view = new DataView(buffer);
  if (view.getUint8(0) !== 0x53 || view.getUint8(1) !== 0x33
      || view.getUint8(2) !== 0x4d || view.getUint8(3) !== 0x44) {
    throw new Error("S3MD Magic 错误");
  }
  const version = view.getUint8(4);
  const mediaType = view.getUint8(5);
  const flags = view.getUint16(6, false);
  const sequence = view.getUint32(8, false);
  const timestampUs = view.getUint32(12, false) * 4294967296 + view.getUint32(16, false);
  const payloadLength = view.getUint32(20, false);
  const width = view.getUint16(24, false);
  const height = view.getUint16(26, false);
  const quality = view.getUint16(28, false);
  const reserved = view.getUint16(30, false);
  if (version !== 1 || mediaType !== 1 || (flags & ~1)) throw new Error("不支持的 S3MD 视频版本或标志");
  if (!payloadLength || payloadLength > 512 * 1024 || buffer.byteLength !== 32 + payloadLength) {
    throw new Error("JPEG payload 长度错误");
  }
  if (!width || !height || width > 4096 || height > 4096 || quality > 63 || reserved !== 0) {
    throw new Error("JPEG 元数据错误");
  }
  const jpeg = new Uint8Array(buffer, 32, payloadLength);
  if (jpeg[0] !== 0xff || jpeg[1] !== 0xd8
      || jpeg[payloadLength - 2] !== 0xff || jpeg[payloadLength - 1] !== 0xd9) {
    throw new Error("JPEG 缺少 SOI/EOI 标记");
  }
  return {flags, sequence, timestampUs, width, height, quality, payloadLength, jpeg};
}

function renderVideoRuntime(session, frame) {
  const elapsed = Math.max(0.001, (performance.now() - session.startedAt) / 1000);
  const invalidFrames = Number(state.snapshot?.media?.video_diagnostics?.invalid_frames || 0);
  $("video-frame-state").textContent = String(session.frames);
  $("video-frame-detail").textContent = `序列 ${frame.sequence} · ${session.gaps} 帧间断`
    + ` · ${(session.frames / elapsed).toFixed(1)} FPS · 无效 ${invalidFrames}`;
  $("video-format-state").textContent = `${frame.width}×${frame.height}`;
  $("video-format-detail").textContent = `${formatBytes(frame.payloadLength)} · JPEG quality ${frame.quality} · S3MD WSS`;
  $("video-runtime").textContent = "WSS 在线 · 实时画面";
  $("video-listener-state").textContent = "预览中";
  statusClass($("video-listener-state"), "ok");
}

function displayLatestVideoFrame(frame, session) {
  if (state.video !== session || !state.videoWanted) return;
  if (session.imageLoading) {
    session.pendingFrame = frame;
    return;
  }

  session.imageLoading = true;
  const nextUrl = URL.createObjectURL(new Blob([frame.jpeg], {type: "image/jpeg"}));
  session.loadingUrl = nextUrl;
  const decoder = new Image();
  session.decoder = decoder;
  decoder.onload = () => {
    if (state.video !== session || !state.videoWanted) {
      URL.revokeObjectURL(nextUrl);
      return;
    }
    const image = $("video-preview");
    const previousUrl = session.objectUrl;
    session.objectUrl = nextUrl;
    session.loadingUrl = null;
    session.decoder = null;
    session.imageLoading = false;
    image.src = nextUrl;
    image.classList.add("ready");
    $("video-empty").hidden = true;
    if (previousUrl) URL.revokeObjectURL(previousUrl);

    const pending = session.pendingFrame;
    session.pendingFrame = null;
    if (pending) displayLatestVideoFrame(pending, session);
  };
  decoder.onerror = () => {
    URL.revokeObjectURL(nextUrl);
    if (state.video !== session) return;
    session.loadingUrl = null;
    session.decoder = null;
    session.imageLoading = false;
    session.errors++;
    $("video-runtime").textContent = "浏览器无法解码 JPEG";
    statusClass($("video-listener-state"), "danger");
    const pending = session.pendingFrame;
    session.pendingFrame = null;
    if (pending) displayLatestVideoFrame(pending, session);
  };
  decoder.src = nextUrl;
}

function handleVideoFrame(buffer, session) {
  if (state.video !== session || !state.videoWanted) return;
  const frame = parseVideoFrame(buffer);
  if (session.lastSequence != null) {
    const delta = (frame.sequence - session.lastSequence) >>> 0;
    if (delta > 1 && delta < 0x80000000) session.gaps += delta - 1;
  }
  session.lastSequence = frame.sequence;
  session.frames++;
  displayLatestVideoFrame(frame, session);
  renderVideoRuntime(session, frame);
}

function scheduleVideoReconnect(session) {
  if (!state.videoWanted || state.video !== session || session.device !== state.device) return;
  clearTimeout(state.videoRetry);
  const delay = state.videoRetryDelay;
  $("video-runtime").textContent = `${(delay / 1000).toFixed(0)} 秒后重连`;
  state.videoRetry = setTimeout(() => connectVideoSocket(session), delay);
  state.videoRetryDelay = Math.min(8000, delay * 2);
}

function connectVideoSocket(session) {
  if (!state.videoWanted || state.video !== session || session.device !== state.device) return;
  clearTimeout(state.videoRetry);
  const socket = new WebSocket(mediaWebSocketUrl(
    `/api/dashboard/media/${encodeURIComponent(session.device)}/video/ws`,
  ));
  socket.binaryType = "arraybuffer";
  session.socket = socket;
  $("video-runtime").textContent = "正在连接视频 WSS";
  socket.onopen = () => {
    if (state.video !== session) { socket.close(); return; }
    state.videoRetryDelay = 1000;
    $("video-runtime").textContent = "WSS 在线 · 等待 P4 JPEG";
    session.heartbeatSequence = 0;
    clearInterval(session.heartbeat);
    session.heartbeat = setInterval(() => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: "media.heartbeat", version: 1, sequence: ++session.heartbeatSequence,
        }));
      }
    }, 20000);
    scheduleRefresh();
  };
  socket.onmessage = event => {
    if (typeof event.data === "string") return;
    try {
      handleVideoFrame(event.data, session);
    } catch (error) {
      session.errors++;
      $("video-runtime").textContent = error.message;
      statusClass($("video-listener-state"), "danger");
    }
  };
  socket.onerror = () => { $("video-runtime").textContent = "视频 WSS 连接异常"; };
  socket.onclose = () => {
    clearInterval(session.heartbeat);
    session.heartbeat = null;
    if (session.socket === socket) session.socket = null;
    scheduleRefresh();
    scheduleVideoReconnect(session);
  };
}

function startVideo() {
  if (state.videoWanted) { stopVideo(); return; }
  if (!state.device) {
    $("video-runtime").textContent = "请先选择设备";
    return;
  }
  state.videoWanted = true;
  state.videoRetryDelay = 1000;
  $("video-toggle").textContent = "停止预览";
  $("video-empty").hidden = false;
  const session = {
    device: state.device,
    socket: null,
    heartbeat: null,
    heartbeatSequence: 0,
    objectUrl: null,
    loadingUrl: null,
    decoder: null,
    imageLoading: false,
    pendingFrame: null,
    frames: 0,
    gaps: 0,
    errors: 0,
    lastSequence: null,
    startedAt: performance.now(),
  };
  state.video = session;
  connectVideoSocket(session);
}

function stopVideo() {
  state.videoWanted = false;
  clearTimeout(state.videoRetry);
  state.videoRetry = null;
  const session = state.video;
  state.video = null;
  if (session) {
    clearInterval(session.heartbeat);
    if (session.socket) {
      session.socket.onclose = null;
      session.socket.close();
    }
    if (session.decoder) session.decoder.src = "";
    if (session.loadingUrl) URL.revokeObjectURL(session.loadingUrl);
    if (session.objectUrl) URL.revokeObjectURL(session.objectUrl);
  }
  const image = $("video-preview");
  image.classList.remove("ready");
  image.removeAttribute("src");
  $("live-vision-overlay").classList.remove("ready");
  $("live-thermal-overlay").classList.remove("ready");
  $("video-empty").hidden = false;
  $("video-runtime").textContent = "尚未预览";
  $("video-toggle").textContent = "开始预览";
  $("video-listener-state").textContent = "未开启";
  statusClass($("video-listener-state"), "");
  scheduleRefresh();
}

function parseAudioFrame(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 32) throw new Error("音频帧头不足 32 字节");
  const view = new DataView(buffer);
  if (view.getUint8(0) !== 0x53 || view.getUint8(1) !== 0x33
      || view.getUint8(2) !== 0x4d || view.getUint8(3) !== 0x44) {
    throw new Error("S3MD Magic 错误");
  }
  const version = view.getUint8(4);
  const mediaType = view.getUint8(5);
  const flags = view.getUint16(6, false);
  const sequence = view.getUint32(8, false);
  const timestampUs = view.getUint32(12, false) * 4294967296 + view.getUint32(16, false);
  const payloadLength = view.getUint32(20, false);
  const sampleRate = view.getUint16(24, false);
  const samplesPerFrame = view.getUint16(26, false);
  const channels = view.getUint16(28, false);
  const bits = view.getUint16(30, false);
  if (version !== 1 || mediaType !== 2 || (flags & ~1)) throw new Error("不支持的 S3MD 音频版本或标志");
  if (payloadLength !== 640 || buffer.byteLength !== 32 + payloadLength) throw new Error("PCM payload 长度错误");
  if (sampleRate !== 16000 || samplesPerFrame !== 320 || channels !== 1 || bits !== 16) {
    throw new Error("音频必须为 16 kHz、单声道、PCM16、320 samples/帧");
  }
  const samples = new Float32Array(samplesPerFrame);
  for (let index = 0; index < samplesPerFrame; index++) {
    samples[index] = view.getInt16(32 + index * 2, true) / 32768;
  }
  return {flags, sequence, timestampUs, samples};
}

function renderAudioRuntime(session, force = false) {
  const now = performance.now();
  if (!force && now - session.lastRenderAt < 200) return;
  session.lastRenderAt = now;
  const elapsed = Math.max(0.001, (now - session.startedAt) / 1000);
  $("audio-frame-state").textContent = String(session.frames);
  $("audio-frame-detail").textContent = `序列 ${session.lastSequence ?? "-"} · ${session.gaps} 帧间断 · ${(session.frames / elapsed).toFixed(1)} FPS`;
  $("audio-latency").textContent = session.relativeLatencyMs == null ? "-" : `${session.relativeLatencyMs.toFixed(1)} ms`;
  $("audio-runtime").textContent = session.socket?.readyState === WebSocket.OPEN
    ? "WSS 在线 · 正在播放" : "正在连接音频 WSS";
  $("audio-listener-state").textContent = "监听中";
  statusClass($("audio-listener-state"), "ok");
}

function handleAudioFrame(buffer, session) {
  if (state.audio !== session || !state.audioWanted) return;
  const frame = parseAudioFrame(buffer);
  if ((frame.flags & 1) || (session.lastTimestampUs != null && frame.timestampUs < session.lastTimestampUs)) {
    session.node.port.postMessage({type: "reset"});
    session.lastSequence = null;
    session.minimumOffsetUs = null;
  }
  if (session.lastSequence != null) {
    const delta = (frame.sequence - session.lastSequence) >>> 0;
    if (delta > 1 && delta < 0x80000000) session.gaps += delta - 1;
  }
  const offsetUs = performance.now() * 1000 - frame.timestampUs;
  session.minimumOffsetUs = session.minimumOffsetUs == null
    ? offsetUs : Math.min(session.minimumOffsetUs, offsetUs);
  session.relativeLatencyMs = Math.max(0, (offsetUs - session.minimumOffsetUs) / 1000);
  session.lastSequence = frame.sequence;
  session.lastTimestampUs = frame.timestampUs;
  session.frames++;
  drawAudio(frame.samples);
  session.node.port.postMessage({type: "samples", samples: frame.samples}, [frame.samples.buffer]);
  renderAudioRuntime(session);
}

function scheduleAudioReconnect(session) {
  if (!state.audioWanted || state.audio !== session || session.device !== state.device) return;
  clearTimeout(state.audioRetry);
  const delay = state.audioRetryDelay;
  $("audio-runtime").textContent = `${(delay / 1000).toFixed(0)} 秒后重连`;
  state.audioRetry = setTimeout(() => connectAudioSocket(session), delay);
  state.audioRetryDelay = Math.min(8000, delay * 2);
}

function connectAudioSocket(session) {
  if (!state.audioWanted || state.audio !== session || session.device !== state.device) return;
  clearTimeout(state.audioRetry);
  session.node.port.postMessage({type: "reset"});
  const socket = new WebSocket(mediaWebSocketUrl(
    `/api/dashboard/media/${encodeURIComponent(session.device)}/audio/ws`,
  ));
  socket.binaryType = "arraybuffer";
  session.socket = socket;
  $("audio-runtime").textContent = "正在连接音频 WSS";
  socket.onopen = () => {
    if (state.audio !== session) { socket.close(); return; }
    state.audioRetryDelay = 1000;
    $("audio-runtime").textContent = "WSS 在线 · 等待 P4 音频";
    session.heartbeatSequence = 0;
    clearInterval(session.heartbeat);
    session.heartbeat = setInterval(() => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({
          type: "media.heartbeat", version: 1, sequence: ++session.heartbeatSequence,
        }));
      }
    }, 20000);
  };
  socket.onmessage = event => {
    if (typeof event.data === "string") {
      try {
        const message = JSON.parse(event.data);
        if (message.type === "media.error") $("audio-runtime").textContent = message.message;
      } catch (_) { /* heartbeat responses need no UI update */ }
      return;
    }
    try {
      handleAudioFrame(event.data, session);
    } catch (error) {
      $("audio-runtime").textContent = error.message;
      statusClass($("audio-listener-state"), "danger");
    }
  };
  socket.onerror = () => { $("audio-runtime").textContent = "音频 WSS 连接异常"; };
  socket.onclose = () => {
    clearInterval(session.heartbeat);
    session.heartbeat = null;
    if (session.socket === socket) session.socket = null;
    session.node.port.postMessage({type: "reset"});
    scheduleAudioReconnect(session);
  };
}

async function startAudio() {
  if (state.audioWanted) { stopAudio(); return; }
  if (!state.device) {
    $("audio-runtime").textContent = "请先选择设备";
    return;
  }
  if (!window.AudioWorkletNode) {
    $("audio-runtime").textContent = "当前浏览器不支持 AudioWorklet";
    return;
  }

  state.audioWanted = true;
  $("audio-toggle").disabled = true;
  $("audio-toggle").textContent = "正在启动";
  const device = state.device;
  let context;
  try {
    context = new AudioContext({latencyHint: "interactive"});
    await context.audioWorklet.addModule("/static/audio-worklet.js");
    if (!state.audioWanted || device !== state.device) { await context.close(); return; }
    const node = new AudioWorkletNode(context, "pcm16-stream-player", {
      processorOptions: {
        inputSampleRate: 16000,
        frameSamples: 320,
        targetBufferFrames: 3,
        maxBufferFrames: 8,
      },
    });
    const gain = context.createGain();
    gain.gain.value = Number($("audio-volume").value);
    node.connect(gain).connect(context.destination);
    const session = {
      device, context, node, gain, socket: null, heartbeat: null,
      heartbeatSequence: 0, frames: 0, gaps: 0, lastSequence: null,
      lastTimestampUs: null, minimumOffsetUs: null, relativeLatencyMs: null,
      startedAt: performance.now(), lastRenderAt: 0,
    };
    node.port.onmessage = event => {
      if (state.audio !== session || event.data?.type !== "stats") return;
      const stats = event.data;
      $("audio-buffer").textContent = `${Number(stats.bufferedMs || 0).toFixed(0)} ms`;
      $("audio-buffer-detail").textContent = `目标 60–80 ms · ${stats.bufferedSamples || 0} samples`;
      const droppedFrames = Math.ceil(Number(stats.droppedSamples || 0) / 320);
      $("audio-playback-errors").textContent = String(Number(stats.underruns || 0) + droppedFrames);
      $("audio-playback-detail").textContent = `欠载 ${stats.underruns || 0} · 浏览器丢弃 ${droppedFrames}`;
    };
    state.audio = session;
    await context.resume();
    $("audio-toggle").disabled = false;
    $("audio-toggle").textContent = "停止监听";
    renderAudioRuntime(session, true);
    connectAudioSocket(session);
  } catch (error) {
    state.audioWanted = false;
    if (context) await context.close().catch(() => {});
    $("audio-toggle").disabled = false;
    $("audio-toggle").textContent = "开始监听";
    $("audio-runtime").textContent = `启动失败：${error.message}`;
  }
}

function stopAudio() {
  state.audioWanted = false;
  clearTimeout(state.audioRetry);
  state.audioRetry = null;
  const session = state.audio;
  state.audio = null;
  if (session) {
    clearInterval(session.heartbeat);
    if (session.socket) {
      session.socket.onclose = null;
      session.socket.close(1000, "listener stopped");
    }
    session.node.port.postMessage({type: "reset"});
    session.node.disconnect();
    session.gain.disconnect();
    session.context.close().catch(() => {});
  }
  $("audio-toggle").disabled = false;
  $("audio-toggle").textContent = "开始监听";
  $("audio-runtime").textContent = "尚未监听";
  $("audio-listener-state").textContent = "未开启";
  statusClass($("audio-listener-state"), "");
  $("audio-buffer").textContent = "-";
}

function drawAudio(values) {
  const canvas = $("audio-meter");
  const context = canvas.getContext("2d");
  context.fillStyle = "#101416"; context.fillRect(0, 0, canvas.width, canvas.height);
  context.strokeStyle = "#49c49e"; context.lineWidth = 2; context.beginPath();
  const step = Math.max(1, Math.floor(values.length / canvas.width));
  for (let x = 0; x < canvas.width; x++) {
    const value = values[Math.min(values.length - 1, x * step)] || 0;
    const y = canvas.height / 2 - value * canvas.height * 0.44;
    if (x) context.lineTo(x, y); else context.moveTo(x, y);
  }
  context.stroke();
}

async function sendCommand(command, args = {}) {
  const result = await api("/api/dashboard/commands", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({device_id: state.device, command, args}),
  });
  $("command-result").textContent = `命令 ${result.id} 已排队`;
  return result;
}

function renderAi(data) {
  state.ai.data = data;
  const status = data?.status || {};
  const enabled = status.enabled === true;
  $("ai-runtime-state").textContent = enabled
    ? `${status.model || "Qwen"} · 故事队列 ${status.story_queue_depth || 0}/${status.story_queue_limit || 0}`
      + ` · 评估队列 ${status.assessment_queue_depth || 0}/${status.assessment_queue_limit || 0}`
    : "大模型服务未启用";
  statusClass($("ai-runtime-state"), enabled ? "ok" : "warning");

  const stories = Array.isArray(data?.stories) ? data.stories : [];
  $("ai-story-list").innerHTML = stories.map(story => {
    const request = story.request || {};
    const playable = Number(story.audio_size_bytes || 0) > 0 && story.status !== "failed";
    const duration = Number(story.audio_duration_ms || 0);
    return `<div class="ai-item"><div class="ai-item-heading"><strong>${esc(story.title || request.theme || "未命名故事")}</strong>`
      + `<span class="ai-status ${story.status === "failed" ? "danger" : ""}">${esc(aiStoryStatusLabels[story.status] || story.status || "未知")}</span></div>`
      + `<p>${esc(story.story_text || story.error || "模型任务正在处理中")}</p>`
      + `<div class="ai-item-meta"><span>${esc(formatTime(story.updated_at || story.created_at))}</span>`
      + `<span>${duration > 0 ? `${(duration / 1000).toFixed(0)} 秒` : "等待音频"}</span>`
      + `${playable ? `<button type="button" data-ai-replay="${esc(story.story_id)}">再次播放</button>` : ""}</div></div>`;
  }).join("") || `<p class="empty-state">暂无故事记录</p>`;
  document.querySelectorAll("[data-ai-replay]").forEach(button => {
    button.onclick = () => replayAiStory(button.dataset.aiReplay, button);
  });

  const advisories = Array.isArray(data?.advisories) ? data.advisories : [];
  $("ai-advisory-list").innerHTML = advisories.map(advisory => {
    const evidence = Array.isArray(advisory.evidence) ? advisory.evidence : [];
    const actions = Array.isArray(advisory.actions) ? advisory.actions : [];
    const confidence = Math.round(Math.max(0, Math.min(1, Number(advisory.confidence || 0))) * 100);
    return `<div class="ai-item advisory-${esc(advisory.level || "normal")}"><div class="ai-item-heading"><strong>${esc(advisory.title || "遥测评估")}</strong>`
      + `<span class="ai-status">${esc(aiAdvisoryLevelLabels[advisory.level] || advisory.level || "未知")} · ${confidence}%</span></div>`
      + `<p>${esc(advisory.summary || "无摘要")}</p>`
      + `${evidence.length ? `<div class="ai-evidence"><b>依据</b>${evidence.map(item => `<span>${esc(item)}</span>`).join("")}</div>` : ""}`
      + `${actions.length ? `<div class="ai-evidence"><b>建议</b>${actions.map(item => `<span>${esc(item)}</span>`).join("")}</div>` : ""}`
      + `<div class="ai-item-meta"><span>${esc(formatTime(advisory.created_at))}</span><span>遥测序列 ${Number(advisory.source_sequence || 0)}</span></div></div>`;
  }).join("") || `<p class="empty-state">暂无评估记录</p>`;
}

async function loadAi(force = false) {
  if (!state.device || state.ai.loading) return;
  if (!force && Date.now() - state.ai.lastLoadedAt < 2500) return;
  state.ai.loading = true;
  try {
    const data = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/ai`);
    state.ai.lastLoadedAt = Date.now();
    renderAi(data);
  } catch (error) {
    $("ai-runtime-state").textContent = error.message;
    statusClass($("ai-runtime-state"), "danger");
  } finally {
    state.ai.loading = false;
  }
}

async function replayAiStory(storyId, button) {
  const previous = button.textContent;
  button.disabled = true;
  button.textContent = "下发中";
  try {
    const result = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/ai/stories/${encodeURIComponent(storyId)}/replay`, {method: "POST"});
    $("ai-story-result").textContent = `故事已排队播放 · 命令 ${result.story.command_id || "等待生成"}`;
    await loadAi(true);
  } catch (error) {
    $("ai-story-result").textContent = error.message;
  } finally {
    button.disabled = false;
    button.textContent = previous;
  }
}

function sessionById(sessionId) {
  return (state.snapshot?.sessions || []).find(session => session.session_id === sessionId);
}

function collectionContextValues() {
  return {
    task: $("collection-task").value,
    subject: $("collection-subject").value,
    posture: $("collection-posture").value,
    bed_region: $("collection-region").value,
    blanket_state: $("collection-blanket").value,
    heater_state: $("collection-heater").value,
    audio_scene: $("collection-audio-scene").value,
    caregiver: $("collection-caregiver").value,
    repeat_id: Math.max(1, Number.parseInt($("collection-repeat").value, 10) || 1),
    default_split: $("collection-split").value,
  };
}

function updateCollectionLabelOptions() {
  const task = $("collection-task").value;
  const values = collectionTaskLabelOptions[task] || [];
  $("collection-label-options").innerHTML = values.map(value => `<option value="${esc(value)}">`).join("");
  $("collection-label").placeholder = task === "rule_reference"
    ? "参考信号可留空，例如 infant_center"
    : `必填：${values.join(" / ")}`;
}

function suggestCollectionLabel() {
  const context = collectionContextValues();
  let label = "";
  if (context.task === "vision_scene") {
    if (context.subject === "absent" && context.blanket_state === "none") label = "empty_bed";
    else if (context.subject === "present") label = "infant";
    else if (context.caregiver === "present") label = "caregiver";
    else if (context.blanket_state !== "none") label = "blanket";
  } else if (context.task === "vision_occlusion") {
    label = context.subject === "absent" ? "not_visible" : ({
      partial_face: "partial", full_face: "covered",
    }[context.blanket_state] || "clear");
  } else if (context.task === "vision_behavior") {
    label = context.subject === "absent" ? "unknown" : ({
      lying_supine: "lying", lying_side: "lying", sitting: "sitting",
      crawling_kneeling: "crawling_kneeling", standing: "standing",
    }[context.posture] || "unknown");
  } else if (context.task === "cry_detection") {
    label = context.audio_scene === "cry_playback" ? "crying" : "non_crying";
  } else if (context.task === "cry_type") {
    label = "unknown";
  } else if (context.subject === "absent") {
    label = "empty_bed";
  } else if (context.posture === "standing") {
    label = "standing";
  } else if (context.bed_region.endsWith("edge")) {
    label = "infant_edge";
  } else {
    label = "infant_center";
  }
  $("collection-label").value = label;
  $("collection-label-help").textContent = label
    ? `建议标签 ${label}；开始前请按实际场景复核。` : "当前条件无法唯一建议标签，请手动填写。";
}

function configureImport(sessionId) {
  const session = sessionById(sessionId);
  const kind = session?.metadata?.data_kind;
  const enabled = session?.metadata?.workflow === "model_dataset" && modelDataKinds.has(kind);
  state.importSession = enabled ? sessionId : "";
  $("sd-import-panel").hidden = !enabled;
  if (!enabled) return;
  $("sd-import-files").accept = kind === "camera" ? ".jpg,.jpeg,image/jpeg" : ".wav,audio/wav";
  const path = kind === "camera" ? "/CAMERA/C*.JPG" : "/MIC/M*.WAV";
  $("sd-import-help").textContent = `${session.name} · 请选择本轮对应的 ${path} 文件；按文件名排序后依次匹配待导入样本。`;
}

function sampleSource(sample) {
  const features = sample.features || {};
  if (sample.source_ref.startsWith("artifact:")) {
    const artifactId = sample.source_ref.slice("artifact:".length);
    const name = features.original_filename || (sample.kind === "camera" ? "JPEG 图像" : "WAV 音频");
    return `<span class="source-status imported">已导入</span><a target="_blank" rel="noopener" `
      + `href="/api/dashboard/artifacts/${encodeURIComponent(artifactId)}">${esc(name)}</a>`;
  }
  if (sample.source_ref.startsWith("sd:")) {
    return `<span class="source-status pending">SD 待导入</span><code>${esc(features.expected_path_pattern || sample.source_ref)}</code>`;
  }
  return `<code>${esc(sample.source_ref)}</code>`;
}

async function loadSamples(sessionId) {
  const samples = await api(`/api/dashboard/sessions/${sessionId}/samples?limit=500`);
  const session = sessionById(sessionId);
  const isModel = session?.metadata?.workflow === "model_dataset";
  state.sampleSession = sessionId;
  configureImport(sessionId);
  $("sample-session").textContent = session ? `${session.name} · ${sessionId}` : sessionId;
  $("sample-count").textContent = `${samples.length} 条`;
  $("sample-rows").innerHTML = samples.map(sample => {
    const splitControl = isModel
      ? `<select data-field="split">${Object.entries(splitNames).map(([value, name]) => `
          <option value="${value}" ${sample.split === value ? "selected" : ""}>${name}</option>`).join("")}</select>`
      : `<span class="not-applicable">不适用</span><input type="hidden" data-field="split" value="unassigned">`;
    return `<tr data-sample="${sample.id}">`
      + `<td>${esc(formatTime(sample.captured_at))}</td><td>${esc(collectionNames[sample.kind] || sample.kind)}</td>`
      + `<td class="sample-source">${sampleSource(sample)}</td>`
      + `<td><input data-field="label" value="${esc(sample.label || "")}" placeholder="未标注"></td>`
      + `<td>${splitControl}</td>`
      + `<td><select data-field="quality">${Object.entries(qualityNames).map(([value, name]) => `
          <option value="${value}" ${sample.quality === value ? "selected" : ""}>${name}</option>`).join("")}</select></td>`
      + `<td><input data-field="notes" value="${esc(sample.notes || "")}" placeholder="复核备注"></td>`
      + `<td><button onclick="saveSample(${sample.id})">保存</button></td></tr>`;
  }).join("") || `<tr><td colspan="8" class="empty-state">当前会话还没有样本。媒体采集时等待 SD 保存计数增长，参考信号采集时等待下一次数据上报。</td></tr>`;
}

window.ack = async id => { await api(`/api/dashboard/events/${id}/ack`, {method: "POST"}); scheduleRefresh(); };
window.sessionAction = async (id, action, kind = "all") => {
  await api(`/api/dashboard/sessions/${id}/${action}?kind=${kind}`, {method: "POST"});
  scheduleRefresh();
};
window.viewSamples = sessionId => loadSamples(sessionId).catch(console.error);
window.openImport = sessionId => {
  configureImport(sessionId);
  loadSamples(sessionId).catch(console.error);
  $("sd-import-files").click();
};
window.saveSample = async sampleId => {
  const row = document.querySelector(`tr[data-sample="${sampleId}"]`);
  const value = field => row.querySelector(`[data-field="${field}"]`).value;
  await api(`/api/dashboard/samples/${sampleId}`, {
    method: "PATCH",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      label: value("label"), split: value("split"), quality: value("quality"), notes: value("notes"),
    }),
  });
  row.querySelector("button").textContent = "已保存";
  setTimeout(() => { row.querySelector("button").textContent = "保存"; }, 1000);
  scheduleRefresh();
};

async function importSdFiles() {
  const session = sessionById(state.importSession);
  const kind = session?.metadata?.data_kind;
  const input = $("sd-import-files");
  const files = [...input.files].sort((left, right) => left.name.localeCompare(
    right.name, undefined, {numeric: true, sensitivity: "base"},
  ));
  if (!session || !modelDataKinds.has(kind)) {
    $("sd-import-result").textContent = "请先选择一个视觉或哭声音频会话";
    return;
  }
  if (!files.length) {
    $("sd-import-result").textContent = "请先选择文件";
    input.click();
    return;
  }
  let imported = 0;
  let duplicates = 0;
  $("sd-import-start").disabled = true;
  try {
    for (const [index, file] of files.entries()) {
      $("sd-import-result").textContent = `正在上传 ${index + 1}/${files.length} · ${file.name}`;
      const query = new URLSearchParams({kind, filename: file.name});
      const result = await api(
        `/api/dashboard/sessions/${session.session_id}/artifacts/import?${query}`,
        {method: "POST", headers: {"Content-Type": file.type || "application/octet-stream"}, body: file},
      );
      if (result.duplicate) duplicates += 1; else imported += 1;
    }
    $("sd-import-result").textContent = `完成：新导入 ${imported}，重复跳过 ${duplicates}`;
    input.value = "";
    await refresh(true);
    await loadSamples(session.session_id);
  } catch (error) {
    $("sd-import-result").textContent = `导入中止：${error.message}`;
  } finally {
    $("sd-import-start").disabled = false;
  }
}

async function collectionAction(kind, action) {
  const sessions = state.snapshot?.sessions || [];
  const active = sessions.filter(session => session.status === "active"
    && session.metadata?.device_id === state.device
    && (session.metadata?.active_kinds || []).includes(kind));
  if (action === "stop") {
    await Promise.all(active.map(session => api(
      `/api/dashboard/sessions/${session.session_id}/stop?kind=${kind}`, {method: "POST"},
    )));
    $("collection-result").textContent = `${collectionNames[kind]} 已停止`;
    await refresh(true);
    if (modelDataKinds.has(kind) && active[0]) await loadSamples(active[0].session_id);
    return;
  }

  const label = $("collection-label").value.trim();
  const isModel = modelDataKinds.has(kind);
  const context = collectionContextValues();
  const expectedPrefix = kind === "camera" ? "vision_" : kind === "audio" ? "cry_" : "";
  if (expectedPrefix && !context.task.startsWith(expectedPrefix)) {
    $("collection-result").textContent = `${collectionNames[kind]} 与当前任务 ${collectionTaskNames[context.task]} 不匹配`;
    $("collection-task").focus();
    return;
  }
  if (isModel && !label) {
    $("collection-result").textContent = "视觉和哭声音频属于正式模型数据，请先输入本轮标签";
    $("collection-label-help").textContent = "模型数据标签必填，例如 crying、non_crying、sleeping 或 face_covered。";
    $("collection-label").focus();
    return;
  }
  const description = $("collection-description").value.trim();
  const workflow = isModel ? "model_dataset" : "reference_signal";
  const modelFamily = kind === "camera" ? "vision" : kind === "audio" ? "cry_audio" : "rule_reference";
  const contextName = label || description || new Date().toLocaleString();
  const created = await api("/api/dashboard/sessions", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({
      device_id: state.device || "demo-device",
      name: `${contextName} · ${collectionNames[kind]}`,
      description,
      target_labels: label ? [label] : [],
      metadata: {
        capture_label: label || null, data_kind: kind, workflow, model_family: modelFamily,
        task: context.task, subject: context.subject, posture: context.posture,
        bed_region: context.bed_region, blanket_state: context.blanket_state,
        heater_state: context.heater_state, audio_scene: context.audio_scene,
        caregiver: context.caregiver, repeat_id: context.repeat_id,
        default_split: isModel ? context.default_split : "unassigned",
      },
    }),
  });
  await api(`/api/dashboard/sessions/${created.session_id}/start?kind=${kind}`, {method: "POST"});
  $("collection-result").textContent = `${collectionNames[kind]} 正在采集 · ${label || "未设置场景标记"}`;
  $("collection-label-help").textContent = isModel
    ? "模型媒体先保存在 RA8 SD，停止后进入对应会话导入原文件。"
    : "参考信号将保留原始时间序列/矩阵，不会被默认混入模型训练集。";
  await refresh(true);
}

document.querySelectorAll("#collection-channels button[data-action]").forEach(button => {
  button.onclick = () => collectionAction(
    button.closest("article").dataset.kind,
    button.dataset.action,
  ).catch(error => { $("collection-result").textContent = error.message; });
});

$("sd-import-files").onchange = event => {
  const count = event.target.files.length;
  $("sd-import-result").textContent = count ? `已选择 ${count} 个文件，等待上传` : "尚未选择文件";
};
$("sd-import-start").onclick = importSdFiles;

$("model-form").onsubmit = async event => {
  event.preventDefault();
  const file = $("model-file").files[0];
  if (!file) return;
  const query = new URLSearchParams({
    name: $("model-name").value,
    version: $("model-version").value,
    target: $("model-target").value || "ra8p1",
    task: $("model-task").value,
  });
  $("model-result").textContent = "上传中";
  try {
    const result = await api(`/api/dashboard/models?${query}`, {
      method: "POST",
      headers: {"Content-Type": "application/zip"},
      body: file,
    });
    $("model-result").textContent = `已保存 ${result.model_id}`;
    event.target.reset();
    scheduleRefresh();
  } catch (error) {
    $("model-result").textContent = error.message;
  }
};

document.querySelectorAll("[data-command]").forEach(button => {
  button.onclick = () => sendCommand(
    button.dataset.command,
    button.dataset.payload ? {payload: button.dataset.payload} : {},
  ).catch(error => { $("command-result").textContent = error.message; });
});
$("send-command").onclick = () => {
  const payload = $("raw-command").value.trim();
  if (payload) sendCommand("ra8.command", {payload}).catch(console.error);
};
$("warm-light-toggle").onchange = event => {
  setWarmLight(event.target.checked).catch(console.error);
};
$("soother-toggle").onchange = event => {
  setSoother(event.target.checked).catch(console.error);
};
$("speaker-volume").oninput = event => {
  $("speaker-volume-value").value = `${Math.round(Number(event.target.value))}%`;
};
$("speaker-volume").onchange = async event => {
  if (state.speakerVolume.pending) return;
  const volume = Math.max(0, Math.min(100, Math.round(Number(event.target.value))));
  state.speakerVolume.pending = true;
  $("speaker-volume").disabled = true;
  $("speaker-volume-result").textContent = "正在下发";
  try {
    const result = await sendCommand("ra8.command", {payload: `volume${volume}`});
    $("speaker-volume-result").textContent = `${volume}% · 命令 ${result.id} 已排队`;
  } catch (error) {
    $("speaker-volume-result").textContent = error.message;
  } finally {
    state.speakerVolume.pending = false;
    $("speaker-volume").disabled = false;
  }
};
$("ai-story-form").onsubmit = async event => {
  event.preventDefault();
  const button = event.submitter || event.target.querySelector("button[type=submit]");
  button.disabled = true;
  $("ai-story-result").textContent = "正在提交模型任务";
  try {
    const requestId = globalThis.crypto?.randomUUID?.() || `${Date.now()}-${Math.random()}`;
    const result = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/ai/stories`, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({
        theme: $("ai-story-theme").value.trim(),
        child_age_months: Number($("ai-story-age").value),
        duration_seconds: Number($("ai-story-duration").value),
        style: $("ai-story-style").value,
        autoplay: $("ai-story-autoplay").checked,
        request_id: requestId,
      }),
    });
    $("ai-story-result").textContent = result.duplicate ? "已找到相同请求" : "故事任务已进入队列";
    await loadAi(true);
  } catch (error) {
    $("ai-story-result").textContent = error.message;
  } finally {
    button.disabled = false;
  }
};
$("ai-assess-now").onclick = async () => {
  $("ai-assess-now").disabled = true;
  $("ai-assessment-result").textContent = "正在排队评估当前遥测";
  try {
    const result = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/ai/assessment`, {method: "POST"});
    $("ai-assessment-result").textContent = result.already_pending ? "已有评估正在执行" : "评估任务已进入队列";
    setTimeout(() => loadAi(true).catch(console.error), 1200);
  } catch (error) {
    $("ai-assessment-result").textContent = error.message;
  } finally {
    $("ai-assess-now").disabled = false;
  }
};
$("copy-json").onclick = () => navigator.clipboard.writeText(JSON.stringify(state.snapshot?.telemetry || {}, null, 2));
$("video-toggle").onclick = startVideo;
$("audio-toggle").onclick = startAudio;
$("overlay-save").onclick = async () => {
  const sourcePoints = [[0, 0], [0, 0], [0, 0], [0, 0]];
  const targetPoints = [[0, 0], [0, 0], [0, 0], [0, 0]];
  const thermalSourcePoints = [[0, 0], [0, 0]];
  const thermalTargetPoints = [[0, 0], [0, 0]];
  document.querySelectorAll("[data-overlay-point]").forEach(input => {
    const points = input.dataset.overlaySide === "source" ? sourcePoints : targetPoints;
    points[Number(input.dataset.overlayPoint)][Number(input.dataset.overlayAxis)] = Number(input.value);
  });
  document.querySelectorAll("[data-thermal-point]").forEach(input => {
    const points = input.dataset.thermalSide === "source" ? thermalSourcePoints : thermalTargetPoints;
    points[Number(input.dataset.thermalPoint)][Number(input.dataset.thermalAxis)] = Number(input.value);
  });
  $("overlay-save-result").textContent = "保存中";
  try {
    const result = await api(`/api/dashboard/devices/${encodeURIComponent(state.device)}/overlay-calibration`, {
      method: "PUT",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({
        enabled: $("overlay-enabled").checked,
        mode: $("overlay-mode").value,
        source_points: sourcePoints,
        target_points: targetPoints,
        thermal_enabled: $("thermal-overlay-enabled").checked,
        thermal_source_points: thermalSourcePoints,
        thermal_target_points: thermalTargetPoints,
        thermal_opacity: Number($("thermal-overlay-opacity").value),
      }),
    });
    state.snapshot.overlay_calibration = result.calibration;
    state.snapshot.vision_overlay = buildBrowserVisionOverlay(state.snapshot.telemetry, result.calibration);
    state.calibrationKey = "";
    renderOverlayCalibration(result.calibration);
    renderLiveVisionOverlay();
    $("overlay-save-result").textContent = "已保存";
  } catch (error) {
    $("overlay-save-result").textContent = error.message;
  }
};
$("thermal-overlay-opacity").oninput = event => {
  $("thermal-overlay-opacity-value").value = `${Math.round(Number(event.target.value) * 100)}%`;
};
$("pressure-calibrate").onclick = () => {
  state.pressureDisplay.calibrationFrames = [];
  state.pressureDisplay.calibrationRemaining = 20;
  state.pressureDisplay.calibrationSequence = null;
  resetPressureHold();
  updatePressureCalibrationState();
};
$("pressure-calibration-clear").onclick = () => {
  state.pressureDisplay.baseline = null;
  state.pressureDisplay.calibrationFrames = [];
  state.pressureDisplay.calibrationRemaining = 0;
  state.pressureDisplay.calibrationSequence = null;
  resetPressureHold();
  savePressureCalibration();
  updatePressureCalibrationState();
  if (state.snapshot?.matrices?.pressure) heatmap("pressure", state.snapshot.matrices.pressure);
};
$("pressure-threshold").oninput = event => {
  state.pressureDisplay.threshold = Number(event.target.value);
  $("pressure-threshold-value").value = event.target.value;
  resetPressureHold();
  savePressureCalibration();
  if (state.snapshot?.matrices?.pressure) heatmap("pressure", state.snapshot.matrices.pressure);
};
$("pressure-sensitivity").oninput = event => {
  state.pressureDisplay.sensitivity = Number(event.target.value);
  $("pressure-sensitivity-value").value = `${event.target.value}×`;
  savePressureCalibration();
  if (state.snapshot?.matrices?.pressure) heatmap("pressure", state.snapshot.matrices.pressure);
};
$("pressure-hold").oninput = event => {
  state.pressureDisplay.holdMs = Number(event.target.value);
  $("pressure-hold-value").value = `${(state.pressureDisplay.holdMs / 1000).toFixed(2).replace(/0+$/, "").replace(/\.$/, "")} s`;
  resetPressureHold();
  savePressureCalibration();
  if (state.snapshot?.matrices?.pressure) heatmap("pressure", state.snapshot.matrices.pressure);
};
$("audio-volume").oninput = event => {
  if (state.audio) state.audio.gain.gain.value = Number(event.target.value);
};
$("environment-threshold-form").onsubmit = saveEnvironmentThresholds;
document.querySelectorAll("[data-recording-kind]").forEach(button => {
  button.onclick = () => sendRecordingControl(
    button.dataset.recordingKind,
    button.dataset.recordingAction,
  );
});
document.querySelectorAll("[data-generate-name]").forEach(button => {
  button.onclick = () => {
    const kind = button.dataset.generateName;
    $(`recording-${kind}-filename`).value = generatedRecordingName(kind);
    updateRecordingPath(kind);
  };
});
for (const kind of ["audio", "camera"]) {
  const input = $(`recording-${kind}-filename`);
  input.value = generatedRecordingName(kind);
  input.oninput = () => updateRecordingPath(kind);
  updateRecordingPath(kind);
}
window.addEventListener("beforeunload", () => {
  clearWarmLightPending();
  clearSootherPatPending();
  stopVideo();
  stopAudio();
  stopRealtimeFallback();
  clearTimeout(state.realtime.retry);
  clearInterval(state.realtime.heartbeat);
  clearInterval(state.ai.pollTimer);
  state.realtime.socket?.close();
});

$("collection-task").onchange = updateCollectionLabelOptions;
$("resource-recording-start").onclick = () => controlResourceRecording("start");
$("resource-recording-stop").onclick = () => controlResourceRecording("stop");
$("collection-suggest-label").onclick = suggestCollectionLabel;
$("collection-subject").onchange = event => {
  if (event.target.value === "absent") $("collection-posture").value = "none";
};
$("empty-bed-calibrate").onclick = async () => {
  if ($("collection-subject").value !== "absent") {
    $("collection-result").textContent = "校准前请移走假人、毛巾和其他载荷，并把假人状态改为空床";
    return;
  }
  $("empty-bed-calibrate").disabled = true;
  try {
    await sendCommand("ra8.command", {payload: "fusion_calibrate_empty"});
    $("collection-result").textContent = "空床校准命令已下发；保持床面空载至少 10 秒";
  } catch (error) {
    $("collection-result").textContent = error.message;
  } finally {
    $("empty-bed-calibrate").disabled = false;
  }
};
updateCollectionLabelOptions();
$("video-preview").onload = layoutLiveVisionOverlay;
window.addEventListener("resize", layoutLiveVisionOverlay);

refresh(true).catch(console.error);
connectDashboardRealtime();
setInterval(() => {
  /* Live telemetry and matrices already arrive through the realtime socket.
   * Reloading the full snapshot (history, logs and matrices) every 15 seconds
   * blocks rendering and makes queued frames appear as a burst afterward. */
  if (state.realtime.socket?.readyState !== WebSocket.OPEN) {
    refresh().catch(console.error);
  }
}, 15000);
setInterval(() => refreshDashboardLiveState(), 2000);
state.ai.pollTimer = setInterval(() => {
  if ($("ai").classList.contains("active")) loadAi().catch(console.error);
}, 3000);
setInterval(() => {
  if (!state.snapshot) return;
  $("telemetry-age").textContent = ageText(state.snapshot.telemetry?.received_at);
  updateMatrixCaption("thermal", state.snapshot.matrices?.thermal);
  updateMatrixCaption("pressure", state.snapshot.matrices?.pressure);
  renderResourceRecording();
}, 1000);
