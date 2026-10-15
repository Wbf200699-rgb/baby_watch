package com.example.baby_watch.ui.home

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Air
import androidx.compose.material.icons.automirrored.filled.ArrowForward
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Bedtime
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.DeviceThermostat
import androidx.compose.material.icons.filled.HealthAndSafety
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.NotificationsActive
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.baby_watch.data.server.AiAdvisory
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.MonitorRepositoryState
import com.example.baby_watch.data.server.MonitorSnapshot
import com.example.baby_watch.data.server.SafetyEvent
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.data.server.TrendSample
import com.example.baby_watch.ui.components.ConnectionBanner
import com.example.baby_watch.ui.components.MetricTile
import com.example.baby_watch.ui.components.ScreenHeader
import com.example.baby_watch.ui.components.SectionHeader
import com.example.baby_watch.ui.theme.AiContainer
import com.example.baby_watch.ui.theme.AiPurple
import com.example.baby_watch.ui.theme.Level1Yellow
import com.example.baby_watch.ui.theme.Level2Purple
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.MonitorDark
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.PrimaryContainer
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.Secondary
import com.example.baby_watch.ui.theme.TextOnDark
import com.example.baby_watch.ui.theme.TextPrimary
import com.example.baby_watch.ui.theme.TextSecondary
import com.example.baby_watch.ui.theme.TextTertiary
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

/**
 * 家长每天进入 App 后的安心总览。
 *
 * 这里刻意不收集视频或音频 StateFlow：媒体只在 MonitorScreen 手动订阅，避免首页
 * 因高频 JPEG/PCM 状态重组，也避免用户只是查看状态时消耗公网带宽。
 */
@Composable
fun HomeScreen(
    modifier: Modifier = Modifier,
    onOpenMonitor: () -> Unit = {},
) {
    val repositoryState by MobileServerRepository.state.collectAsStateWithLifecycle()
    val events by MobileServerRepository.events.collectAsStateWithLifecycle()
    val advisories by MobileServerRepository.advisories.collectAsStateWithLifecycle()
    val history by MobileServerRepository.history.collectAsStateWithLifecycle()
    val snapshot = repositoryState.snapshot
    val deviceOnline = repositoryState.connectionStatus == ServerConnectionStatus.Connected &&
        snapshot.device.online

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        contentPadding = PaddingValues(start = 20.dp, top = 18.dp, end = 20.dp, bottom = 32.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item(key = "header") {
            ScreenHeader(
                eyebrow = greetingEyebrow(),
                title = if (deviceOnline) "宝宝安稳，守护在线" else "正在守护宝宝",
                subtitle = connectionSubtitle(repositoryState),
                trailing = { BabyAvatar(online = deviceOnline) },
            )
        }

        item(key = "connection") {
            ConnectionBanner(repositoryState)
        }

        item(key = "safety-hero") {
            SafetyHero(snapshot = snapshot, deviceOnline = deviceOnline)
        }

        if (snapshot.fusion.schemaVersion >= 5) {
            item(key = "fusion-evidence") {
                FusionEvidenceCard(snapshot = snapshot, deviceOnline = deviceOnline)
            }
        }

        item(key = "monitor-entry") {
            LiveMonitorEntry(
                snapshot = snapshot,
                deviceOnline = deviceOnline,
                onOpenMonitor = onOpenMonitor,
            )
        }

        item(key = "now-title") {
            SectionHeader(
                title = "此刻状态",
                subtitle = "用最重要的四项信息快速确认宝宝与房间状态",
            )
        }

        item(key = "now-primary") {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                MetricTile(
                    label = "哭声",
                    value = cryState(snapshot, deviceOnline),
                    helper = snapshot.inference.cryConfidence
                        .takeIf { deviceOnline }
                        ?.let { "识别置信度 ${"%.0f".format(it)}%" }
                        ?: "等待实时判断",
                    icon = Icons.Default.Mic,
                    tint = if (snapshot.inference.cryConfidence >= 50f) Level1Yellow else SafeGreen,
                    modifier = Modifier.weight(1f),
                )
                MetricTile(
                    label = "在床状态",
                    value = occupiedState(snapshot, deviceOnline),
                    helper = if (snapshot.fusion.schemaVersion >= 5) {
                        val evidence = snapshot.fusion.evidence
                        if (evidence.available) {
                            "${evidence.presenceSourceCount.coerceIn(0, 3)} 路证据支持 · 多模态融合"
                        } else {
                            "融合判断依据正在同步"
                        }
                    } else {
                        snapshot.pressure.activePoints?.let { "$it 个受压点" } ?: "压力融合判断"
                    },
                    icon = Icons.Default.Bedtime,
                    tint = Primary,
                    modifier = Modifier.weight(1f),
                )
            }
        }

        item(key = "now-environment") {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                MetricTile(
                    label = "房间空气",
                    value = co2Value(snapshot, deviceOnline),
                    helper = co2Helper(snapshot, deviceOnline),
                    icon = Icons.Default.Air,
                    tint = SafeGreen,
                    modifier = Modifier.weight(1f),
                )
                MetricTile(
                    label = "环境温度",
                    value = environmentTemperature(snapshot, deviceOnline),
                    helper = "来自房间环境传感器",
                    icon = Icons.Default.DeviceThermostat,
                    tint = Secondary,
                    modifier = Modifier.weight(1f),
                )
            }
        }

        events.firstOrNull(SafetyEvent::requiresAttention)?.let { event ->
            item(key = "pending-event-${event.id}") {
                PendingEventCard(event)
            }
        }

        if (repositoryState.capabilities.aiEnabled) {
            advisories.firstOrNull { !it.acknowledged }?.let { advisory ->
                item(key = "ai-${advisory.advisoryId}") {
                    AiAdvisoryCard(advisory)
                }
            }
        }

        item(key = "summary-title") {
            SectionHeader(
                title = "今日监护摘要",
                subtitle = "基于本机今天已同步的数据，不代替医疗判断",
            )
        }

        item(key = "summary") {
            TodaySummaryCard(history = history, events = events, snapshot = snapshot)
        }
    }
}

