package com.example.baby_watch.ui.monitor

import android.graphics.BitmapFactory
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeOff
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.AutoStories
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Headphones
import androidx.compose.material.icons.filled.Lightbulb
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Replay
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Vibration
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.baby_watch.data.server.AiStory
import com.example.baby_watch.data.server.AudioPlayerSummary
import com.example.baby_watch.data.server.AudioStreamState
import com.example.baby_watch.data.server.MobileCommandStatus
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.MonitorSnapshot
import com.example.baby_watch.data.server.RelayActuatorSummary
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.data.server.VideoStreamState
import com.example.baby_watch.data.server.VisionOverlaySummary
import com.example.baby_watch.ui.components.ConnectionBanner
import com.example.baby_watch.ui.components.ScreenHeader
import com.example.baby_watch.ui.components.SectionHeader
import com.example.baby_watch.ui.theme.AiContainer
import com.example.baby_watch.ui.theme.AiPurple
import com.example.baby_watch.ui.theme.Level1Yellow
import com.example.baby_watch.ui.theme.Level2Purple
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.MonitorDark
import com.example.baby_watch.ui.theme.MonitorSurface
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.PrimaryContainer
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.Secondary
import com.example.baby_watch.ui.theme.TextOnDark
import com.example.baby_watch.ui.theme.TextSecondary
import com.example.baby_watch.ui.theme.TextTertiary
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext

private data class AudioUiState(
    val wanted: Boolean,
    val connected: Boolean,
    val receiving: Boolean,
    val muted: Boolean,
    val stale: Boolean,
    val reconnectAttempt: Int,
    val errorMessage: String?,
)

/**
 * 实时媒体的唯一消费页面。
 *
 * 视频与音频均由用户手动开启，页面离开时立即释放 WSS 和 AudioTrack。视频帧解码在
 * Dispatchers.Default 完成，且高频 StateFlow 只在各自媒体卡片内收集，避免整页重组。
 */
@Composable
fun MonitorScreen(modifier: Modifier = Modifier) {
    val repositoryState by MobileServerRepository.state.collectAsStateWithLifecycle()
    val stories by MobileServerRepository.stories.collectAsStateWithLifecycle()
    val command by MobileServerRepository.lastCommand.collectAsStateWithLifecycle()
    val snapshot = repositoryState.snapshot
    val deviceOnline = repositoryState.connectionStatus == ServerConnectionStatus.Connected &&
        snapshot.device.online
    val storyTransfer = snapshot.storyTransfer
    val storyTransporting = storyTransfer.active ||
        storyTransfer.audioTransportSuspended ||
        storyTransfer.videoTransportSuspended
    val storyPausedMedia = storyTransfer.audioTransportSuspended ||
        storyTransfer.videoTransportSuspended
    val storyMediaRecovering = !storyTransfer.active && storyPausedMedia

    DisposableEffect(Unit) {
        onDispose {
            MobileServerRepository.stopVideo()
            MobileServerRepository.stopAudio()
        }
    }

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        contentPadding = PaddingValues(start = 20.dp, top = 18.dp, end = 20.dp, bottom = 32.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item(key = "header") {
            ScreenHeader(
                eyebrow = "LIVE CARE",
                title = "实时监护",
                subtitle = when {
                    storyMediaRecovering -> "故事已送达，实时音视频正在自动恢复"
                    storyPausedMedia -> "AI 故事正在传输，实时音视频稍后自动恢复"
                    storyTransfer.active -> "AI 故事正在安全下发到设备"
                    deviceOnline -> "媒体按需开启，离开本页自动关闭"
                    else -> "设备离线，等待安全链路恢复"
                },
                trailing = { MediaPrivacyBadge() },
            )
        }

        item(key = "connection") {
            ConnectionBanner(repositoryState)
        }

        if (storyTransporting) {
            item(key = "story-transfer") {
                StoryTransferNotice(
                    active = storyTransfer.active,
                    receivedBytes = storyTransfer.receivedBytes,
                    downloadHadIssue = storyTransfer.downloadError != 0,
                    mediaSuspended = storyPausedMedia,
                )
            }
        }

        item(key = "video") {
            VideoMonitorCard(
                deviceOnline = deviceOnline,
                videoAvailable = snapshot.videoAvailable,
                overlay = snapshot.visionOverlay,
                transportSuspended = storyTransfer.videoTransportSuspended,
            )
        }

        item(key = "audio") {
            AudioListenCard(
                deviceOnline = deviceOnline,
                audioAvailable = snapshot.audioAvailable,
                transportSuspended = storyTransfer.audioTransportSuspended,
            )
        }

        item(key = "recognition-title") {
            SectionHeader(
                title = "智能识别",
                subtitle = "AI 结果用于辅助照护，硬件安全告警始终优先",
            )
        }

        item(key = "recognition") {
            RecognitionCard(snapshot)
        }

        item(key = "comfort-title") {
            SectionHeader(
                title = "安抚与陪伴",
                subtitle = "舒缓设备、摇篮曲与 AI 故事分别控制",
            )
        }

        item(key = "comfort-devices") {
            ComfortDevicesCard(
                warmLight = snapshot.warmLight,
                soother = snapshot.soother,
                latestCommand = command,
                deviceOnline = deviceOnline,
            )
        }

        item(key = "lullaby-player") {
            LullabyPlayerCard(
                player = snapshot.audioPlayer,
                latestCommand = command,
                deviceOnline = deviceOnline,
                storyTransferActive = storyTransporting,
            )
        }

        item(key = "ai-story") {
            AiComfortStoryCard(
                latestStory = stories.firstOrNull(),
                aiEnabled = repositoryState.capabilities.aiEnabled,
                deviceOnline = deviceOnline,
            )
        }
    }
}

