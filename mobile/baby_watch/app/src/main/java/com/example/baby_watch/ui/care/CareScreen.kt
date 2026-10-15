package com.example.baby_watch.ui.care

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Psychology
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.baby_watch.data.server.AudioPlayerSummary
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.RelayActuatorSummary
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.ui.theme.Background
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.PrimaryContainer
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.SecondaryContainer
import com.example.baby_watch.ui.theme.TextPrimary
import com.example.baby_watch.ui.theme.TextSecondary

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CareScreen(modifier: Modifier = Modifier) {
    val repositoryState by MobileServerRepository.state.collectAsState()
    val command by MobileServerRepository.lastCommand.collectAsState()
    val player = repositoryState.snapshot.audioPlayer
    val controlsEnabled = repositoryState.connectionStatus == ServerConnectionStatus.Connected &&
        repositoryState.snapshot.device.online

    Scaffold(
        containerColor = Background,
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            "安抚中心",
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                            color = TextPrimary,
                        )
                        Text(
                            "通过服务端控制 RA8 音频播放器",
                            fontSize = 13.sp,
                            color = TextSecondary,
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Background),
            )
        },
    ) { innerPadding ->
        Column(
            modifier = modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            InsightCard(
                stateLabel = repositoryState.snapshot.inference.stateLabel,
                severity = repositoryState.snapshot.inference.severity,
            )
            MusicCard(player)
            RelayControlsCard(
                warmLight = repositoryState.snapshot.warmLight,
                soother = repositoryState.snapshot.soother,
                controlsEnabled = controlsEnabled,
            )
            command?.let {
                CommandStatusCard(
                    action = it.action,
                    status = it.status,
                )
            }
            SafetyCard()
        }
    }
}

@Composable
private fun RelayControlsCard(
    warmLight: RelayActuatorSummary,
    soother: RelayActuatorSummary,
    controlsEnabled: Boolean,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier.padding(horizontal = 18.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                "远程安抚设备",
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                color = TextPrimary,
                modifier = Modifier.padding(bottom = 6.dp),
            )
            RelaySwitchRow(
                label = "暖光灯",
                detail = relayDetail(warmLight, 711),
                actuator = warmLight,
                enabled = controlsEnabled && warmLight.ready,
                onCheckedChange = {
                    MobileServerRepository.sendCommand("light.set", mapOf("on" to it))
                },
            )
            RelaySwitchRow(
                label = "拍一拍电机",
                detail = relayDetail(soother, 914),
                actuator = soother,
                enabled = controlsEnabled && soother.ready,
                onCheckedChange = {
                    MobileServerRepository.sendCommand("soother.set", mapOf("on" to it))
                },
            )
            Text(
                "开关状态以 RA8 遥测确认；AI 建议不会自动操作设备。",
                fontSize = 12.sp,
                color = TextSecondary,
                modifier = Modifier.padding(top = 8.dp, bottom = 2.dp),
            )
        }
    }
}

@Composable
private fun RelaySwitchRow(
    label: String,
    detail: String,
    actuator: RelayActuatorSummary,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(label, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = TextPrimary)
            Text(detail, fontSize = 12.sp, color = TextSecondary)
        }
        Switch(
            checked = actuator.on,
            onCheckedChange = onCheckedChange,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = Primary,
                uncheckedThumbColor = Color.White,
                uncheckedTrackColor = Color(0xFFE7E4DF),
                uncheckedBorderColor = Color.Transparent,
            ),
        )
    }
}

private fun relayDetail(actuator: RelayActuatorSummary, defaultPin: Int): String {
    val state = when {
        actuator.ready && actuator.on -> "已开启"
        actuator.ready -> "已关闭"
        actuator.status == 2 -> "GPIO 初始化失败"
        else -> "等待 RA8 状态"
    }
    return "$state · P${actuator.pin ?: defaultPin}"
}

@Composable
private fun InsightCard(stateLabel: String, severity: Int) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = PrimaryContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Row(
            modifier = Modifier.padding(18.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(48.dp)
                    .clip(CircleShape)
                    .background(Color.White),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Default.Psychology,
                    contentDescription = null,
                    tint = Primary,
                    modifier = Modifier.size(25.dp),
                )
            }
            Spacer(modifier = Modifier.width(14.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("服务端状态评估", fontSize = 12.sp, color = TextSecondary)
                Text(
                    stateLabel,
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold,
                    color = TextPrimary,
                )
                Text(
                    if (severity >= 3) "当前风险较高，请先人工确认现场情况。"
                    else "可按需远程播放舒缓音乐。",
                    fontSize = 13.sp,
                    color = TextSecondary,
                )
            }
        }
    }
}