@Composable
private fun BabyAvatar(online: Boolean) {
    Box(
        modifier = Modifier
            .size(48.dp)
            .clip(CircleShape)
            .background(PrimaryContainer),
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Default.HealthAndSafety, null, tint = Primary, modifier = Modifier.size(25.dp))
        Box(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .size(12.dp)
                .clip(CircleShape)
                .background(if (online) SafeGreen else TextTertiary),
        )
    }
}

@Composable
private fun SafetyHero(snapshot: MonitorSnapshot, deviceOnline: Boolean) {
    val fusionV5 = snapshot.fusion.schemaVersion >= 5
    val severity = if (fusionV5) {
        snapshot.fusion.alarm.level.coerceIn(0, 3)
    } else {
        snapshot.inference.severity
    }
    val accent = if (deviceOnline) {
        if (fusionV5) alarmLevelColor(severity) else severityColor(severity)
    } else {
        TextTertiary
    }
    val alarm = snapshot.fusion.alarm
    val heroTitle = when {
        !deviceOnline -> "设备暂时离线"
        fusionV5 -> fusionAlarmTitle(alarm.primaryLabel, severity)
        else -> snapshot.inference.stateLabel
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(28.dp),
        colors = CardDefaults.cardColors(containerColor = Color.Transparent),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Column(
            modifier = Modifier
                .background(
                    Brush.linearGradient(
                        listOf(MonitorDark, Color(0xFF17483F), Color(0xFF2C6257))
                    )
                )
                .padding(22.dp),
            verticalArrangement = Arrangement.spacedBy(18.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(52.dp)
                        .clip(CircleShape)
                        .background(accent.copy(alpha = 0.22f)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        if (deviceOnline && severity == 0) Icons.Default.Shield else Icons.Default.HealthAndSafety,
                        null,
                        tint = if (deviceOnline) Color.White else Color.White.copy(alpha = 0.55f),
                        modifier = Modifier.size(28.dp),
                    )
                }
                Spacer(Modifier.width(14.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        when {
                            !deviceOnline -> "最近一次监护状态"
                            fusionV5 -> "多模态安全判断"
                            else -> "服务端融合判断"
                        },
                        style = MaterialTheme.typography.labelMedium,
                        color = TextOnDark.copy(alpha = 0.68f),
                    )
                    Text(
                        heroTitle,
                        style = MaterialTheme.typography.headlineMedium,
                        color = TextOnDark,
                    )
                }
                Text(
                    if (deviceOnline && fusionV5) {
                        "L$severity ${alarmLevelLabel(severity)}"
                    } else if (deviceOnline) {
                        severityLabel(severity)
                    } else {
                        "非实时"
                    },
                    modifier = Modifier
                        .background(accent.copy(alpha = 0.22f), RoundedCornerShape(50))
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    style = MaterialTheme.typography.labelMedium,
                    color = TextOnDark,
                )
            }

            Text(
                if (deviceOnline) {
                    if (fusionV5) {
                        "${presenceDecision(snapshot)} · ${diagnosticsLabel(snapshot.diagnosticsOverall)}"
                    } else {
                        "融合置信度 ${"%.1f".format(snapshot.inference.confidence)}% · ${diagnosticsLabel(snapshot.diagnosticsOverall)}"
                    }
                } else {
                    "我们会持续尝试恢复连接。恢复在线后，实时监护将自动继续。"
                },
                style = MaterialTheme.typography.bodyMedium,
                color = TextOnDark.copy(alpha = 0.78f),
            )

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                HeroSignal("设备", if (deviceOnline) "在线" else "离线", deviceOnline)
                HeroSignal("床垫", if (snapshot.device.matrixConnected) "已连接" else "等待", snapshot.device.matrixConnected)
                HeroSignal("传感器", diagnosticsShortLabel(snapshot.diagnosticsOverall), snapshot.diagnosticsOverall.lowercase() in setOf("ok", "healthy", "normal"))
            }
        }
    }
}