@Composable
private fun StoryTransferNotice(
    active: Boolean,
    receivedBytes: Long,
    downloadHadIssue: Boolean,
    mediaSuspended: Boolean,
) {
    val recoveringMedia = !active && mediaSuspended
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer,
        ),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(42.dp)
                    .background(
                        MaterialTheme.colorScheme.secondary.copy(alpha = 0.14f),
                        CircleShape,
                    ),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Default.AutoStories,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.secondary,
                    modifier = Modifier.size(22.dp),
                )
            }
            Spacer(Modifier.width(12.dp))
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                Text(
                    if (recoveringMedia) "实时媒体恢复中" else "AI 故事传输中",
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                )
                Text(
                    when {
                        recoveringMedia -> "故事任务已经结束，设备正在恢复音视频，无需重复开关。"
                        downloadHadIssue -> "设备正在安全收尾本次传输，实时监护会自动恢复。"
                        mediaSuspended -> "为稳定下载故事音频，音视频暂时暂停；稍后自动恢复。"
                        else -> "故事已下载，正在安全发送到设备；实时监护保持可用。"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.76f),
                )
                if (receivedBytes > 0L) {
                    Text(
                        "已接收 ${formatTransferBytes(receivedBytes)}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.secondary,
                    )
                }
            }
        }
    }
}