@Composable
private fun MusicCard(player: AudioPlayerSummary) {
    var volumeDraft by remember(player.volume) {
        mutableFloatStateOf(player.volume.coerceIn(0, 100).toFloat())
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    Icons.Default.MusicNote,
                    contentDescription = null,
                    tint = Primary,
                    modifier = Modifier.size(22.dp),
                )
                Spacer(modifier = Modifier.width(8.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "摇篮曲",
                        fontSize = 17.sp,
                        fontWeight = FontWeight.Bold,
                        color = TextPrimary,
                    )
                    Text(
                        playerSubtitle(player),
                        fontSize = 13.sp,
                        color = TextSecondary,
                    )
                }
            }
            Spacer(modifier = Modifier.height(18.dp))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                IconButton(
                    onClick = { MobileServerRepository.sendCommand("audio.previous") },
                    modifier = Modifier.size(48.dp),
                ) {
                    Icon(
                        Icons.Default.SkipPrevious,
                        contentDescription = "上一首",
                        modifier = Modifier.size(32.dp),
                        tint = TextSecondary,
                    )
                }
                Spacer(modifier = Modifier.width(18.dp))
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
                    modifier = Modifier
                        .size(68.dp)
                        .clip(CircleShape)
                        .background(if (player.isPlaying) Primary else SafeGreen),
                ) {
                    Icon(
                        if (player.isPlaying) Icons.Default.Pause else Icons.Default.PlayArrow,
                        contentDescription = if (player.isPlaying) "暂停" else "播放",
                        tint = Color.White,
                        modifier = Modifier.size(34.dp),
                    )
                }
                Spacer(modifier = Modifier.width(18.dp))
                IconButton(
                    onClick = { MobileServerRepository.sendCommand("audio.next") },
                    modifier = Modifier.size(48.dp),
                ) {
                    Icon(
                        Icons.Default.SkipNext,
                        contentDescription = "下一首",
                        modifier = Modifier.size(32.dp),
                        tint = TextSecondary,
                    )
                }
            }
            Spacer(modifier = Modifier.height(12.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("音量", fontSize = 13.sp, color = TextSecondary)
                Slider(
                    value = volumeDraft,
                    onValueChange = { volumeDraft = it },
                    onValueChangeFinished = {
                        MobileServerRepository.sendCommand(
                            "audio.set_volume",
                            mapOf("percent" to volumeDraft.toInt()),
                        )
                    },
                    valueRange = 0f..100f,
                    modifier = Modifier.weight(1f).padding(horizontal = 10.dp),
                )
                Text(
                    "${volumeDraft.toInt()}%",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextPrimary,
                )
            }
        }
    }
}

@Composable
private fun CommandStatusCard(action: String, status: String) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(containerColor = SecondaryContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Text(
            "最近命令：$action · ${commandStatusLabel(status)}",
            modifier = Modifier.padding(14.dp),
            fontSize = 13.sp,
            color = TextPrimary,
        )
    }
}

@Composable
private fun SafetyCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = SecondaryContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Text(
            "所有控制命令均由服务端排队并转发到 RA8；“已确认”代表网关已转发，实际播放状态以最新遥测为准。",
            modifier = Modifier.padding(16.dp),
            fontSize = 13.sp,
            lineHeight = 19.sp,
            color = Level3Red,
        )
    }
}

private fun playerSubtitle(player: AudioPlayerSummary): String {
    val state = when (player.status) {
        1 -> "等待 SD 卡"
        2 -> "文件打开失败"
        3 -> "WAV 文件异常"
        4 -> "音频接口异常"
        5 -> "正在播放"
        6 -> "播放完成"
        7 -> "已暂停"
        8 -> "已停止"
        else -> "空闲"
    }
    val track = player.file.ifBlank {
        if (player.track > 0) "曲目 ${player.track}" else "未选择曲目"
    }
    return "$state · $track · 共 ${player.trackCount} 首"
}

private fun commandStatusLabel(status: String): String = when (status) {
    "queued" -> "等待发送"
    "sent" -> "已转发"
    "acked" -> "网关已确认"
    "failed" -> "执行失败"
    else -> status
}