@Composable
private fun FusionEvidenceCard(snapshot: MonitorSnapshot, deviceOnline: Boolean) {
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    val activeTypes = fusion.activeIncidents.mapTo(mutableSetOf()) { it.type }
    val showAirwayEvidence = fusion.alarm.primaryType in 4..6 || activeTypes.any { it in 4..6 }
    val showEdgeEvidence = fusion.alarm.primaryType in 7..8 || activeTypes.any { it in 7..8 }
    val feverScreeningSupported = fusion.schemaVersion >= 8
    val feverScreeningValid = feverScreeningEvidenceValid(snapshot)
    val bodyTemperatureLabel = fusion.contexts["body_temperature"]?.label.orEmpty()

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .background(PrimaryContainer, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.AutoAwesome, null, tint = Primary, modifier = Modifier.size(20.dp))
                }
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("判断依据", style = MaterialTheme.typography.titleMedium)
                    Text(
                        if (deviceOnline) "压力、视觉与热成像共同判断" else "最近一次融合依据",
                        style = MaterialTheme.typography.bodySmall,
                        color = TextSecondary,
                    )
                }
                Text(
                    "Fusion v${fusion.schemaVersion}",
                    style = MaterialTheme.typography.labelSmall,
                    color = TextTertiary,
                )
            }

            if (!evidence.available) {
                Text(
                    "融合证据正在同步，暂不把缺失读数判断为正常。",
                    style = MaterialTheme.typography.bodyMedium,
                    color = TextSecondary,
                )
            } else {
                Text(
                    presenceDecision(snapshot),
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = presenceDecisionColor(snapshot),
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    EvidenceStrength(
                        label = "压力",
                        strengthX1000 = evidence.pressurePresenceX1000,
                        available = fusion.pressureEvidenceAvailable,
                        modifier = Modifier.weight(1f),
                    )
                    EvidenceStrength(
                        label = "视觉",
                        strengthX1000 = evidence.visionPresenceX1000,
                        available = fusion.visionEvidenceAvailable,
                        modifier = Modifier.weight(1f),
                    )
                    EvidenceStrength(
                        label = "热成像",
                        strengthX1000 = evidence.thermalPresenceX1000,
                        available = fusion.thermalEvidenceAvailable,
                        modifier = Modifier.weight(1f),
                    )
                }
                if (showAirwayEvidence || showEdgeEvidence) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        if (showAirwayEvidence) {
                            EvidenceStrength(
                                label = "口鼻风险",
                                strengthX1000 = evidence.airwayRiskX1000,
                                available = fusion.visionEvidenceAvailable,
                                modifier = Modifier.weight(1f),
                            )
                        }
                        if (showEdgeEvidence) {
                            EvidenceStrength(
                                label = "床缘风险",
                                strengthX1000 = evidence.edgeRiskX1000,
                                available = fusion.visionEvidenceAvailable,
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                }
                if (feverScreeningSupported) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(
                                if (bodyTemperatureLabel.equals("high", ignoreCase = true) && feverScreeningValid) {
                                    Level1Yellow.copy(alpha = 0.12f)
                                } else {
                                    MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f)
                                },
                                RoundedCornerShape(12.dp),
                            )
                            .padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(3.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                "体表温度筛查",
                                modifier = Modifier.weight(1f),
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.SemiBold,
                                color = TextPrimary,
                            )
                            Text(
                                if (feverScreeningValid) {
                                    "%.1f°C".format(Locale.ROOT, evidence.thermalHotspot.meanC)
                                } else {
                                    "--"
                                },
                                style = MaterialTheme.typography.titleMedium,
                                color = if (feverScreeningValid) {
                                    if (bodyTemperatureLabel.equals("high", ignoreCase = true)) Level1Yellow else SafeGreen
                                } else {
                                    TextTertiary
                                },
                            )
                        }
                        Text(
                            feverScreeningStatus(snapshot),
                            style = MaterialTheme.typography.labelSmall,
                            color = if (feverScreeningValid) TextSecondary else TextTertiary,
                        )
                        Text(
                            "热成像仅作非接触筛查；疑似发热时请使用合格体温计复测。",
                            style = MaterialTheme.typography.labelSmall,
                            color = TextTertiary,
                        )
                    }
                }
                Text(
                    "证据强度表示当前传感器对判断的支持程度，不代表医学概率。",
                    style = MaterialTheme.typography.labelSmall,
                    color = TextTertiary,
                )
            }
        }
    }
}

