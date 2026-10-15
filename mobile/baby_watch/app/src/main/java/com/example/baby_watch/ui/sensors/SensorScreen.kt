package com.example.baby_watch.ui.sensors

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
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
import androidx.compose.material.icons.filled.AutoStories
import androidx.compose.material.icons.filled.DeviceThermostat
import androidx.compose.material.icons.filled.GridOn
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.BiasAlignment
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.baby_watch.data.server.MatrixFrame
import com.example.baby_watch.data.server.Bme688Summary
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.PressureSummary
import com.example.baby_watch.data.server.Scd41Summary
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.ui.theme.Background
import com.example.baby_watch.ui.theme.Level3Red
import com.example.baby_watch.ui.theme.Level1Yellow
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.PrimaryContainer
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.Secondary
import com.example.baby_watch.ui.theme.TextPrimary
import com.example.baby_watch.ui.theme.TextSecondary
import com.example.baby_watch.ui.theme.TextTertiary
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.abs

private enum class MatrixPalette {
    Thermal,
    Pressure,
}

private data class MatrixStatistics(
    val values: List<Int>,
    val minimum: Int,
    val maximum: Int,
    val centerValue: Int?,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SensorScreen(modifier: Modifier = Modifier) {
    val repositoryState by MobileServerRepository.state.collectAsStateWithLifecycle()
    val thermalMatrix by MobileServerRepository.thermalMatrix.collectAsStateWithLifecycle()
    val pressureMatrix by MobileServerRepository.pressureMatrix.collectAsStateWithLifecycle()
    val snapshot = repositoryState.snapshot
    val deviceOnline = repositoryState.connectionStatus ==
        ServerConnectionStatus.Connected && snapshot.device.online
    val storyTransferActive = snapshot.storyTransfer.active

    Scaffold(
        containerColor = Background,
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Icon(
                                Icons.Default.Sensors,
                                contentDescription = null,
                                tint = Primary,
                                modifier = Modifier.size(26.dp),
                            )
                            Spacer(Modifier.width(9.dp))
                            Text(
                                "数据中心",
                                fontSize = 24.sp,
                                fontWeight = FontWeight.Bold,
                                color = TextPrimary,
                            )
                        }
                        Text(
                             when {
                                 repositoryState.connectionStatus != ServerConnectionStatus.Connected ->
                                     repositoryState.errorMessage ?: "正在连接服务端…"
                                 storyTransferActive -> "AI 故事传输中 · 传感器刷新暂缓"
                                 snapshot.device.online -> "服务端实时同步 · 设备在线"
                                else -> "服务端已连接 · 设备离线"
                            },
                            fontSize = 13.sp,
                            color = TextSecondary,
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Background),
            )
        },
    ) { innerPadding ->
        LazyColumn(
            modifier = modifier
                .fillMaxSize()
                .padding(innerPadding),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
         ) {
            if (storyTransferActive) {
                item(key = "story-transfer-pause") {
                    SensorRefreshPauseCard()
                }
            }
            item {
                FivePointTemperatureCard(
                    values = snapshot.fivePointTemperaturesC,
                    deviceOnline = deviceOnline,
                )
            }
            item {
                EnvironmentSensorCard(
                    scd41 = snapshot.scd41,
                    bme688 = snapshot.bme688,
                    bme688Supported = repositoryState.capabilities.bme688Supported,
                    deviceOnline = deviceOnline,
                )
            }
            item {
                val evidence = snapshot.fusion.evidence
                MatrixImageCard(
                    title = "热成像",
                    description = "服务端实时热成像矩阵",
                    icon = Icons.Default.DeviceThermostat,
                    matrix = thermalMatrix,
                    palette = MatrixPalette.Thermal,
                    thermalCenterC = snapshot.thermalCenterC,
                    fusionEvidenceAvailable = snapshot.fusion.schemaVersion >= 5 && evidence.available,
                    thermalSourceAvailable = snapshot.fusion.thermalEvidenceAvailable,
                    bedTemperatureSourceAvailable = snapshot.fusion.bedTemperatureEvidenceAvailable,
                    fusionSchemaVersion = snapshot.fusion.schemaVersion,
                    fusionPresenceLabel = snapshot.fusion.contexts["presence"]?.label.orEmpty(),
                    bodyTemperatureLabel = snapshot.fusion.contexts["body_temperature"]?.label.orEmpty(),
                    thermalPresenceX1000 = evidence.thermalPresenceX1000,
                    thermalHotspotPoints = evidence.thermalHotspot.points,
                    thermalHotspotX1000 = evidence.thermalHotspot.xX1000,
                    thermalHotspotY1000 = evidence.thermalHotspot.yX1000,
                    thermalHotspotMeanC = evidence.thermalHotspot.meanC,
                    bedLeftC = evidence.bedTemperature.leftC,
                    bedCenterC = evidence.bedTemperature.centerC,
                    bedRightC = evidence.bedTemperature.rightC,
                    bedSpreadC = evidence.bedTemperature.spreadC,
                    hotspotRegionLabel = evidence.bedTemperature.hotspotRegionLabel,
                    deviceOnline = deviceOnline,
                )
            }
            item {
                MatrixImageCard(
                    title = "压力分布",
                    description = "服务端实时压力矩阵",
                    icon = Icons.Default.GridOn,
                    matrix = pressureMatrix,
                    palette = MatrixPalette.Pressure,
                    pressure = snapshot.pressure,
                    deviceOnline = deviceOnline,
                )
            }
        }
    }
}