@Composable
private fun VideoMonitorCard(
    deviceOnline: Boolean,
    videoAvailable: Boolean,
    overlay: VisionOverlaySummary,
    transportSuspended: Boolean,
) {
    val videoState by MobileServerRepository.videoState.collectAsStateWithLifecycle()
    val image by decodedFrame(videoState)
    val pausedForStory = transportSuspended
    val showingFrame = deviceOnline && image != null && !videoState.stale && !pausedForStory
    val overlaySynchronized = overlayMatchesFrame(overlay, videoState.sequence)

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(containerColor = MonitorDark),
        border = videoState.errorMessage
            ?.takeUnless { pausedForStory }
            ?.let { BorderStroke(1.dp, Level3Red) },
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Column {
            BoxWithConstraints(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(4f / 3f)
                    .background(MonitorDark),
                contentAlignment = Alignment.Center,
            ) {
                if (showingFrame) {
                    Image(
                        bitmap = image!!,
                        contentDescription = "宝宝房间服务端实时画面",
                        modifier = Modifier.fillMaxSize(),
                        contentScale = ContentScale.Fit,
                    )

                    if (overlaySynchronized && videoState.width > 0 && videoState.height > 0) {
                        val frameAspect = videoState.width.toFloat() / videoState.height
                        val stageAspect = maxWidth.value / maxHeight.value
                        val visibleWidth = if (frameAspect >= stageAspect) maxWidth else maxHeight * frameAspect
                        val visibleHeight = if (frameAspect >= stageAspect) maxWidth / frameAspect else maxHeight
                        val frameLeft = (maxWidth - visibleWidth) / 2f
                        val frameTop = (maxHeight - visibleHeight) / 2f

                        overlay.boxes.forEach { detection ->
                            val color = detectionColor(detection.classId)
                            Box(
                                modifier = Modifier
                                    .align(Alignment.TopStart)
                                    .offset(
                                        x = frameLeft + visibleWidth * detection.x,
                                        y = frameTop + visibleHeight * detection.y,
                                    )
                                    .size(
                                        width = visibleWidth * detection.width,
                                        height = visibleHeight * detection.height,
                                    )
                                    .border(2.dp, color, RoundedCornerShape(2.dp)),
                            ) {
                                Text(
                                    text = "${detection.label.ifBlank { "类别 ${detection.classId}" }} ${"%.1f".format(detection.scoreX1000 / 10f)}%",
                                    modifier = Modifier
                                        .background(color)
                                        .padding(horizontal = 6.dp, vertical = 3.dp),
                                    style = MaterialTheme.typography.labelSmall,
                                    fontWeight = FontWeight.Bold,
                                    color = Color(0xFF0B1714),
                                    maxLines = 1,
                                )
                            }
                        }
                    }
                } else {
                    MediaPlaceholder(
                        icon = Icons.Default.Videocam,
                        title = videoPlaceholderTitle(
                            state = videoState,
                            deviceOnline = deviceOnline,
                            available = videoAvailable,
                            pausedForStory = pausedForStory,
                        ),
                        subtitle = videoPlaceholderSubtitle(videoState, pausedForStory),
                    )
                }

                StreamBadge(
                    text = when {
                        showingFrame -> "实时画面"
                        pausedForStory -> "故事传输中"
                        videoState.wanted -> "正在连接"
                        else -> "未开启"
                    },
                    active = showingFrame,
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .padding(14.dp),
                )

                if (showingFrame) {
                    OverlayBadge(
                        overlay = overlay,
                        synchronized = overlaySynchronized,
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(14.dp),
                    )
                }
            }

            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(MonitorSurface)
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            when {
                                pausedForStory -> "实时画面暂时暂停"
                                videoState.wanted -> "摄像头监护已开启"
                                else -> "开启实时画面"
                            },
                            style = MaterialTheme.typography.titleMedium,
                            color = TextOnDark,
                        )
                        Text(
                            if (pausedForStory) {
                                "故事安全下发完成后会自动恢复，无需重复操作"
                            } else {
                                "仅在本页按需订阅，不阻塞遥测数据"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = TextOnDark.copy(alpha = 0.58f),
                        )
                    }
                    Switch(
                        checked = videoState.wanted,
                        onCheckedChange = { enabled ->
                            if (enabled) MobileServerRepository.startVideo() else MobileServerRepository.stopVideo()
                        },
                        enabled = deviceOnline && (videoAvailable || videoState.wanted),
                        colors = mediaSwitchColors(),
                    )
                }

                if (videoState.wanted) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        StreamMetric(
                            label = "画质",
                            value = if (videoState.width > 0) "${videoState.width}×${videoState.height}" else "等待",
                            modifier = Modifier.weight(1f),
                        )
                        StreamMetric("帧序列", videoState.sequence.toString(), Modifier.weight(1f))
                        StreamMetric("链路间断", videoState.gapCount.toString(), Modifier.weight(1f))
                    }
                    if (videoState.reconnectAttempt > 0 || videoState.droppedByClient > 0) {
                        Text(
                            "自动重连 ${videoState.reconnectAttempt} 次 · 客户端丢弃 ${videoState.droppedByClient} 帧 · 最新帧优先",
                            style = MaterialTheme.typography.labelSmall,
                            color = Color(0xFFFFD39A),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun decodedFrame(videoState: VideoStreamState) = produceState<ImageBitmap?>(
    initialValue = null,
    key1 = videoState.sequence,
    key2 = videoState.frameBytes,
) {
    val bytes = videoState.frameBytes
    value = if (bytes == null || bytes.isEmpty()) {
        null
    } else {
        withContext(Dispatchers.Default) {
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
        }
    }
}

@Composable
private fun AudioListenCard(
    deviceOnline: Boolean,
    audioAvailable: Boolean,
    transportSuspended: Boolean,
) {
    // PCM 包会高频更新 sequence；UI 只订阅可见状态变化，避免 20 ms 一次无意义重组。
    val audioUiFlow = remember {
        MobileServerRepository.audioState
            .map(AudioStreamState::toUiState)
            .distinctUntilChanged()
    }
    val audioState by audioUiFlow.collectAsStateWithLifecycle(
        initialValue = MobileServerRepository.audioState.value.toUiState()
    )
    val pausedForStory = transportSuspended
    val enabled = deviceOnline && (audioAvailable || audioState.wanted)
    val statusColor = when {
        pausedForStory -> Secondary
        audioState.receiving && !audioState.muted -> SafeGreen
        audioState.wanted -> Level1Yellow
        else -> TextTertiary
    }

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
                        .size(44.dp)
                        .background(statusColor.copy(alpha = 0.12f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        if (audioState.wanted && !audioState.muted) Icons.Default.Headphones else Icons.AutoMirrored.Filled.VolumeOff,
                        null,
                        tint = statusColor,
                    )
                }
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("实时声音", style = MaterialTheme.typography.titleMedium)
                    Text(
                        audioStatus(
                            state = audioState,
                            deviceOnline = deviceOnline,
                            available = audioAvailable,
                            pausedForStory = pausedForStory,
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = TextSecondary,
                    )
                }
                Switch(
                    checked = audioState.wanted,
                    onCheckedChange = { on ->
                        if (on) MobileServerRepository.startAudio() else MobileServerRepository.stopAudio()
                    },
                    enabled = enabled,
                )
            }

            if (audioState.wanted) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(PrimaryContainer, RoundedCornerShape(14.dp))
                        .padding(horizontal = 12.dp, vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(
                        Modifier
                            .size(8.dp)
                            .background(
                                when {
                                    pausedForStory -> Secondary
                                    audioState.receiving -> SafeGreen
                                    else -> Level1Yellow
                                },
                                CircleShape,
                            )
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        when {
                            pausedForStory -> "故事传输中 · 完成后自动继续监听"
                            audioState.receiving -> "正在监听宝宝房间"
                            else -> "等待音频帧"
                        },
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onPrimaryContainer,
                    )
                    IconButton(
                        onClick = { MobileServerRepository.setAudioMuted(!audioState.muted) },
                        modifier = Modifier.size(36.dp),
                    ) {
                        Icon(
                            if (audioState.muted) Icons.AutoMirrored.Filled.VolumeOff else Icons.AutoMirrored.Filled.VolumeUp,
                            contentDescription = if (audioState.muted) "恢复声音" else "静音",
                            tint = Primary,
                        )
                    }
                }
            }

            Text(
                "隐私提示：声音只在你手动开启后播放，离开监护页立即停止。",
                style = MaterialTheme.typography.labelSmall,
                color = TextTertiary,
            )
        }
    }
}