@Composable
private fun EvidenceStrength(
    label: String,
    strengthX1000: Int,
    available: Boolean = true,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f), RoundedCornerShape(12.dp))
            .padding(horizontal = 10.dp, vertical = 9.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = TextSecondary)
        Text(
            if (available) "证据强度 ${evidenceStrengthLabel(strengthX1000)}" else "来源暂不可用",
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.SemiBold,
            color = if (available) TextSecondary else TextTertiary,
            maxLines = 1,
        )
    }
}

@Composable
private fun HeroSignal(label: String, value: String, healthy: Boolean) {
    Row(
        modifier = Modifier
            .background(Color.White.copy(alpha = 0.08f), RoundedCornerShape(50))
            .padding(horizontal = 10.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(7.dp)
                .clip(CircleShape)
                .background(if (healthy) Color(0xFF77E0B6) else Color(0xFFFFC477))
        )
        Spacer(Modifier.width(6.dp))
        Text(
            "$label $value",
            style = MaterialTheme.typography.labelSmall,
            color = TextOnDark.copy(alpha = 0.86f),
            maxLines = 1,
        )
    }
}

@Composable
private fun LiveMonitorEntry(
    snapshot: MonitorSnapshot,
    deviceOnline: Boolean,
    onOpenMonitor: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp),
    ) {
        Column {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(164.dp)
                    .background(
                        Brush.linearGradient(
                            listOf(Color(0xFF0D1E1B), Color(0xFF1B3933))
                        )
                    ),
                contentAlignment = Alignment.Center,
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Box(
                        modifier = Modifier
                            .size(58.dp)
                            .clip(CircleShape)
                            .background(Color.White.copy(alpha = 0.10f)),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(Icons.Default.Videocam, null, tint = Color.White, modifier = Modifier.size(30.dp))
                    }
                    Spacer(Modifier.height(10.dp))
                    Text(
                        "媒体流默认关闭",
                        style = MaterialTheme.typography.titleMedium,
                        color = Color.White,
                    )
                    Text(
                        "进入监护后由你手动开启，不占用首页带宽",
                        style = MaterialTheme.typography.bodySmall,
                        color = Color.White.copy(alpha = 0.62f),
                    )
                }
                Text(
                    if (deviceOnline) "设备在线" else "设备离线",
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .padding(14.dp)
                        .background(Color.Black.copy(alpha = 0.34f), RoundedCornerShape(50))
                        .padding(horizontal = 9.dp, vertical = 5.dp),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (deviceOnline) Color(0xFF9CE7CA) else Color.White.copy(alpha = 0.7f),
                )
            }

            Row(
                modifier = Modifier.padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("实时监护", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "视频 ${availabilityLabel(snapshot.videoAvailable)} · 音频 ${availabilityLabel(snapshot.audioAvailable)}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Button(
                    onClick = onOpenMonitor,
                    shape = RoundedCornerShape(14.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = Primary),
                    contentPadding = PaddingValues(horizontal = 14.dp, vertical = 10.dp),
                ) {
                    Text("进入监护")
                    Spacer(Modifier.width(4.dp))
                    Icon(Icons.AutoMirrored.Filled.ArrowForward, null, modifier = Modifier.size(18.dp))
                }
            }
        }
    }
}