@Composable
private fun SensorRefreshPauseCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(40.dp)
                    .background(MaterialTheme.colorScheme.secondary.copy(alpha = 0.14f), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Default.AutoStories,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.secondary,
                    modifier = Modifier.size(21.dp),
                )
            }
            Spacer(Modifier.width(12.dp))
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    "正在把故事送到设备",
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                )
                Text(
                    "为保证传输稳定，当前保留上次读数；完成后自动恢复实时刷新。",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.76f),
                )
            }
        }
    }
}

@Composable
private fun EnvironmentSensorCard(
    scd41: Scd41Summary,
    bme688: Bme688Summary,
    bme688Supported: Boolean,
    deviceOnline: Boolean,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
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
                        .clip(CircleShape)
                        .background(Color(0xFFE8F7F1)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Default.Sensors, null, tint = SafeGreen, modifier = Modifier.size(22.dp))
                }
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text("环境监测", fontSize = 17.sp, fontWeight = FontWeight.Bold, color = TextPrimary)
                    Text(
                        if (deviceOnline) "SCD41 CO₂ · BME688 环境参数" else "显示服务端最近一次环境数据",
                        fontSize = 12.sp,
                        color = TextSecondary,
                    )
                }
            }

            Text("SCD41", fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = Primary)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                EnvironmentValue(
                    Modifier.weight(1f), "CO₂",
                    scd41.co2Ppm?.let { "$it ppm" } ?: "--",
                    scd41.valid,
                )
                EnvironmentValue(
                    Modifier.weight(1f), "温度",
                    scd41.temperatureC?.let { "%.1f°C".format(it) } ?: "--",
                    scd41.valid,
                )
                EnvironmentValue(
                    Modifier.weight(1f), "湿度",
                    scd41.humidityPercent?.let { "%.1f%%".format(it) } ?: "--",
                    scd41.valid,
                )
            }

            Text("BME688", fontSize = 12.sp, fontWeight = FontWeight.SemiBold, color = Primary)
            if (!bme688Supported && !bme688.supported) {
                Text("当前服务端未声明 BME688 能力", fontSize = 12.sp, color = TextTertiary)
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    EnvironmentValue(
                        Modifier.weight(1f), "温度",
                        bme688.temperatureC?.let { "%.1f°C".format(it) } ?: "--",
                        bme688.climateValid,
                    )
                    EnvironmentValue(
                        Modifier.weight(1f), "湿度",
                        bme688.humidityPercent?.let { "%.1f%%".format(it) } ?: "--",
                        bme688.climateValid,
                    )
                    EnvironmentValue(
                        Modifier.weight(1f), "气压",
                        bme688.pressureHpa?.let { "%.1f hPa".format(it) } ?: "--",
                        bme688.climateValid,
                    )
                }
                EnvironmentValue(
                    Modifier.fillMaxWidth(),
                    "原始气阻（非 VOC/IAQ）",
                    bme688.rawGasKohm?.let { "%.1f kΩ".format(it) } ?: "--",
                    bme688.gasValid,
                )
            }
        }
    }
}