@Composable
private fun RecognitionCard(snapshot: MonitorSnapshot) {
    val overlay = snapshot.visionOverlay
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    val fusionEvidenceAvailable = fusion.schemaVersion >= 5 && evidence.available
    val feverScreeningValid = monitorFeverScreeningEvidenceValid(snapshot)
    val bodyTemperatureLabel = fusion.contexts["body_temperature"]?.label.orEmpty()
    val fusionTitle = if (fusionEvidenceAvailable) {
        monitorFusionAlarmTitle(fusion.alarm.primaryLabel, fusion.alarm.level)
    } else {
        snapshot.inference.stateLabel
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = AiContainer),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(42.dp)
                        .background(AiPurple.copy(alpha = 0.12f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.AutoAwesome, null, tint = AiPurple)
                }
                Spacer(Modifier.width(11.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(fusionTitle, style = MaterialTheme.typography.titleMedium)
                    Text(
                        if (fusionEvidenceAvailable) {
                            val presenceLabel = monitorPresenceLabel(fusion.contexts["presence"]?.label)
                            "Fusion v${fusion.schemaVersion} · $presenceLabel · ${evidence.presenceSourceCount.coerceIn(0, 3)}路支持"
                        } else {
                            "融合置信度 ${"%.1f".format(snapshot.inference.confidence)}%"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = TextSecondary,
                    )
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                RecognitionMetric("哭声", "${"%.0f".format(snapshot.inference.cryConfidence)}%", Modifier.weight(1f))
                RecognitionMetric("目标", if (overlay.expired) "结果过期" else "${overlay.boxes.size} 个", Modifier.weight(1f))
                RecognitionMetric(
                    "风险",
                    if (fusionEvidenceAvailable) {
                        "L${fusion.alarm.level.coerceIn(0, 3)} ${alarmLevelLabel(fusion.alarm.level)}"
                    } else {
                        severityLabel(snapshot.inference.severity)
                    },
                    Modifier.weight(1f),
                    valueColor = if (fusionEvidenceAvailable) {
                        alarmLevelColor(fusion.alarm.level)
                    } else {
                        severityColor(snapshot.inference.severity)
                    },
                )
            }

            if (fusionEvidenceAvailable) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(1.dp)
                        .background(AiPurple.copy(alpha = 0.10f)),
                )
                Text(
                    "多模态证据强度",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = TextSecondary,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RecognitionMetric(
                        "活动",
                        fusionEvidenceStrength(
                            evidence.activityScoreX1000,
                            fusion.pressureEvidenceAvailable || fusion.visionEvidenceAvailable,
                        ),
                        Modifier.weight(1f),
                    )
                    RecognitionMetric(
                        "口鼻风险",
                        fusionEvidenceStrength(evidence.airwayRiskX1000, fusion.visionEvidenceAvailable),
                        Modifier.weight(1f),
                    )
                    RecognitionMetric(
                        "床缘风险",
                        fusionEvidenceStrength(evidence.edgeRiskX1000, fusion.visionEvidenceAvailable),
                        Modifier.weight(1f),
                    )
                }
                if (fusion.schemaVersion >= 8) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(
                                if (feverScreeningValid && bodyTemperatureLabel.equals("high", ignoreCase = true)) {
                                    Level1Yellow.copy(alpha = 0.12f)
                                } else {
                                    Color.White.copy(alpha = 0.58f)
                                },
                                RoundedCornerShape(12.dp),
                            )
                            .padding(horizontal = 12.dp, vertical = 9.dp),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                "体表温度筛查",
                                modifier = Modifier.weight(1f),
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.SemiBold,
                            )
                            Text(
                                if (feverScreeningValid) {
                                    "%.1f°C".format(evidence.thermalHotspot.meanC)
                                } else {
                                    "证据不足"
                                },
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = FontWeight.SemiBold,
                                color = if (feverScreeningValid && bodyTemperatureLabel.equals("high", ignoreCase = true)) {
                                    Level1Yellow
                                } else {
                                    TextSecondary
                                },
                            )
                        }
                        Text(
                            when {
                                !feverScreeningValid -> "未形成有效筛查结论，不显示为正常或异常"
                                bodyTemperatureLabel.equals("high", ignoreCase = true) -> "疑似偏高，请用合格体温计复测"
                                else -> "当前筛查未见温度偏高"
                            },
                            style = MaterialTheme.typography.labelSmall,
                            color = TextTertiary,
                        )
                    }
                }
                Text(
                    "在床一致性 · ${evidence.presenceSourceCount.coerceIn(0, 3)} 路证据支持",
                    style = MaterialTheme.typography.labelSmall,
                    color = TextSecondary,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RecognitionMetric(
                        "压力",
                        fusionEvidenceStrength(evidence.pressurePresenceX1000, fusion.pressureEvidenceAvailable),
                        Modifier.weight(1f),
                    )
                    RecognitionMetric(
                        "视觉",
                        fusionEvidenceStrength(evidence.visionPresenceX1000, fusion.visionEvidenceAvailable),
                        Modifier.weight(1f),
                    )
                    RecognitionMetric(
                        "热成像",
                        fusionEvidenceStrength(evidence.thermalPresenceX1000, fusion.thermalEvidenceAvailable),
                        Modifier.weight(1f),
                    )
                }
                Text(
                    "证据强度用于说明设备当前判断依据，不代表医学概率。",
                    style = MaterialTheme.typography.labelSmall,
                    color = TextTertiary,
                )
            }
            Text(
                if (overlay.expired) "视觉结果已超过有效期，界面不会继续显示旧检测框。" else overlayStatus(overlay),
                style = MaterialTheme.typography.labelSmall,
                color = if (overlay.expired) Level1Yellow else TextSecondary,
            )
        }
    }
}

private fun fusionEvidenceStrength(valueX1000: Int, available: Boolean = true): String =
    if (available) "%.0f%%".format(valueX1000.coerceIn(0, 1000) / 10f) else "--"

private fun monitorPresenceLabel(label: String?): String = when (label?.trim()?.lowercase()) {
    "present" -> "宝宝在床"
    "absent" -> "可能离床"
    "uncertain" -> "在床状态待确认"
    else -> "在床状态融合中"
}