@Composable
private fun PendingEventCard(event: SafetyEvent) {
    val color = severityColor(event.severity)
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = color.copy(alpha = 0.09f)),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Row(
            modifier = Modifier.padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier
                    .size(42.dp)
                    .background(color.copy(alpha = 0.14f), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Default.NotificationsActive, null, tint = color)
            }
            Spacer(Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("待核查事件 · ${severityLabel(event.severity)}", style = MaterialTheme.typography.labelMedium, color = color)
                Text(
                    event.title,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text("请先确认宝宝实际情况，再前往事件页处理。", style = MaterialTheme.typography.bodySmall, color = TextSecondary)
            }
        }
    }
}

@Composable
private fun AiAdvisoryCard(advisory: AiAdvisory) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = AiContainer),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.AutoAwesome, null, tint = AiPurple, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text("AI 辅助建议", modifier = Modifier.weight(1f), style = MaterialTheme.typography.labelLarge, color = AiPurple)
                Text("${(advisory.confidence * 100).toInt()}%", style = MaterialTheme.typography.labelSmall, color = TextSecondary)
            }
            Text(advisory.title.ifBlank { "状态提示" }, style = MaterialTheme.typography.titleMedium)
            Text(advisory.summary, style = MaterialTheme.typography.bodyMedium, color = TextSecondary)
            advisory.actions.take(2).forEach { action ->
                Text("• $action", style = MaterialTheme.typography.bodySmall)
            }
            Text(
                "辅助建议不代替医疗诊断，硬件安全告警始终优先。",
                style = MaterialTheme.typography.labelSmall,
                color = TextTertiary,
            )
            TextButton(
                onClick = { MobileServerRepository.acknowledgeAdvisory(advisory.advisoryId) },
                modifier = Modifier.align(Alignment.End),
            ) {
                Icon(Icons.Default.CheckCircle, null, modifier = Modifier.size(17.dp))
                Spacer(Modifier.width(5.dp))
                Text("我已查看")
            }
        }
    }
}