@Composable
private fun EnvironmentValue(
    modifier: Modifier,
    label: String,
    value: String,
    valid: Boolean,
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(if (valid) Color(0xFFF0F9F5) else Color(0xFFF5F6F8))
            .padding(horizontal = 10.dp, vertical = 9.dp),
    ) {
        Text(label, fontSize = 10.sp, color = TextSecondary)
        Text(value, fontSize = 14.sp, fontWeight = FontWeight.Bold, color = if (valid) TextPrimary else TextTertiary)
    }
}

@Composable
private fun FivePointTemperatureCard(values: List<Float?>, deviceOnline: Boolean) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(CircleShape)
                        .background(PrimaryContainer),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        Icons.Default.DeviceThermostat,
                        contentDescription = null,
                        tint = Primary,
                        modifier = Modifier.size(22.dp),
                    )
                }
                Spacer(Modifier.width(10.dp))
                Column {
                    Text(
                        "五点温度",
                        fontSize = 17.sp,
                        fontWeight = FontWeight.Bold,
                        color = TextPrimary,
                    )
                    Text(
                        if (deviceOnline) {
                            "四角与中心位置 · 实时"
                        } else {
                            "四角与中心位置 · 最近一次数据"
                        },
                        fontSize = 12.sp,
                        color = TextSecondary,
                    )
                }
            }

            Spacer(Modifier.height(16.dp))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(1.65f)
                    .clip(RoundedCornerShape(16.dp))
                    .background(
                        Brush.linearGradient(
                            listOf(Color(0xFFF8FBFF), Color(0xFFFFF8EE))
                        )
                    )
                    .border(
                        width = 1.5.dp,
                        color = Color(0xFFC9D6E5),
                        shape = RoundedCornerShape(16.dp),
                    )
                    .padding(horizontal = 18.dp, vertical = 14.dp),
            ) {
                TemperaturePoint(
                    modifier = Modifier.align(Alignment.TopStart),
                    label = "T2 左上",
                    value = values.getOrNull(1),
                )
                TemperaturePoint(
                    modifier = Modifier.align(Alignment.TopEnd),
                    label = "T5 右上",
                    value = values.getOrNull(4),
                )
                TemperaturePoint(
                    modifier = Modifier.align(Alignment.Center),
                    label = "T4 中心",
                    value = values.getOrNull(3),
                )
                TemperaturePoint(
                    modifier = Modifier.align(Alignment.BottomStart),
                    label = "T1 左下",
                    value = values.getOrNull(0),
                )
                TemperaturePoint(
                    modifier = Modifier.align(Alignment.BottomEnd),
                    label = "T3 右下",
                    value = values.getOrNull(2),
                )
            }
            Spacer(Modifier.height(10.dp))
            Text(
                if (deviceOnline) {
                    "物理映射：T1 左下、T2 左上、T3 右下、T4 中心、T5 右上。"
                } else {
                    "设备当前离线，以上为服务端保留的最近一次数据。"
                },
                fontSize = 11.sp,
                color = TextTertiary,
            )
        }
    }
}

@Composable
private fun BoxScope.TemperaturePoint(
    modifier: Modifier,
    label: String,
    value: Float?,
) {
    Column(
        modifier = modifier.width(76.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            modifier = Modifier
                .size(52.dp)
                .clip(CircleShape)
                .background(temperatureColor(value))
                .border(2.dp, Color.White, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = value?.let { "%.1f°C".format(it) } ?: "--",
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                color = if (value == null) TextSecondary else Color.White,
            )
        }
        Spacer(Modifier.height(3.dp))
        Text(
            text = label,
            fontSize = 10.sp,
            fontWeight = FontWeight.SemiBold,
            color = TextSecondary,
        )
    }
}