private fun monitorFusionAlarmTitle(primaryLabel: String, level: Int): String {
    if (level <= 0) return "监护状态正常"
    return when (primaryLabel.trim().lowercase()) {
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

private fun monitorFeverScreeningEvidenceValid(snapshot: MonitorSnapshot): Boolean {
    val fusion = snapshot.fusion
    val evidence = fusion.evidence
    val presence = fusion.contexts["presence"]?.label?.trim()?.lowercase()
    val bodyTemperature = fusion.contexts["body_temperature"]?.label?.trim()?.lowercase()
    return fusion.schemaVersion >= 8 &&
        evidence.available &&
        fusion.thermalEvidenceAvailable &&
        presence == "present" &&
        evidence.thermalPresenceX1000 >= 550 &&
        evidence.thermalHotspot.points >= 4 &&
        evidence.thermalHotspot.meanC?.isFinite() == true &&
        bodyTemperature in setOf("normal", "high")
}

@Composable
private fun ComfortDevicesCard(
    warmLight: RelayActuatorSummary,
    soother: RelayActuatorSummary,
    latestCommand: MobileCommandStatus?,
    deviceOnline: Boolean,
) {
    var lightPending by remember { mutableStateOf<ActuatorPendingCommand?>(null) }
    var sootherPending by remember { mutableStateOf<ActuatorPendingCommand?>(null) }

    val lightCommand = latestCommand.matching(lightPending)
    val sootherCommand = latestCommand.matching(sootherPending)

    LaunchedEffect(warmLight.on, lightPending?.targetOn) {
        if (lightPending?.targetOn == warmLight.on) lightPending = null
    }
    LaunchedEffect(soother.on, sootherPending?.targetOn) {
        if (sootherPending?.targetOn == soother.on) sootherPending = null
    }

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
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    "舒缓设备",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    "开关状态由监护设备实时回传，网络延迟时不会误显示为已执行",
                    style = MaterialTheme.typography.bodySmall,
                    color = TextSecondary,
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    ActuatorControlTile(
                        modifier = Modifier.weight(1f),
                        title = "暖光陪护",
                        icon = Icons.Default.Lightbulb,
                        actuator = warmLight,
                        fallbackPin = 711,
                        deviceOnline = deviceOnline,
                        pending = lightPending,
                        command = lightCommand,
                        accent = Color(0xFFF2A65A),
                        onToggle = { target ->
                            lightPending = ActuatorPendingCommand(
                                action = "light.set",
                                targetOn = target,
                                previousCommandId = latestCommand?.id,
                            )
                            MobileServerRepository.sendCommand("light.set", mapOf("on" to target))
                        },
                    )
                    ActuatorControlTile(
                        modifier = Modifier.weight(1f),
                        title = "轻柔拍抚",
                        icon = Icons.Default.Vibration,
                        actuator = soother,
                        fallbackPin = 914,
                        deviceOnline = deviceOnline,
                        pending = sootherPending,
                        command = sootherCommand,
                        accent = AiPurple,
                        onToggle = { target ->
                            sootherPending = ActuatorPendingCommand(
                                action = "soother.set",
                                targetOn = target,
                                previousCommandId = latestCommand?.id,
                            )
                            MobileServerRepository.sendCommand("soother.set", mapOf("on" to target))
                        },
                    )
                }
            }

        }
    }
}

@Composable
private fun LullabyPlayerCard(
    player: AudioPlayerSummary,
    latestCommand: MobileCommandStatus?,
    deviceOnline: Boolean,
    storyTransferActive: Boolean,
) {
    var volume by remember(player.volume) {
        mutableFloatStateOf(player.volume.coerceIn(0, 100).toFloat())
    }
    val controlsEnabled = deviceOnline && !storyTransferActive

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
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(46.dp)
                        .background(Color(0xFFFFEBDD), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.MusicNote, null, tint = Secondary)
                }
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("摇篮曲", style = MaterialTheme.typography.titleMedium)
                    Text(
                        when {
                            !deviceOnline -> "设备离线，暂时无法控制播放"
                            storyTransferActive -> "AI 故事正在下发，摇篮曲稍后可用"
                            else -> "设备音频状态 · ${playerSubtitle(player)}"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = TextSecondary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }

            Text(
                "播放设备内置曲库。摇篮曲与 AI 故事入口分开，但共用扬声器和设备音量。",
                style = MaterialTheme.typography.labelSmall,
                color = TextTertiary,
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(
                    onClick = { MobileServerRepository.sendCommand("audio.previous") },
                    enabled = controlsEnabled,
                ) {
                    Icon(Icons.Default.SkipPrevious, "上一首摇篮曲")
                }
                Spacer(Modifier.width(18.dp))
                IconButton(
                    onClick = {
                        when {
                            player.isPlaying -> MobileServerRepository.sendCommand("audio.pause")
                            player.isPaused -> MobileServerRepository.sendCommand("audio.resume")
                            else -> MobileServerRepository.sendCommand(
                                "audio.play_track",
                                mapOf("track" to player.track.coerceAtLeast(1)),
                            )
                        }
                    },
                    enabled = controlsEnabled,
                    modifier = Modifier
                        .size(62.dp)
                        .background(if (controlsEnabled) Primary else TextTertiary, CircleShape),
                ) {
                    Icon(
                        if (player.isPlaying) Icons.Default.Pause else Icons.Default.PlayArrow,
                        if (player.isPlaying) "暂停摇篮曲" else "播放摇篮曲",
                        tint = Color.White,
                        modifier = Modifier.size(31.dp),
                    )
                }
                Spacer(Modifier.width(18.dp))
                IconButton(
                    onClick = { MobileServerRepository.sendCommand("audio.next") },
                    enabled = controlsEnabled,
                ) {
                    Icon(Icons.Default.SkipNext, "下一首摇篮曲")
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.AutoMirrored.Filled.VolumeUp, null, tint = TextSecondary, modifier = Modifier.size(19.dp))
                Slider(
                    value = volume,
                    onValueChange = { volume = it },
                    onValueChangeFinished = {
                        MobileServerRepository.sendCommand("audio.set_volume", mapOf("percent" to volume.toInt()))
                    },
                    valueRange = 0f..100f,
                    enabled = controlsEnabled,
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 8.dp),
                )
                Text("${volume.toInt()}%", style = MaterialTheme.typography.labelMedium)
            }

            latestCommand?.takeIf { it.action.startsWith("audio.") }?.let { status ->
                Text(
                    "摇篮曲操作 · ${commandStatusLabel(status.status)}",
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(PrimaryContainer, RoundedCornerShape(12.dp))
                        .padding(horizontal = 12.dp, vertical = 9.dp),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                )
            }
        }
    }
}