@Composable
private fun TodaySummaryCard(
    history: List<TrendSample>,
    events: List<SafetyEvent>,
    snapshot: MonitorSnapshot,
) {
    val calendar = Calendar.getInstance().apply {
        set(Calendar.HOUR_OF_DAY, 0)
        set(Calendar.MINUTE, 0)
        set(Calendar.SECOND, 0)
        set(Calendar.MILLISECOND, 0)
    }
    val todaySamples = history.filter { it.timestampMs >= calendar.timeInMillis }
    val todayPrefix = SimpleDateFormat("yyyy-MM-dd", Locale.getDefault()).format(Date())
    val todayEvents = events.count { it.receivedAt.startsWith(todayPrefix) }
    val peakSeverity = todaySamples.maxOfOrNull { it.severity } ?: snapshot.inference.severity
    val averageTemperature = todaySamples.mapNotNull { it.temperatureC }.takeIf { it.isNotEmpty() }?.average()
    val averageCry = todaySamples.takeIf { it.isNotEmpty() }?.map { it.cryConfidence }?.average()

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp),
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                SummaryValue("最高风险", severityLabel(peakSeverity), severityColor(peakSeverity), Modifier.weight(1f))
                SummaryValue("环境均温", averageTemperature?.let { "%.1f°C".format(it) } ?: "--", Secondary, Modifier.weight(1f))
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                SummaryValue("哭声均值", averageCry?.let { "%.0f%%".format(it) } ?: "--", AiPurple, Modifier.weight(1f))
                SummaryValue("异常事件", "$todayEvents 条", if (todayEvents > 0) Level1Yellow else SafeGreen, Modifier.weight(1f))
            }
            Text(
                if (todaySamples.isEmpty()) "等待今天的趋势数据同步" else "已汇总 ${todaySamples.size} 个本地趋势点",
                style = MaterialTheme.typography.labelSmall,
                color = TextTertiary,
            )
        }
    }
}

@Composable
private fun SummaryValue(label: String, value: String, color: Color, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .background(color.copy(alpha = 0.09f), RoundedCornerShape(16.dp))
            .padding(13.dp),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = TextSecondary)
        Text(value, style = MaterialTheme.typography.titleMedium, color = color)
    }
}

private fun greetingEyebrow(): String {
    val hour = Calendar.getInstance().get(Calendar.HOUR_OF_DAY)
    return when (hour) {
        in 5..10 -> "早上好 · 安心守护"
        in 11..13 -> "中午好 · 安心守护"
        in 14..17 -> "下午好 · 安心守护"
        else -> "晚上好 · 安心守护"
    }
}

private fun connectionSubtitle(state: MonitorRepositoryState): String = when (state.connectionStatus) {
    ServerConnectionStatus.NotConfigured -> "完成设备配置后开始智能监护"
    ServerConnectionStatus.Connecting -> "正在安全连接监护服务…"
    ServerConnectionStatus.Error -> state.errorMessage ?: "连接暂时异常，系统正在重试"
    ServerConnectionStatus.Connected -> if (state.snapshot.device.online) {
        "${state.snapshot.device.hardware.ifBlank { state.snapshot.device.id }} · 数据持续同步"
    } else {
        "服务端已连接，监护设备暂时离线"
    }
}

private fun cryState(snapshot: MonitorSnapshot, online: Boolean): String = when {
    !online -> "非实时"
    snapshot.inference.cryConfidence >= 70f -> "检测到哭声"
    snapshot.inference.cryConfidence >= 35f -> "需要留意"
    else -> "安静"
}