@Composable
private fun MatrixImageCard(
    title: String,
    description: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    matrix: MatrixFrame?,
    palette: MatrixPalette,
    thermalCenterC: Float? = null,
    pressure: PressureSummary? = null,
    fusionEvidenceAvailable: Boolean = false,
    thermalSourceAvailable: Boolean = false,
    bedTemperatureSourceAvailable: Boolean = false,
    fusionSchemaVersion: Int = 1,
    fusionPresenceLabel: String = "unknown",
    bodyTemperatureLabel: String = "unknown",
    thermalPresenceX1000: Int = 0,
    thermalHotspotPoints: Int = 0,
    thermalHotspotX1000: Int = 0,
    thermalHotspotY1000: Int = 0,
    thermalHotspotMeanC: Float? = null,
    bedLeftC: Float? = null,
    bedCenterC: Float? = null,
    bedRightC: Float? = null,
    bedSpreadC: Float? = null,
    hotspotRegionLabel: String = "未知",
    deviceOnline: Boolean,
) {
    val statistics = remember(
        matrix?.sequence,
        matrix?.receivedAt,
        matrix?.rows,
        matrix?.cols,
    ) {
        matrix?.statisticsOrNull()
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 1.dp),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(38.dp)
                        .clip(CircleShape)
                        .background(
                            if (palette == MatrixPalette.Thermal) {
                                Color(0xFFFFEEE7)
                            } else {
                                Color(0xFFEAF2FF)
                            }
                        ),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        imageVector = icon,
                        contentDescription = null,
                        tint = if (palette == MatrixPalette.Thermal) Secondary else Primary,
                        modifier = Modifier.size(22.dp),
                    )
                }
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        title,
                        fontSize = 17.sp,
                        fontWeight = FontWeight.Bold,
                        color = TextPrimary,
                    )
                    Text(
                        matrixSummary(description, matrix, palette, deviceOnline),
                        fontSize = 12.sp,
                        color = TextSecondary,
                    )
                }
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .clip(CircleShape)
                        .background(
                            when {
                                matrix == null || !deviceOnline -> TextTertiary
                                matrix.delayed -> Level1Yellow
                                else -> SafeGreen
                            }
                        )
                )
            }

            Spacer(Modifier.height(14.dp))
            if (matrix == null || statistics == null) {
                EmptyMatrixPlaceholder(palette)
            } else {
                MatrixCanvas(
                    matrix = matrix,
                    palette = palette,
                    statistics = statistics,
                    hotspotX1000 = thermalHotspotX1000.takeIf {
                        fusionEvidenceAvailable && thermalSourceAvailable && thermalHotspotPoints > 0
                    },
                    hotspotY1000 = thermalHotspotY1000.takeIf {
                        fusionEvidenceAvailable && thermalSourceAvailable && thermalHotspotPoints > 0
                    },
                )
                Spacer(Modifier.height(10.dp))
                MatrixLegend(matrix = matrix, palette = palette, statistics = statistics)
                Text(
                    "${if (deviceOnline) "实时序列" else "最近序列"} ${matrix.sequence} · ${
                        matrix.receivedAt.ifBlank { "等待时间戳" }
                    }",
                    modifier = Modifier.padding(top = 8.dp),
                    fontSize = 11.sp,
                    color = TextTertiary,
                )
                Text(
                    buildString {
                        matrix.fps?.let { append("%.2f FPS".format(it)) }
                        matrix.frameAgeMs?.let {
                            if (isNotEmpty()) append(" · ")
                            append("延迟 ${it} ms")
                        }
                        if (matrix.drops > 0) {
                            if (isNotEmpty()) append(" · ")
                            append("丢帧 ${matrix.drops}")
                        }
                        if (matrix.delayed) {
                            if (isNotEmpty()) append(" · ")
                            append("数据延迟")
                        }
                    }.ifBlank { "等待帧质量指标" },
                    modifier = Modifier.padding(top = 4.dp),
                    fontSize = 11.sp,
                    fontWeight = if (matrix.delayed) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (matrix.delayed) Level1Yellow else TextTertiary,
                )
                if (palette == MatrixPalette.Thermal) {
                    Spacer(Modifier.height(12.dp))
                    ThermalTemperatureSummary(
                        matrix = matrix,
                        statistics = statistics,
                        serverCenterC = thermalCenterC,
                    )
                } else if (pressure != null) {
                    Spacer(Modifier.height(12.dp))
                    PressureSignalSummary(pressure)
                }
            }
            if (palette == MatrixPalette.Thermal && fusionEvidenceAvailable) {
                Spacer(Modifier.height(12.dp))
                ThermalFusionSummary(
                    hotspotPoints = thermalHotspotPoints,
                    hotspotX1000 = thermalHotspotX1000,
                    hotspotY1000 = thermalHotspotY1000,
                    hotspotMeanC = thermalHotspotMeanC,
                    bedLeftC = bedLeftC,
                    bedCenterC = bedCenterC,
                    bedRightC = bedRightC,
                    bedSpreadC = bedSpreadC,
                    hotspotRegionLabel = hotspotRegionLabel,
                    thermalSourceAvailable = thermalSourceAvailable,
                    bedTemperatureSourceAvailable = bedTemperatureSourceAvailable,
                    fusionSchemaVersion = fusionSchemaVersion,
                    fusionPresenceLabel = fusionPresenceLabel,
                    bodyTemperatureLabel = bodyTemperatureLabel,
                    thermalPresenceX1000 = thermalPresenceX1000,
                )
            }
        }
    }
}

