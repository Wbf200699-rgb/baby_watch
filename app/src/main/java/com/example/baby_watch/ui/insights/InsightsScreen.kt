package com.example.baby_watch.ui.insights

import androidx.compose.foundation.Canvas
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
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Bedtime
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.automirrored.filled.MenuBook
import androidx.compose.material.icons.filled.MonitorHeart
import androidx.compose.material.icons.filled.Pets
import androidx.compose.material.icons.filled.PriorityHigh
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.baby_watch.data.server.AiAdvisory
import com.example.baby_watch.data.server.AiStory
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.SafetyEvent
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.data.server.TrendSample
import com.example.baby_watch.ui.components.ConnectionBanner
import com.example.baby_watch.ui.components.ScreenHeader
import com.example.baby_watch.ui.components.SectionHeader
import com.example.baby_watch.ui.theme.AiPurple
import com.example.baby_watch.ui.theme.Level1Yellow
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.Secondary
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun InsightsScreen(modifier: Modifier = Modifier) {
    val repositoryState by MobileServerRepository.state.collectAsStateWithLifecycle()
    val history by MobileServerRepository.history.collectAsStateWithLifecycle()
    val events by MobileServerRepository.events.collectAsStateWithLifecycle()
    val advisories by MobileServerRepository.advisories.collectAsStateWithLifecycle()
    val stories by MobileServerRepository.stories.collectAsStateWithLifecycle()

    val todaySamples = remember(history) {
        val dayStart = Calendar.getInstance().run {
            set(Calendar.HOUR_OF_DAY, 0)
            set(Calendar.MINUTE, 0)
            set(Calendar.SECOND, 0)
            set(Calendar.MILLISECOND, 0)
            timeInMillis
        }
        history.filter { it.timestampMs >= dayStart }
    }
    val insight = remember(todaySamples) { DailyInsight.from(todaySamples) }
    val todayKey = remember {
        SimpleDateFormat("yyyy-MM-dd", Locale.getDefault()).format(Date())
    }
    val todayAdvisories = remember(advisories, todayKey) {
        advisories.filter { it.createdAt.startsWith(todayKey) }
    }
    val latestAdvisory = todayAdvisories.firstOrNull { !it.acknowledged }
        ?: todayAdvisories.firstOrNull()
    val latestStory = stories.firstOrNull()
    val connected = repositoryState.connectionStatus == ServerConnectionStatus.Connected &&
        repositoryState.snapshot.device.online

    LazyColumn(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 18.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item {
            ScreenHeader(
                eyebrow = "AI MONITORING",
                title = "监护洞察",
                subtitle = "把实时数据整理成易读、可追溯的照护线索",
            )
        }

        item { ConnectionBanner(repositoryState) }

        item {
            DailyAiSummaryCard(
                insight = insight,
                advisory = latestAdvisory,
                aiEnabled = repositoryState.capabilities.aiEnabled,
                usingCachedData = repositoryState.usingCachedData,
                onAcknowledge = { advisoryId ->
                    MobileServerRepository.acknowledgeAdvisory(advisoryId)
                },
            )
        }

        item {
            SectionHeader(
                title = "今日状态切片",
                subtitle = "基于今天已收到的采样，不等同于连续医学监测",
            )
        }

        item {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    InsightMetricCard(
                        modifier = Modifier.weight(1f),
                        icon = Icons.Default.GraphicEq,
                        tint = Secondary,
                        title = "哭声线索",
                        value = insight.cryValue,
                        helper = insight.cryHelper,
                    )
                    InsightMetricCard(
                        modifier = Modifier.weight(1f),
                        icon = Icons.Default.Bedtime,
                        tint = AiPurple,
                        title = "睡眠状态",
                        value = insight.sleepValue,
                        helper = insight.sleepHelper,
                    )
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    InsightMetricCard(
                        modifier = Modifier.weight(1f),
                        icon = Icons.Default.Pets,
                        tint = Primary,
                        title = "在床行为",
                        value = insight.occupancyValue,
                        helper = insight.occupancyHelper,
                    )
                    InsightMetricCard(
                        modifier = Modifier.weight(1f),
                        icon = Icons.Default.MonitorHeart,
                        tint = if (insight.riskSamples > 0) Level1Yellow else SafeGreen,
                        title = "风险采样",
                        value = if (insight.hasData) "${insight.riskSamples}" else "--",
                        helper = if (insight.hasData) {
                            "${insight.sampleCount} 个采样点中等级高于正常"
                        } else {
                            "等待有效采样"
                        },
                    )
                }
            }
        }

        item {
            SectionHeader(
                title = "状态趋势",
                subtitle = "最近 ${minOf(todaySamples.size, TREND_POINT_LIMIT)} 个今日采样点",
            )
        }

        item { TrendCard(todaySamples.takeLast(TREND_POINT_LIMIT)) }

        item {
            SectionHeader(
                title = "异常事件时间线",
                subtitle = "服务端最近记录，已确认事件仍保留用于回顾",
            )
        }

        item { EventTimeline(events.sortedByDescending(SafetyEvent::id).take(6)) }

        item {
            SectionHeader(
                title = "睡前故事",
                subtitle = "由 AI 生成照护内容，播放前建议家长先试听",
            )
        }

        item {
            StoryGeneratorCard(
                enabled = repositoryState.capabilities.aiEnabled && connected,
                aiEnabled = repositoryState.capabilities.aiEnabled,
                latestStory = latestStory,
                onGenerate = { theme ->
                    MobileServerRepository.createStory(
                        theme = theme,
                        durationSeconds = 60,
                        style = "gentle",
                        autoplay = true,
                    )
                },
            )
        }

        item { SafetyDisclaimer() }
    }
}