private fun occupiedState(snapshot: MonitorSnapshot, online: Boolean): String = when {
    !online -> "非实时"
    snapshot.fusion.schemaVersion >= 5 -> presenceDecision(snapshot)
    snapshot.inference.occupied == true || snapshot.pressure.occupied == true -> "宝宝在床"
    snapshot.inference.occupied == false || snapshot.pressure.occupied == false -> "宝宝离床"
    else -> "判断中"
}

private fun presenceDecision(snapshot: MonitorSnapshot): String {
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    if (!evidence.available) return "判断依据暂不可用"

    val sourceCount = evidence.presenceSourceCount.coerceIn(0, 3)
    val context = fusion.contexts["presence"]
    val semantic = "${context?.code ?: 0} ${context?.label.orEmpty()}".lowercase(Locale.ROOT)
    val indicatesAbsent = listOf(
        "absent", "empty", "off_bed", "out_of_bed", "not_occupied", "离床", "空床", "无人",
    ).any(semantic::contains)
    val indicatesPresent = !indicatesAbsent && listOf(
        "present", "occupied", "in_bed", "confirmed", "在床", "有人",
    ).any(semantic::contains)

    return when {
        indicatesPresent && sourceCount >= 2 -> "宝宝在床 · ${sourceCount}路证据一致"
        indicatesAbsent -> "宝宝可能离床"
        else -> "在床状态暂不确定 · 当前${sourceCount}路支持"
    }
}

private fun presenceDecisionColor(snapshot: MonitorSnapshot): Color {
    val fusion = snapshot.fusion
    if (!fusion.evidence.available) return TextSecondary
    val label = fusion.contexts["presence"]?.label?.lowercase(Locale.ROOT).orEmpty()
    return when {
        label == "present" && fusion.evidence.presenceSourceCount >= 2 -> SafeGreen
        label == "absent" -> Level2Purple
        else -> Level1Yellow
    }
}

private fun evidenceStrengthLabel(valueX1000: Int): String =
    "%.0f%%".format(Locale.ROOT, valueX1000.coerceIn(0, 1000) / 10f)

private fun fusionAlarmTitle(primaryLabel: String, level: Int): String {
    if (level <= 0) return "监护状态正常"
    return when (primaryLabel.trim().lowercase(Locale.ROOT)) {
        "presence_conflict" -> "在床证据需要确认"
        "sensor_degraded" -> "部分传感器需要检查"
        "monitor_blind" -> "监护视野暂时受限"
        "airway_not_visible" -> "口鼻区域暂不可见"
        "airway_partial" -> "口鼻区域部分遮挡"
        "airway_covered" -> "请检查口鼻遮挡"
        "edge_near" -> "宝宝靠近床缘"
        "edge_climbing" -> "检测到床缘攀爬风险"
        "out_of_bed" -> "宝宝可能离床"
        "cry" -> "检测到宝宝哭声"
        "persistent_cry" -> "宝宝持续哭泣"
        "overheat" -> "床面温度偏高"
        "underheat" -> "床面温度偏低"
        "camera_frozen" -> "摄像头画面异常"
        "environment_air" -> "房间空气需要关注"
        "environment_co2_high" -> "室内 CO₂ 浓度过高"
        "environment_temperature_high" -> "室温过高"
        "environment_humidity_high" -> "湿度过高"
        "environment_humidity_low" -> "湿度过低"
        "environment_voc_suspected" -> "疑似 VOC 升高，请复核原始气阻"
        "visual_subject_hidden" -> "视觉证据不足，疑似婴儿被完全覆盖"
        "suspected_fever" -> "疑似发热，请复测确认"
        else -> "监护状态需要关注"
    }
}