@Composable
private fun PressureSignalSummary(pressure: PressureSummary) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        EnvironmentValue(
            Modifier.weight(1f),
            "总压力",
            pressure.total?.toString() ?: "--",
            pressure.total != null,
        )
        EnvironmentValue(
            Modifier.weight(1f),
            "边缘压力",
            pressure.edge?.toString() ?: "--",
            pressure.edge != null,
        )
        EnvironmentValue(
            Modifier.weight(1f),
            "有效点",
            pressure.activePoints?.toString() ?: "--",
            pressure.activePoints != null,
        )
    }
}

@Composable
private fun ThermalTemperatureSummary(
    matrix: MatrixFrame,
    statistics: MatrixStatistics,
    serverCenterC: Float?,
) {
    val minimumC = thermalValueC(statistics.minimum, matrix.unit)
    val maximumC = thermalValueC(statistics.maximum, matrix.unit)
    val matrixCenterC = statistics.centerValue?.let { thermalValueC(it, matrix.unit) }
    val centerC = serverCenterC ?: matrixCenterC

    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        ThermalTemperatureItem(
            modifier = Modifier.weight(1f),
            label = "最高温度",
            value = maximumC,
            color = Level3Red,
            background = Color(0xFFFFECEA),
        )
        ThermalTemperatureItem(
            modifier = Modifier.weight(1f),
            label = "中心温度",
            value = centerC,
            color = Secondary,
            background = Color(0xFFFFF3E8),
        )
        ThermalTemperatureItem(
            modifier = Modifier.weight(1f),
            label = "最低温度",
            value = minimumC,
            color = Primary,
            background = Color(0xFFEAF2FF),
        )
    }
}