@Composable
private fun AiComfortStoryCard(
    latestStory: AiStory?,
    aiEnabled: Boolean,
    deviceOnline: Boolean,
) {
    var storyExpanded by remember { mutableStateOf(false) }
    var storyTheme by remember { mutableStateOf("月亮和小云朵") }
    var storyDurationSeconds by remember { mutableStateOf(60) }
    val durationOptions = listOf(
        30 to "30 秒",
        60 to "1 分钟",
        120 to "2 分钟",
        240 to "4 分钟",
    )
    val storyBusy = latestStory?.status in setOf(
        "queued",
        "generating",
        "synthesizing",
        "queued_for_device",
    )

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = AiContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(46.dp)
                        .background(AiPurple.copy(alpha = 0.12f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.AutoStories, null, tint = AiPurple)
                }
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("AI 安抚故事", style = MaterialTheme.typography.titleMedium)
                    Text(
                        latestStory?.let {
                            "${it.title.ifBlank { "最近故事" }} · ${storyStatusLabel(it.status)}"
                        } ?: if (aiEnabled) {
                            "按主题生成专属故事音频"
                        } else {
                            "服务端暂未启用 AI 故事"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = if (latestStory?.status == "failed") Level3Red else TextSecondary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }

            Text(
                "故事由服务端单独生成并安全下发，不会加入摇篮曲曲库；传输期间实时音视频可能短暂暂停。",
                style = MaterialTheme.typography.labelSmall,
                color = TextTertiary,
            )

            if (aiEnabled) {
                OutlinedButton(
                    onClick = { storyExpanded = !storyExpanded },
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(14.dp),
                ) {
                    Icon(Icons.Default.AutoStories, null, modifier = Modifier.size(19.dp))
                    Spacer(Modifier.width(7.dp))
                    Text(if (storyExpanded) "收起故事创作" else "创作新的安抚故事")
                }

                AnimatedVisibility(visible = storyExpanded) {
                    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        OutlinedTextField(
                            value = storyTheme,
                            onValueChange = { storyTheme = it.take(40) },
                            modifier = Modifier.fillMaxWidth(),
                            label = { Text("故事主题") },
                            supportingText = { Text("生成完成后将自动下发并播放") },
                            singleLine = true,
                            enabled = deviceOnline,
                        )
                        Text(
                            "故事时长",
                            style = MaterialTheme.typography.labelMedium,
                            color = TextSecondary,
                        )
                        SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                            durationOptions.forEachIndexed { index, (seconds, label) ->
                                SegmentedButton(
                                    selected = storyDurationSeconds == seconds,
                                    onClick = { storyDurationSeconds = seconds },
                                    shape = SegmentedButtonDefaults.itemShape(
                                        index = index,
                                        count = durationOptions.size,
                                    ),
                                    enabled = deviceOnline && !storyBusy,
                                ) {
                                    Text(label, maxLines = 1)
                                }
                            }
                        }
                        Button(
                            onClick = {
                                MobileServerRepository.createStory(
                                    theme = storyTheme,
                                    durationSeconds = storyDurationSeconds,
                                )
                            },
                            enabled = deviceOnline && storyTheme.isNotBlank() && !storyBusy,
                            modifier = Modifier.fillMaxWidth(),
                            shape = RoundedCornerShape(12.dp),
                        ) {
                            Text(if (storyBusy) "故事生成或传输中" else "生成并播放 AI 故事")
                        }
                    }
                }

                if (latestStory?.canReplay == true) {
                    FilledTonalButton(
                        onClick = { MobileServerRepository.replayStory(latestStory.storyId) },
                        enabled = deviceOnline,
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(12.dp),
                    ) {
                        Icon(Icons.Default.Replay, null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.width(7.dp))
                        Text("再次播放最近的 AI 故事")
                    }
                }

                latestStory?.let { story ->
                    if (story.storyText.isNotBlank()) {
                        Text(
                            story.storyText,
                            style = MaterialTheme.typography.bodySmall,
                            color = TextSecondary,
                            maxLines = 3,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    if (story.error.isNotBlank()) {
                        Text(
                            story.error.take(160),
                            style = MaterialTheme.typography.labelSmall,
                            color = Level3Red,
                        )
                    }
                }
            }
        }
    }
}

private data class ActuatorPendingCommand(
    val action: String,
    val targetOn: Boolean,
    val previousCommandId: Long?,
)

private fun MobileCommandStatus?.matching(
    pending: ActuatorPendingCommand?,
): MobileCommandStatus? = takeIf { status ->
    pending != null && status != null &&
        status.action == pending.action && status.id != pending.previousCommandId
}