private fun feverScreeningEvidenceValid(snapshot: MonitorSnapshot): Boolean {
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    val bodyTemperature = fusion.contexts["body_temperature"]?.label?.trim()?.lowercase(Locale.ROOT)
    val presence = fusion.contexts["presence"]?.label?.trim()?.lowercase(Locale.ROOT)
    return fusion.schemaVersion >= 8 &&
        evidence.available &&
        fusion.thermalEvidenceAvailable &&
        presence == "present" &&
        evidence.thermalPresenceX1000 >= 550 &&
        evidence.thermalHotspot.points >= 4 &&
        evidence.thermalHotspot.meanC?.isFinite() == true &&
        bodyTemperature in setOf("normal", "high")
}

private fun feverScreeningStatus(snapshot: MonitorSnapshot): String {
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    if (!fusion.thermalEvidenceAvailable) return "等待有效热成像数据"
    if (!fusion.contexts["presence"]?.label.equals("present", ignoreCase = true)) {
        return "宝宝在床证据不足，当前不作体温筛查判断"
    }
    if (evidence.thermalPresenceX1000 < 550 || evidence.thermalHotspot.points < 4) {
        return "热源证据不足，当前不作体温筛查判断"
    }
    return when (fusion.contexts["body_temperature"]?.label?.trim()?.lowercase(Locale.ROOT)) {
        "high" -> "筛查结果偏高，请尽快用体温计复测"
        "normal" -> "当前筛查未见温度偏高"
        else -> "服务端尚未形成有效筛查结论"
    }
}

private fun co2Value(snapshot: MonitorSnapshot, online: Boolean): String = when {
    !online -> "非实时"
    snapshot.scd41.valid && snapshot.scd41.co2Ppm != null -> "${snapshot.scd41.co2Ppm} ppm"
    else -> "--"
}

private fun co2Helper(snapshot: MonitorSnapshot, online: Boolean): String = when {
    !online -> "显示最近环境数据"
    !snapshot.scd41.valid || snapshot.scd41.co2Ppm == null -> "等待有效的空气数据"
    snapshot.scd41.co2Ppm < 800 -> "空气状态舒适"
    snapshot.scd41.co2Ppm < 1200 -> "建议适当通风"
    else -> "建议尽快通风"
}

private fun environmentTemperature(snapshot: MonitorSnapshot, online: Boolean): String {
    if (!online) return "非实时"
    val value = snapshot.bme688.temperatureC.takeIf { snapshot.bme688.climateValid }
        ?: snapshot.scd41.temperatureC.takeIf { snapshot.scd41.valid }
    return value?.let { "%.1f°C".format(it) } ?: "--"
}

private fun availabilityLabel(available: Boolean): String = if (available) "可用" else "等待"

private fun diagnosticsLabel(value: String): String = when (value.lowercase()) {
    "ok", "healthy", "normal" -> "传感器正常"
    "degraded", "warning" -> "部分传感器需留意"
    "offline" -> "数据暂时中断"
    "error", "fault" -> "传感器异常"
    else -> "诊断待同步"
}

private fun diagnosticsShortLabel(value: String): String = when (value.lowercase()) {
    "ok", "healthy", "normal" -> "正常"
    "degraded", "warning" -> "留意"
    "offline" -> "离线"
    "error", "fault" -> "异常"
    else -> "待确认"
}

private fun severityLabel(severity: Int): String = when (severity) {
    0 -> "正常"
    1 -> "留意"
    2 -> "提醒"
    3 -> "警告"
    4 -> "高危"
    else -> "紧急"
}

private fun severityColor(severity: Int): Color = when (severity) {
    0 -> SafeGreen
    1, 2 -> Level1Yellow
    3, 4 -> Level2Purple
    else -> Level3Red
}

private fun alarmLevelLabel(level: Int): String = when (level.coerceIn(0, 3)) {
    0 -> "正常"
    1 -> "提醒"
    2 -> "警告"
    else -> "紧急"
}

private fun alarmLevelColor(level: Int): Color = when (level.coerceIn(0, 3)) {
    0 -> SafeGreen
    1 -> Level1Yellow
    2 -> Level2Purple
    else -> Level3Red
}