@Composable
private fun ThermalFusionSummary(
    hotspotPoints: Int,
    hotspotX1000: Int,
    hotspotY1000: Int,
    hotspotMeanC: Float?,
    bedLeftC: Float?,
    bedCenterC: Float?,
    bedRightC: Float?,
    bedSpreadC: Float?,
    hotspotRegionLabel: String,
    thermalSourceAvailable: Boolean,
    bedTemperatureSourceAvailable: Boolean,
    fusionSchemaVersion: Int,
    fusionPresenceLabel: String,
    bodyTemperatureLabel: String,
    thermalPresenceX1000: Int,
) {
    val hotspotAvailable = thermalSourceAvailable && hotspotPoints > 0
    val feverScreeningSupported = fusionSchemaVersion >= 8
    val feverScreeningValid = feverScreeningSupported &&
        thermalSourceAvailable &&
        fusionPresenceLabel.equals("present", ignoreCase = true) &&
        thermalPresenceX1000 >= 550 &&
        hotspotPoints >= 4 &&
        hotspotMeanC?.isFinite() == true &&
        bodyTemperatureLabel.lowercase() in setOf("normal", "high")
    val hotspotRegionAvailable = bedTemperatureSourceAvailable &&
        hotspotRegionLabel.isNotBlank() &&
        hotspotRegionLabel != "--" &&
        hotspotRegionLabel != "未知"

    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(
            "床面温度分区",
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color = TextPrimary,
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            EnvironmentValue(
                Modifier.weight(1f),
                "左侧",
                bedLeftC?.takeIf { bedTemperatureSourceAvailable }?.let { "%.1f°C".format(it) } ?: "--",
                bedTemperatureSourceAvailable && bedLeftC != null,
            )
            EnvironmentValue(
                Modifier.weight(1f),
                "中心",
                bedCenterC?.takeIf { bedTemperatureSourceAvailable }?.let { "%.1f°C".format(it) } ?: "--",
                bedTemperatureSourceAvailable && bedCenterC != null,
            )
            EnvironmentValue(
                Modifier.weight(1f),
                "右侧",
                bedRightC?.takeIf { bedTemperatureSourceAvailable }?.let { "%.1f°C".format(it) } ?: "--",
                bedTemperatureSourceAvailable && bedRightC != null,
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            EnvironmentValue(
                Modifier.weight(1f),
                "区域温差",
                bedSpreadC?.takeIf { bedTemperatureSourceAvailable }?.let { "%.1f°C".format(it) } ?: "--",
                bedTemperatureSourceAvailable && bedSpreadC != null,
            )
            EnvironmentValue(
                Modifier.weight(1f),
                "热点区",
                hotspotRegionLabel.takeIf { hotspotRegionAvailable } ?: "--",
                hotspotRegionAvailable,
            )
        }

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xFFFFF3E8), RoundedCornerShape(14.dp))
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        if (feverScreeningValid) {
                            "体表温度筛查（非医疗测温）"
                        } else {
                            "热源区域均温（不可用于体温判断）"
                        },
                        fontSize = 12.sp,
                        color = TextSecondary,
                    )
                    Text(
                        hotspotMeanC?.takeIf { hotspotAvailable }?.let { "%.1f°C".format(it) } ?: "--",
                        fontSize = 19.sp,
                        fontWeight = FontWeight.Bold,
                        color = if (hotspotAvailable) Secondary else TextTertiary,
                    )
                }
                Text(
                    if (thermalSourceAvailable) {
                        "有效热源点 ${hotspotPoints.coerceAtLeast(0)}"
                    } else {
                        "热成像来源暂不可用"
                    },
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = TextSecondary,
                )
            }
            Text(
                if (!thermalSourceAvailable) {
                    "等待有效热成像融合证据"
                } else if (feverScreeningSupported && feverScreeningValid) {
                    val screening = if (bodyTemperatureLabel.equals("high", ignoreCase = true)) {
                        "筛查偏高，请用合格体温计复测"
                    } else {
                        "当前筛查未见温度偏高"
                    }
                    "$screening · 热图位置 X ${normalizedCoordinateLabel(hotspotX1000)} · Y ${normalizedCoordinateLabel(hotspotY1000)}"
                } else if (feverScreeningSupported && !fusionPresenceLabel.equals("present", ignoreCase = true)) {
                    "宝宝在床证据不足，当前不作体温筛查判断"
                } else if (feverScreeningSupported && (thermalPresenceX1000 < 550 || hotspotPoints < 4)) {
                    "热源证据不足，当前不作体温筛查判断"
                } else if (hotspotPoints > 0) {
                    "热图内归一化位置 · X ${normalizedCoordinateLabel(hotspotX1000)} · Y ${normalizedCoordinateLabel(hotspotY1000)}"
                } else {
                    "当前热图内未形成有效热源区域"
                },
                fontSize = 11.sp,
                color = TextTertiary,
            )
            Text(
                "热成像受距离、环境与遮挡影响，仅作非接触筛查；疑似发热请用体温计复测。",
                fontSize = 11.sp,
                color = TextTertiary,
            )
        }
    }
}

@Composable
private fun ThermalTemperatureItem(
    modifier: Modifier,
    label: String,
    value: Float?,
    color: Color,
    background: Color,
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(background)
            .padding(horizontal = 8.dp, vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = value?.let { "%.1f°C".format(it) } ?: "--",
            fontSize = 16.sp,
            fontWeight = FontWeight.Bold,
            color = color,
        )
        Spacer(Modifier.height(2.dp))
        Text(
            text = label,
            fontSize = 11.sp,
            color = TextSecondary,
        )
    }
}