@Composable
private fun ActuatorControlTile(
    title: String,
    icon: ImageVector,
    actuator: RelayActuatorSummary,
    fallbackPin: Int,
    deviceOnline: Boolean,
    pending: ActuatorPendingCommand?,
    command: MobileCommandStatus?,
    accent: Color,
    onToggle: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    val telemetryConfirmed = pending?.targetOn == actuator.on
    val failed = pending != null && command?.status == "failed"
    val waiting = pending != null && !telemetryConfirmed && !failed
    val canControl = deviceOnline && actuator.ready && !waiting
    val pin = actuator.pin?.takeIf { it > 0 } ?: fallbackPin
    val statusText = when {
        !deviceOnline -> "设备离线 · P$pin"
        !actuator.ready -> "执行器未就绪 · P$pin"
        failed -> "执行失败，请重试 · P$pin"
        waiting && command?.status == "acked" -> "设备已接收，等待回传 · P$pin"
        waiting && command?.status == "sent" -> "已发送，等待设备确认 · P$pin"
        waiting -> "正在安全发送 · P$pin"
        actuator.on -> "运行中 · P$pin"
        else -> "已关闭 · P$pin"
    }
    val statusColor = when {
        failed -> Level3Red
        waiting -> Level1Yellow
        actuator.on && actuator.ready && deviceOnline -> SafeGreen
        else -> TextSecondary
    }

    Column(
        modifier = modifier
            .clip(RoundedCornerShape(18.dp))
            .background(accent.copy(alpha = 0.09f))
            .border(1.dp, accent.copy(alpha = 0.18f), RoundedCornerShape(18.dp))
            .padding(horizontal = 13.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                modifier = Modifier
                    .size(36.dp)
                    .background(accent.copy(alpha = 0.16f), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(icon, contentDescription = null, tint = accent, modifier = Modifier.size(20.dp))
            }
            Spacer(Modifier.weight(1f))
            Switch(
                checked = actuator.on,
                onCheckedChange = onToggle,
                enabled = canControl,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = accent,
                ),
            )
        }
        Text(
            title,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
        )
        Text(
            statusText,
            style = MaterialTheme.typography.labelSmall,
            color = statusColor,
            maxLines = 2,
        )
    }
}

@Composable
private fun MediaPlaceholder(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    subtitle: String,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            modifier = Modifier
                .size(62.dp)
                .background(Color.White.copy(alpha = 0.09f), CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, null, tint = Color.White.copy(alpha = 0.72f), modifier = Modifier.size(31.dp))
        }
        Spacer(Modifier.height(12.dp))
        Text(title, style = MaterialTheme.typography.titleMedium, color = TextOnDark)
        Text(subtitle, style = MaterialTheme.typography.bodySmall, color = TextOnDark.copy(alpha = 0.56f))
    }
}

@Composable
private fun StreamBadge(text: String, active: Boolean, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(50))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(7.dp).background(if (active) SafeGreen else Level1Yellow, CircleShape))
        Spacer(Modifier.width(6.dp))
        Text(text, style = MaterialTheme.typography.labelSmall, color = Color.White)
    }
}

@Composable
private fun OverlayBadge(
    overlay: VisionOverlaySummary,
    synchronized: Boolean,
    modifier: Modifier = Modifier,
) {
    val warning = overlay.expired || overlay.stale || !synchronized
    Text(
        if (!synchronized && !overlay.expired) "识别结果待同步" else overlayStatus(overlay),
        modifier = modifier
            .background(Color.Black.copy(alpha = 0.52f), RoundedCornerShape(9.dp))
            .border(
                1.dp,
                if (warning) Color(0xFFB98543) else Color.White.copy(alpha = 0.2f),
                RoundedCornerShape(9.dp),
            )
            .padding(horizontal = 9.dp, vertical = 6.dp),
        style = MaterialTheme.typography.labelSmall,
        color = if (warning) Color(0xFFFFD39A) else Color.White,
    )
}

private fun overlayMatchesFrame(overlay: VisionOverlaySummary, videoSequence: Long): Boolean {
    if (!overlay.enabled || overlay.stale || overlay.expired || videoSequence <= 0L) return false
    if (overlay.sourceAgeMs !in 0L..2_500L) return false
    if (overlay.sourceSequence <= 0L) return true
    val sequenceDelta = (videoSequence - overlay.sourceSequence) and 0xFFFF_FFFFL
    return sequenceDelta <= 8L
}

@Composable
private fun StreamMetric(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .background(Color.White.copy(alpha = 0.07f), RoundedCornerShape(12.dp))
            .padding(horizontal = 10.dp, vertical = 9.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = TextOnDark.copy(alpha = 0.5f))
        Text(value, style = MaterialTheme.typography.labelLarge, color = TextOnDark, maxLines = 1)
    }
}

@Composable
private fun RecognitionMetric(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    valueColor: Color = Color.Unspecified,
) {
    Column(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.72f), RoundedCornerShape(14.dp))
            .padding(11.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = TextSecondary)
        Text(value, style = MaterialTheme.typography.titleSmall, color = valueColor, maxLines = 1)
    }
}