@Composable
private fun DailyAiSummaryCard(
    insight: DailyInsight,
    advisory: AiAdvisory?,
    aiEnabled: Boolean,
    usingCachedData: Boolean,
    onAcknowledge: (String) -> Unit,
) {
    val needsAttention = advisory?.requiresUserAttention == true || insight.riskSamples > 0
    val accent = when (advisory?.level) {
        "urgent" -> Level3Red
        "attention" -> Level1Yellow
        else -> if (needsAttention) Level1Yellow else SafeGreen
    }
    val summaryTitle = when {
        advisory != null && advisory.title.isNotBlank() -> advisory.title
        !aiEnabled -> "AI 洞察暂未启用"
        !insight.hasEnoughData -> "今天的数据还不足以形成总结"
        insight.riskSamples > 0 -> "有几段状态值得家长回看"
        else -> "已采样时段内未见风险等级升高"
    }
    val summaryBody = when {
        advisory != null && advisory.summary.isNotBlank() -> advisory.summary
        !aiEnabled -> "服务端未开放 AI 能力，页面仅展示设备采样与告警记录。"
        !insight.hasEnoughData -> "至少积累 3 个今日采样点后，才能给出有依据的状态概览。"
        insight.riskSamples > 0 ->
            "今天 ${insight.sampleCount} 个采样点中有 ${insight.riskSamples} 个风险等级高于正常，请结合事件时间线和实时画面核实。"
        else -> "基于今天 ${insight.sampleCount} 个采样点生成；这只是辅助整理，不代表医疗结论。"
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(containerColor = Color.Transparent),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Column(
            modifier = Modifier
                .background(
                    Brush.linearGradient(
                        listOf(
                            MaterialTheme.colorScheme.primaryContainer,
                            MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.78f),
                        )
                    )
                )
                .padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .background(AiPurple.copy(alpha = 0.13f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.AutoAwesome, null, tint = AiPurple)
                }
                Spacer(Modifier.width(11.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "AI 今日总结",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                    )
                    Text(
                        when {
                            usingCachedData -> "当前使用最近缓存 · 不是实时判断"
                            advisory != null -> "服务端辅助建议 · ${advisoryTime(advisory.createdAt)}"
                            else -> "基于今日设备采样"
                        },
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Box(
                    modifier = Modifier
                        .background(accent.copy(alpha = 0.13f), RoundedCornerShape(50))
                        .padding(horizontal = 9.dp, vertical = 5.dp),
                ) {
                    Text(
                        if (needsAttention) "建议查看" else "辅助参考",
                        style = MaterialTheme.typography.labelSmall,
                        color = accent,
                    )
                }
            }
            Text(
                summaryTitle,
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text(
                summaryBody,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            advisory
                ?.takeIf { !it.acknowledged && it.requiresUserAttention }
                ?.let {
                    TextButton(
                        onClick = { onAcknowledge(it.advisoryId) },
                        contentPadding = PaddingValues(0.dp),
                    ) {
                        Icon(Icons.Default.CheckCircle, null, modifier = Modifier.size(17.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("我已查看这条辅助建议")
                    }
                }
        }
    }
}

@Composable
private fun InsightMetricCard(
    icon: ImageVector,
    tint: Color,
    title: String,
    value: String,
    helper: String,
    modifier: Modifier = Modifier,
) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(19.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column(
            modifier = Modifier.padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(32.dp)
                        .background(tint.copy(alpha = 0.12f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(icon, null, tint = tint, modifier = Modifier.size(17.dp))
                }
                Spacer(Modifier.width(8.dp))
                Text(
                    title,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                value,
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
            )
            Text(
                helper,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                minLines = 2,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun TrendCard(samples: List<TrendSample>) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                TrendLegend(color = Secondary, label = "哭声模型置信度")
                TrendLegend(color = Level3Red, label = "风险等级")
            }
            if (samples.size < 2) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(150.dp)
                        .background(
                            MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f),
                            RoundedCornerShape(14.dp),
                        ),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        "至少需要 2 个今日采样点才能绘制趋势",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            } else {
                val gridColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.24f)
                Canvas(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(166.dp),
                ) {
                    val horizontalPadding = 4.dp.toPx()
                    val verticalPadding = 8.dp.toPx()
                    val chartWidth = size.width - horizontalPadding * 2
                    val chartHeight = size.height - verticalPadding * 2
                    repeat(5) { index ->
                        val y = verticalPadding + chartHeight * index / 4f
                        drawLine(
                            color = gridColor,
                            start = Offset(horizontalPadding, y),
                            end = Offset(size.width - horizontalPadding, y),
                            strokeWidth = 1.dp.toPx(),
                        )
                    }

                    fun xAt(index: Int): Float = horizontalPadding +
                        chartWidth * index / (samples.lastIndex.coerceAtLeast(1)).toFloat()

                    fun yAt(value: Float): Float = verticalPadding +
                        chartHeight * (1f - value.coerceIn(0f, 1f))

                    val cryPath = Path()
                    val riskPath = Path()
                    samples.forEachIndexed { index, sample ->
                        val x = xAt(index)
                        val cryY = yAt(sample.cryConfidence / 100f)
                        val riskY = yAt(sample.severity / 5f)
                        if (index == 0) {
                            cryPath.moveTo(x, cryY)
                            riskPath.moveTo(x, riskY)
                        } else {
                            cryPath.lineTo(x, cryY)
                            riskPath.lineTo(x, riskY)
                        }
                    }
                    drawPath(
                        path = cryPath,
                        color = Secondary,
                        style = androidx.compose.ui.graphics.drawscope.Stroke(
                            width = 2.5.dp.toPx(),
                            cap = StrokeCap.Round,
                        ),
                    )
                    drawPath(
                        path = riskPath,
                        color = Level3Red,
                        style = androidx.compose.ui.graphics.drawscope.Stroke(
                            width = 2.dp.toPx(),
                            cap = StrokeCap.Round,
                        ),
                    )
                    val last = samples.last()
                    drawCircle(
                        color = Secondary,
                        radius = 4.dp.toPx(),
                        center = Offset(xAt(samples.lastIndex), yAt(last.cryConfidence / 100f)),
                    )
                    drawCircle(
                        color = Level3Red,
                        radius = 3.5.dp.toPx(),
                        center = Offset(xAt(samples.lastIndex), yAt(last.severity / 5f)),
                    )
                }
                Row(modifier = Modifier.fillMaxWidth()) {
                    Text(
                        formatClock(samples.first().timestampMs),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.weight(1f))
                    Text(
                        formatClock(samples.last().timestampMs),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun TrendLegend(color: Color, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(8.dp)
                .background(color, CircleShape)
        )
        Spacer(Modifier.width(6.dp))
        Text(
            label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun EventTimeline(events: List<SafetyEvent>) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        if (events.isEmpty()) {
            Row(
                modifier = Modifier.padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Default.CheckCircle, null, tint = SafeGreen)
                Spacer(Modifier.width(10.dp))
                Column {
                    Text("暂无异常事件", style = MaterialTheme.typography.titleSmall)
                    Text(
                        "服务端产生的新事件会显示在这里",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        } else {
            Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
                events.forEachIndexed { index, event ->
                    TimelineRow(event = event, showLine = index < events.lastIndex)
                }
            }
        }
    }
}

@Composable
private fun TimelineRow(event: SafetyEvent, showLine: Boolean) {
    val accent = when {
        event.isRecoveryOrResolved -> SafeGreen
        event.severity == 0 -> SafeGreen
        event.severity in 1..2 -> Level1Yellow
        else -> Level3Red
    }
    Row(modifier = Modifier.fillMaxWidth()) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Box(
                modifier = Modifier
                    .padding(top = 18.dp)
                    .size(11.dp)
                    .background(accent, CircleShape)
            )
            if (showLine) {
                Box(
                    modifier = Modifier
                        .width(1.dp)
                        .height(52.dp)
                        .background(MaterialTheme.colorScheme.outline.copy(alpha = 0.35f))
                )
            }
        }
        Spacer(Modifier.width(12.dp))
        Column(
            modifier = Modifier
                .weight(1f)
                .padding(vertical = 13.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    event.title,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    compactServerTime(event.receivedAt),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                if (event.isRecoveryOrResolved) {
                    event.lifecycleStatusLabel
                } else {
                    "风险等级 ${event.severity} · ${event.lifecycleStatusLabel}"
                },
                style = MaterialTheme.typography.bodySmall,
                color = accent,
            )
        }
    }
}

@Composable
private fun StoryGeneratorCard(
    enabled: Boolean,
    aiEnabled: Boolean,
    latestStory: AiStory?,
    onGenerate: (String) -> Unit,
) {
    var theme by rememberSaveable { mutableStateOf("") }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .background(AiPurple.copy(alpha = 0.12f), CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.AutoMirrored.Filled.MenuBook, null, tint = AiPurple)
                }
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("创建一段温柔的故事", style = MaterialTheme.typography.titleSmall)
                    Text(
                        when {
                            !aiEnabled -> "服务端当前未开放 AI 故事能力"
                            !enabled -> "设备离线，恢复连接后可生成并播放"
                            else -> "输入一个主题，将生成约 1 分钟的故事"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            OutlinedTextField(
                value = theme,
                onValueChange = { theme = it.take(STORY_THEME_LIMIT) },
                modifier = Modifier.fillMaxWidth(),
                enabled = enabled,
                singleLine = true,
                label = { Text("故事主题") },
                placeholder = { Text("例如：小鲸鱼第一次看星星") },
                supportingText = { Text("请勿输入个人隐私或医疗问题") },
                shape = RoundedCornerShape(14.dp),
            )
            Button(
                onClick = { onGenerate(theme.trim()) },
                enabled = enabled && theme.isNotBlank(),
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(14.dp),
                colors = ButtonDefaults.buttonColors(containerColor = AiPurple),
            ) {
                Icon(Icons.Default.AutoAwesome, null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(7.dp))
                Text("生成并发送到设备")
            }
            latestStory?.let { story ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
                            RoundedCornerShape(12.dp),
                        )
                        .padding(11.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(
                        if (story.status == "failed") Icons.Default.PriorityHigh
                        else Icons.Default.CheckCircle,
                        null,
                        tint = if (story.status == "failed") Level3Red else SafeGreen,
                        modifier = Modifier.size(18.dp),
                    )
                    Spacer(Modifier.width(8.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            story.title.ifBlank { "最近生成的故事" },
                            style = MaterialTheme.typography.labelMedium,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Text(
                            storyStatusLabel(story.status),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun SafetyDisclaimer() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.42f),
                RoundedCornerShape(16.dp),
            )
            .padding(14.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Icon(
            Icons.Default.Info,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(18.dp),
        )
        Spacer(Modifier.width(9.dp))
        Text(
            "AI 总结、哭声概率、睡眠与行为状态仅用于辅助照护，不构成医疗诊断，也不能替代家长现场确认。若宝宝出现呼吸异常、持续高热或其他紧急情况，请立即进行人工检查并寻求专业帮助。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private data class DailyInsight(
    val sampleCount: Int,
    val cryPeak: Float?,
    val elevatedCrySamples: Int,
    val sleepShare: Int?,
    val occupancyShare: Int?,
    val riskSamples: Int,
) {
    val hasData: Boolean
        get() = sampleCount > 0

    val hasEnoughData: Boolean
        get() = sampleCount >= MIN_SUMMARY_SAMPLES

    val cryValue: String
        get() = cryPeak?.let { "${it.roundToInt()}%" } ?: "--"

    val cryHelper: String
        get() = when {
            !hasData -> "等待今日哭声模型采样"
            elevatedCrySamples > 0 -> "$elevatedCrySamples 个采样点高于 65%"
            else -> "已采样时点未超过 65%"
        }

    val sleepValue: String
        get() = sleepShare?.let { "$it%" } ?: "--"

    val sleepHelper: String
        get() = if (sleepShare == null) {
            "暂无足够睡眠/清醒分类"
        } else {
            "睡眠分类采样占比，非睡眠时长"
        }

    val occupancyValue: String
        get() = occupancyShare?.let { "$it%" } ?: "--"

    val occupancyHelper: String
        get() = if (occupancyShare == null) {
            "暂无足够在床状态采样"
        } else {
            "在床采样占比，不代表持续时长"
        }

    companion object {
        fun from(samples: List<TrendSample>): DailyInsight {
            val cryValues = samples.map { it.cryConfidence.coerceIn(0f, 100f) }
            val sleepClassified = samples.filter { it.safetyState == 10 || it.safetyState == 11 }
            val occupancyClassified = samples.mapNotNull(TrendSample::occupied)
            return DailyInsight(
                sampleCount = samples.size,
                cryPeak = cryValues.maxOrNull(),
                elevatedCrySamples = cryValues.count { it >= 65f },
                sleepShare = sleepClassified.takeIf(List<TrendSample>::isNotEmpty)?.let { known ->
                    (known.count { it.safetyState == 10 } * 100f / known.size).roundToInt()
                },
                occupancyShare = occupancyClassified.takeIf(List<Boolean>::isNotEmpty)?.let { known ->
                    (known.count { it } * 100f / known.size).roundToInt()
                },
                riskSamples = samples.count { it.severity > 0 },
            )
        }
    }
}

private fun storyStatusLabel(status: String): String = when (status) {
    "queued" -> "等待生成"
    "generating" -> "正在生成内容"
    "synthesizing" -> "正在合成语音"
    "ready" -> "已生成"
    "queued_for_device" -> "正在发送到设备"
    "queued_for_playback" -> "已进入播放队列"
    "failed" -> "生成失败，请稍后重试"
    else -> status.ifBlank { "状态待同步" }
}

private fun formatClock(timestampMs: Long): String =
    SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(timestampMs))

private fun compactServerTime(raw: String): String {
    if (raw.isBlank()) return "时间待同步"
    val time = Regex("T(\\d{2}:\\d{2})").find(raw)?.groupValues?.getOrNull(1)
    return time ?: raw.take(16)
}

private fun advisoryTime(raw: String): String = compactServerTime(raw)

private const val MIN_SUMMARY_SAMPLES = 3
private const val TREND_POINT_LIMIT = 60
private const val STORY_THEME_LIMIT = 48