@Composable
private fun EmptyMatrixPlaceholder(palette: MatrixPalette) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .aspectRatio(if (palette == MatrixPalette.Thermal) 1.25f else 1f)
            .clip(RoundedCornerShape(14.dp))
            .background(Color(0xFFF4F6F9))
            .border(1.dp, Color(0xFFD8DEE8), RoundedCornerShape(14.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                imageVector = if (palette == MatrixPalette.Thermal) {
                    Icons.Default.DeviceThermostat
                } else {
                    Icons.Default.GridOn
                },
                contentDescription = null,
                tint = TextTertiary,
                modifier = Modifier.size(38.dp),
            )
            Spacer(Modifier.height(8.dp))
            Text("等待服务端矩阵数据", fontSize = 13.sp, color = TextSecondary)
        }
    }
}

@Composable
private fun MatrixCanvas(
    matrix: MatrixFrame,
    palette: MatrixPalette,
    statistics: MatrixStatistics,
    hotspotX1000: Int? = null,
    hotspotY1000: Int? = null,
) {
    val aspect = (matrix.cols.toFloat() / matrix.rows.toFloat()).coerceIn(0.8f, 2f)
    val image by produceState<ImageBitmap?>(
        initialValue = null,
        key1 = matrix.sequence,
        key2 = matrix.receivedAt,
        key3 = palette,
    ) {
        value = withContext(Dispatchers.Default) {
            val range = (statistics.maximum - statistics.minimum).toFloat()
            val pixels = IntArray(statistics.values.size) { index ->
                val raw = statistics.values[index]
                val normalized = if (range == 0f) {
                    if (raw == 0) 0f else 0.5f
                } else {
                    ((raw - statistics.minimum) / range).coerceIn(0f, 1f)
                }
                matrixColor(normalized, palette).toArgb()
            }
            Bitmap.createBitmap(
                pixels,
                matrix.cols,
                matrix.rows,
                Bitmap.Config.ARGB_8888,
            ).asImageBitmap()
        }
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .aspectRatio(aspect)
            .clip(RoundedCornerShape(14.dp))
            .background(Color(0xFFEDF1F6))
            .border(1.dp, Color(0xFFD8DEE8), RoundedCornerShape(14.dp)),
        contentAlignment = Alignment.Center,
    ) {
        image?.let { rendered ->
            Image(
                bitmap = rendered,
                contentDescription = if (palette == MatrixPalette.Thermal) {
                    "热成像矩阵"
                } else {
                    "压力分布矩阵"
                },
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.FillBounds,
                filterQuality = FilterQuality.None,
            )
        }
        if (
            palette == MatrixPalette.Thermal &&
            hotspotX1000 != null && hotspotY1000 != null &&
            hotspotX1000 in 0..1000 && hotspotY1000 in 0..1000
        ) {
            Box(
                modifier = Modifier
                    .align(
                        BiasAlignment(
                            horizontalBias = hotspotX1000 / 500f - 1f,
                            verticalBias = hotspotY1000 / 500f - 1f,
                        )
                    )
                    .size(22.dp)
                    .background(Level3Red.copy(alpha = 0.20f), CircleShape)
                    .border(2.dp, Color.White, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .size(7.dp)
                        .background(Level3Red, CircleShape)
                )
            }
        }
    }
}

@Composable
private fun MatrixLegend(
    matrix: MatrixFrame,
    palette: MatrixPalette,
    statistics: MatrixStatistics,
) {
    val startLabel: String
    val endLabel: String
    if (palette == MatrixPalette.Thermal) {
        startLabel = formatThermalValue(statistics.minimum, matrix.unit)
        endLabel = formatThermalValue(statistics.maximum, matrix.unit)
    } else {
        startLabel = "低压力 ${statistics.minimum}"
        endLabel = "高压力 ${statistics.maximum}"
    }

    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(startLabel, fontSize = 11.sp, color = TextSecondary)
        Box(
            modifier = Modifier
                .padding(horizontal = 10.dp)
                .weight(1f)
                .height(8.dp)
                .clip(RoundedCornerShape(50))
                .background(
                    Brush.horizontalGradient(
                        if (palette == MatrixPalette.Thermal) {
                            listOf(
                                Color(0xFF3157D5),
                                Color(0xFF38C7C1),
                                Color(0xFFFFD166),
                                Color(0xFFE53935),
                            )
                        } else {
                            listOf(
                                Color(0xFFF0F4FA),
                                Color(0xFF6CA6FF),
                                Color(0xFF6750A4),
                                Color(0xFFD81B60),
                            )
                        }
                    )
                )
        )
        Text(endLabel, fontSize = 11.sp, color = TextSecondary)
    }
}