@Composable
private fun PrivacyBadge(active: Boolean) {
    Row(
        modifier = Modifier
            .background(
                if (active) Level3Red.copy(alpha = 0.11f) else PrimaryContainer,
                RoundedCornerShape(50),
            )
            .padding(horizontal = 10.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(Icons.Default.Security, null, tint = if (active) Level3Red else Primary, modifier = Modifier.size(16.dp))
        Spacer(Modifier.width(5.dp))
        Text(if (active) "媒体开启" else "隐私保护", style = MaterialTheme.typography.labelSmall, color = if (active) Level3Red else Primary)
    }
}

@Composable
private fun MediaPrivacyBadge() {
    val mediaWanted = remember {
        combine(
            MobileServerRepository.videoState,
            MobileServerRepository.audioState,
        ) { video, audio ->
            video.wanted || audio.wanted
        }.distinctUntilChanged()
    }
    val active by mediaWanted.collectAsStateWithLifecycle(initialValue = false)
    PrivacyBadge(active = active)
}

@Composable
private fun mediaSwitchColors() = SwitchDefaults.colors(
    checkedThumbColor = Color.White,
    checkedTrackColor = SafeGreen,
    uncheckedThumbColor = Color.White,
    uncheckedTrackColor = Color.White.copy(alpha = 0.18f),
    uncheckedBorderColor = Color.White.copy(alpha = 0.18f),
)

private fun videoPlaceholderTitle(
    state: VideoStreamState,
    deviceOnline: Boolean,
    available: Boolean,
    pausedForStory: Boolean,
): String = when {
    !deviceOnline -> "监护设备暂时离线"
    pausedForStory -> "AI 故事正在传输"
    !available -> "摄像头正在准备"
    !state.wanted -> "实时画面尚未开启"
    state.connected -> "已连接，等待最新画面"
    else -> "正在连接安全视频通道"
}

private fun videoPlaceholderSubtitle(state: VideoStreamState, pausedForStory: Boolean): String = when {
    pausedForStory -> "画面暂时暂停，完成后自动恢复"
    !state.errorMessage.isNullOrBlank() -> state.errorMessage.take(100)
    state.reconnectAttempt > 0 -> "弱网自动重连第 ${state.reconnectAttempt} 次"
    !state.wanted -> "使用下方开关手动开启"
    else -> "公网 WSS · 最新帧优先"
}

private fun audioStatus(
    state: AudioUiState,
    deviceOnline: Boolean,
    available: Boolean,
    pausedForStory: Boolean,
): String = when {
    !deviceOnline -> "设备离线，声音监听不可用"
    pausedForStory -> "AI 故事传输中，声音完成后自动恢复"
    !available -> "设备暂未上传声音"
    !state.wanted -> "手动开启后开始监听"
    state.muted -> "监听已静音"
    state.stale && state.connected -> "音频数据延迟，正在自动恢复"
    state.receiving -> "正在播放宝宝房间声音"
    state.connected -> "通道已连接，等待音频"
    !state.errorMessage.isNullOrBlank() -> state.errorMessage
    state.reconnectAttempt > 0 -> "弱网自动重连第 ${state.reconnectAttempt} 次"
    else -> "正在建立安全音频通道"
}

private fun formatTransferBytes(bytes: Long): String = when {
    bytes >= 1024L * 1024L -> "%.1f MB".format(bytes / (1024f * 1024f))
    bytes >= 1024L -> "%.0f KB".format(bytes / 1024f)
    else -> "$bytes B"
}

private fun AudioStreamState.toUiState(): AudioUiState = AudioUiState(
    wanted = wanted,
    connected = connected,
    receiving = receiving,
    muted = muted,
    stale = stale,
    reconnectAttempt = reconnectAttempt,
    errorMessage = errorMessage,
)

private fun overlayStatus(overlay: VisionOverlaySummary): String = when {
    !overlay.enabled -> "AI 检测框未开启"
    overlay.expired -> "检测结果已过期"
    overlay.stale -> "上次识别 · ${"%.1f".format(overlay.sourceAgeMs / 1000f)} 秒前"
    overlay.boxes.isEmpty() -> "本次未检出目标"
    else -> "${overlay.boxes.size} 个目标 · ${if (overlay.calibrationStatus == "calibrated") "已校准" else "待校准"}"
}

private fun detectionColor(classId: Int): Color = when (classId) {
    0 -> Color(0xFF61D8AF)
    1 -> Color(0xFF69B4FF)
    2 -> Color(0xFFFFCE68)
    3 -> Color(0xFFDFA0FF)
    4 -> Color(0xFFFF8686)
    else -> Color(0xFFF2F5F4)
}

private fun playerSubtitle(player: AudioPlayerSummary): String {
    val state = when (player.status) {
        1 -> "等待存储卡"
        2 -> "音频文件不可用"
        3 -> "音频格式异常"
        4 -> "播放接口异常"
        5 -> "正在播放"
        6 -> "播放完成"
        7 -> "已暂停"
        8 -> "已停止"
        else -> "待机"
    }
    val track = player.file.ifBlank { if (player.track > 0) "曲目 ${player.track}" else "未选择曲目" }
    return "$state · $track"
}

private fun commandStatusLabel(status: String): String = when (status) {
    "queued" -> "等待发送"
    "sent" -> "已安全转发"
    "acked" -> "设备已确认"
    "failed" -> "执行失败"
    else -> status
}

private fun storyStatusLabel(status: String): String = when (status) {
    "queued" -> "等待生成"
    "generating" -> "正在生成"
    "synthesizing" -> "正在合成语音"
    "ready" -> "已生成"
    "queued_for_device" -> "正在传输到设备"
    "queued_for_playback" -> "已加入播放队列"
    "failed" -> "生成失败"
    else -> status
}

private fun severityLabel(severity: Int): String = when (severity) {
    0 -> "正常"
    1 -> "留意"
    2 -> "提醒"
    3 -> "警告"
    4 -> "高危"
    else -> "紧急"
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

private fun severityColor(severity: Int): Color = when (severity.coerceIn(0, 5)) {
    0 -> SafeGreen
    1, 2 -> Level1Yellow
    3, 4 -> Level2Purple
    else -> Level3Red
}
