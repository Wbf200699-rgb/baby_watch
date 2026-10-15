package com.example.baby_watch.ui.things

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.History
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.SafetyEvent
import com.example.baby_watch.ui.theme.Background
import com.example.baby_watch.ui.theme.Level1Yellow
import com.example.baby_watch.ui.theme.Level2Purple
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.PrimaryContainer
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.TextPrimary
import com.example.baby_watch.ui.theme.TextSecondary
import com.example.baby_watch.ui.theme.TextTertiary

private enum class EventFilter(val label: String) {
    All("全部"),
    Pending("待确认"),
    Acknowledged("已确认"),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogScreen(modifier: Modifier = Modifier) {
    val events by MobileServerRepository.events.collectAsState()
    var filter by remember { mutableStateOf(EventFilter.All) }
    val filtered = events.filter {
        when (filter) {
            EventFilter.All -> true
            EventFilter.Pending -> !it.acknowledged
            EventFilter.Acknowledged -> it.acknowledged
        }
    }

    Scaffold(
        containerColor = Background,
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            "安全事件",
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                            color = TextPrimary,
                        )
                        Text(
                            "服务端历史告警与人工确认",
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
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            FilterTabs(selected = filter, onSelect = { filter = it })
            if (filtered.isEmpty()) {
                EmptyState(filter)
            } else {
                LazyColumn(verticalArrangement = Arrangement.spacedBy(0.dp)) {
                    itemsIndexed(
                        items = filtered,
                        key = { _, event -> event.id },
                    ) { index, event ->
                        TimelineItem(
                            event = event,
                            isFirst = index == 0,
                            isLast = index == filtered.lastIndex,
                            onAcknowledge = {
                                MobileServerRepository.acknowledgeEvent(event.id)
                            },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun FilterTabs(selected: EventFilter, onSelect: (EventFilter) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(PrimaryContainer)
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        EventFilter.entries.forEach { tab ->
            val active = selected == tab
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(11.dp))
                    .background(if (active) Color.White else Color.Transparent)
                    .clickable { onSelect(tab) }
                    .padding(vertical = 10.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    tab.label,
                    fontSize = 13.sp,
                    fontWeight = if (active) FontWeight.Bold else FontWeight.Medium,
                    color = if (active) Primary else TextSecondary,
                )
            }
        }
    }
}

@Composable
private fun TimelineItem(
    event: SafetyEvent,
    isFirst: Boolean,
    isLast: Boolean,
    onAcknowledge: () -> Unit,
) {
    val color = severityColor(event.severity)
    Row(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.width(36.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(modifier = Modifier.height(if (isFirst) 10.dp else 0.dp))
            if (!isFirst) {
                Box(
                    modifier = Modifier
                        .width(2.dp)
                        .height(10.dp)
                        .background(PrimaryContainer)
                )
            }
            Box(
                modifier = Modifier
                    .size(11.dp)
                    .clip(CircleShape)
                    .background(color)
            )
            if (!isLast) {
                Box(
                    modifier = Modifier
                        .width(2.dp)
                        .height(if (event.acknowledged) 90.dp else 124.dp)
                        .background(PrimaryContainer)
                )
            }
        }
        Card(
            modifier = Modifier
                .weight(1f)
                .padding(bottom = 10.dp),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = if (event.severity >= 3 && !event.acknowledged) {
                    Color(0xFFFFF4F4)
                } else {
                    Color.White
                }
            ),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        ) {
            Column(modifier = Modifier.padding(15.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        eventTime(event.receivedAt),
                        fontSize = 12.sp,
                        color = TextSecondary,
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    SeverityBadge(event.severity)
                    Spacer(modifier = Modifier.weight(1f))
                    if (event.acknowledged) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.CheckCircle,
                                contentDescription = null,
                                tint = SafeGreen,
                                modifier = Modifier.size(15.dp),
                            )
                            Spacer(Modifier.width(4.dp))
                            Text("已确认", fontSize = 11.sp, color = SafeGreen)
                        }
                    }
                }
                Spacer(modifier = Modifier.height(7.dp))
                Text(
                    event.title,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Bold,
                    color = if (event.severity >= 3) Level3Red else TextPrimary,
                )
                Spacer(modifier = Modifier.height(3.dp))
                Text(
                    event.detail,
                    fontSize = 13.sp,
                    color = TextSecondary,
                    lineHeight = 18.sp,
                )
                if (!event.acknowledged) {
                    TextButton(
                        onClick = onAcknowledge,
                        modifier = Modifier.align(Alignment.End),
                    ) {
                        Text("确认告警")
                    }
                }
            }
        }
    }
}

@Composable
private fun SeverityBadge(severity: Int) {
    val color = severityColor(severity)
    Text(
        "级别 $severity",
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        color = color,
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(color.copy(alpha = 0.12f))
            .padding(horizontal = 7.dp, vertical = 3.dp),
    )
}

@Composable
private fun EmptyState(filter: EventFilter) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 42.dp, horizontal = 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Icon(
                Icons.Default.History,
                contentDescription = null,
                tint = TextTertiary,
                modifier = Modifier.size(42.dp),
            )
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                when (filter) {
                    EventFilter.All -> "暂无安全事件"
                    EventFilter.Pending -> "没有待确认告警"
                    EventFilter.Acknowledged -> "暂无已确认告警"
                },
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                color = TextPrimary,
            )
            Text(
                "服务端产生新事件后会自动同步到这里。",
                fontSize = 13.sp,
                color = TextSecondary,
            )
        }
    }
}

private fun severityColor(severity: Int): Color = when (severity) {
    0 -> SafeGreen
    1, 2 -> Level1Yellow
    3, 4 -> Level2Purple
    else -> Level3Red
}

private fun eventTime(value: String): String {
    if (value.isBlank()) return "--"
    val date = value.substringBefore('T')
    val time = value.substringAfter('T', "").take(8)
    return if (time.isBlank()) value else "$date $time"
}