private fun matrixSummary(
    description: String,
    matrix: MatrixFrame?,
    palette: MatrixPalette,
    deviceOnline: Boolean,
): String {
    if (matrix == null) return "$description · 等待数据"
    val status = when {
        !deviceOnline -> "最近一次"
        matrix.delayed -> "数据延迟"
        else -> "实时"
    }
    return if (palette == MatrixPalette.Pressure) {
        "$status · ${matrix.rows}×${matrix.cols} · ${matrix.activePoints} 个受压点"
    } else {
        "$status · ${matrix.rows}×${matrix.cols} · ${matrix.unit.ifBlank { "温度" }}"
    }
}

private fun matrixColor(normalized: Float, palette: MatrixPalette): Color {
    return when (palette) {
        MatrixPalette.Thermal -> when {
            normalized < 0.34f -> lerp(
                Color(0xFF3157D5),
                Color(0xFF38C7C1),
                normalized / 0.34f,
            )

            normalized < 0.68f -> lerp(
                Color(0xFF38C7C1),
                Color(0xFFFFD166),
                (normalized - 0.34f) / 0.34f,
            )

            else -> lerp(
                Color(0xFFFFD166),
                Color(0xFFE53935),
                (normalized - 0.68f) / 0.32f,
            )
        }

        MatrixPalette.Pressure -> when {
            normalized < 0.4f -> lerp(
                Color(0xFFDDE7F4),
                Color(0xFF6CA6FF),
                normalized / 0.4f,
            )

            normalized < 0.75f -> lerp(
                Color(0xFF6CA6FF),
                Color(0xFF6750A4),
                (normalized - 0.4f) / 0.35f,
            )

            else -> lerp(
                Color(0xFF6750A4),
                Color(0xFFD81B60),
                (normalized - 0.75f) / 0.25f,
            )
        }
    }
}

private fun temperatureColor(value: Float?): Color = when {
    value == null -> Color(0xFFE5E9F0)
    value < 20f -> Color(0xFF4F80E1)
    value < 30f -> Color(0xFF24A68A)
    value < 38f -> Color(0xFFFF9F43)
    else -> Level3Red
}

private fun MatrixFrame.statisticsOrNull(): MatrixStatistics? {
    if (rows <= 0 || cols <= 0) return null
    val expected = rows * cols
    if (expected <= 0 || values.size < expected) return null
    val frameValues = if (values.size == expected) values else values.take(expected)
    val frameMinimum = minimum ?: frameValues.minOrNull() ?: return null
    val frameMaximum = maximum ?: frameValues.maxOrNull() ?: frameMinimum
    val centerIndex = (rows / 2) * cols + (cols / 2)
    return MatrixStatistics(
        values = frameValues,
        minimum = frameMinimum,
        maximum = frameMaximum,
        centerValue = frameValues.getOrNull(centerIndex),
    )
}

private fun normalizedCoordinateLabel(valueX1000: Int): String =
    "%.0f%%".format(valueX1000.coerceIn(0, 1000) / 10f)

private fun formatThermalValue(value: Int, unit: String): String {
    return "%.1f°C".format(thermalValueC(value, unit))
}

private fun thermalValueC(value: Int, unit: String): Float {
    val normalizedUnit = unit.trim().lowercase()
    return when {
        normalizedUnit in setOf("mc", "millidegree_c", "millicelsius") -> value / 1000f
        normalizedUnit in setOf("cd", "cdeg", "centidegree_c", "centicelsius") -> value / 100f
        normalizedUnit in setOf("dc", "ddeg", "deci_c", "decidegree_c", "decicelsius") -> value / 10f
        normalizedUnit in setOf("c", "°c", "celsius") -> value.toFloat()
        abs(value) >= 1000 -> value / 100f
        abs(value) >= 100 -> value / 10f
        else -> value.toFloat()
    }
}
