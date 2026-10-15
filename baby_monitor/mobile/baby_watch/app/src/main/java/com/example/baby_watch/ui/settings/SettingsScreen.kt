package com.example.baby_watch.ui.settings

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.Devices
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.FamilyRestroom
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Router
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.baby_watch.data.server.MobileServerRepository
import com.example.baby_watch.data.server.NetworkTransport
import com.example.baby_watch.data.server.ServerConnectionStatus
import com.example.baby_watch.data.server.ServerEndpointMode
import com.example.baby_watch.data.server.isAllowedServerUrl
import com.example.baby_watch.ui.components.ConnectionBanner
import com.example.baby_watch.ui.components.InlineError
import com.example.baby_watch.ui.components.ScreenHeader
import com.example.baby_watch.ui.components.SectionHeader
import com.example.baby_watch.ui.theme.Primary
import com.example.baby_watch.ui.theme.SafeGreen
import com.example.baby_watch.ui.theme.Secondary

@Composable
fun SettingsScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val repositoryState by MobileServerRepository.state.collectAsStateWithLifecycle()
    val configuration = repositoryState.configuration
    var advancedExpanded by remember { mutableStateOf(false) }
    var baseUrl by remember(configuration.activeMode, configuration.baseUrl) {
        mutableStateOf(configuration.baseUrl)
    }
    var username by remember(configuration.activeMode, configuration.username) {
        mutableStateOf(configuration.username)
    }
    var password by remember(configuration.activeMode) { mutableStateOf("") }
    var passwordVisible by remember(configuration.activeMode) { mutableStateOf(false) }

    LaunchedEffect(Unit) { MobileServerRepository.ensureInitialized(context) }

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            start = 20.dp,
            top = 20.dp,
            end = 20.dp,
            bottom = 32.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item {
            ScreenHeader(
                eyebrow = "账户与设备",
                title = "我的",
                subtitle = "照护成员、设备与隐私设置",
            )
        }
        item { ProfileCard(repositoryState.snapshot.device.online) }
        item { ConnectionBanner(repositoryState) }

        item { SectionHeader("服务连接") }
        item {
            ConnectionModeCard(
                activeMode = configuration.activeMode,
                cloudUrl = configuration.cloud.baseUrl,
                localUrl = configuration.local.baseUrl,
                status = repositoryState.connectionStatus,
                onUseCloud = {
                    password = ""
                    passwordVisible = false
                    MobileServerRepository.switchEndpoint(context, ServerEndpointMode.Cloud)
                },
                onUseLocal = {
                    password = ""
                    passwordVisible = false
                    MobileServerRepository.switchEndpoint(context, ServerEndpointMode.Local)
                },
            )
        }

        item { SectionHeader("设备与照护") }
        item {
            SettingsGroup(
                rows = listOf(
                    SettingsRowData(
                        Icons.Default.Devices,
                        "宝宝监护器",
                        configuration.deviceId,
                        if (repositoryState.snapshot.device.online) SafeGreen else MaterialTheme.colorScheme.error,
                    ),
                    SettingsRowData(
                        Icons.Default.Router,
                        "网络连接",
                        networkLabel(repositoryState.network.transport, repositoryState.network.metered),
                        Primary,
                    ),
                    SettingsRowData(
                        Icons.Default.FamilyRestroom,
                        "照护成员",
                        "当前设备账户",
                        Secondary,
                    ),
                ),
            )
        }

        item { SectionHeader("通知与安全") }
        item {
            val notificationsEnabled = NotificationManagerCompat.from(context).areNotificationsEnabled()
            SettingsGroup(
                rows = listOf(
                    SettingsRowData(
                        Icons.Default.Notifications,
                        "异常通知",
                        if (notificationsEnabled) "已允许系统通知" else "需要在系统中开启",
                        if (notificationsEnabled) SafeGreen else MaterialTheme.colorScheme.error,
                        onClick = { openNotificationSettings(context) },
                    ),
                    SettingsRowData(
                        Icons.Default.Security,
                        "隐私保护",
                        "音视频仅在手动开启时接收",
                        Primary,
                    ),
                    SettingsRowData(
                        Icons.Default.Lock,
                        "凭据保护",
                        "密码已由 Android Keystore 加密",
                        Primary,
                    ),
                ),
            )
        }

        item {
            AdvancedConnectionCard(
                expanded = advancedExpanded,
                onExpandedChange = { advancedExpanded = !advancedExpanded },
                baseUrl = baseUrl,
                username = username,
                password = password,
                passwordVisible = passwordVisible,
                hasSavedPassword = configuration.hasPassword,
                activeMode = configuration.activeMode,
                status = repositoryState.connectionStatus,
                onBaseUrlChange = { baseUrl = it },
                onUsernameChange = { username = it },
                onPasswordChange = { password = it },
                onPasswordVisibilityChange = { passwordVisible = !passwordVisible },
                onReconnect = MobileServerRepository::reconnect,
                onSave = {
                    MobileServerRepository.saveConfiguration(
                        context,
                        configuration.activeMode,
                        baseUrl,
                        username,
                        password,
                    )
                    password = ""
                    passwordVisible = false
                },
            )
        }
        item { InlineError(repositoryState.errorMessage) }
        item {
            Text(
                "AI 分析仅用于辅助照护，不替代医生诊断或成人现场确认。高风险提示出现时，请第一时间查看宝宝。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 4.dp),
            )
        }
    }
}

@Composable
private fun ProfileCard(online: Boolean) {
    Card(
        shape = RoundedCornerShape(26.dp),
        colors = CardDefaults.cardColors(containerColor = Color.Transparent),
        elevation = CardDefaults.cardElevation(0.dp),
    ) {
        Row(
            modifier = Modifier
                .background(
                    Brush.linearGradient(
                        listOf(
                            MaterialTheme.colorScheme.primaryContainer,
                            MaterialTheme.colorScheme.secondaryContainer,
                        )
                    )
                )
                .fillMaxWidth()
                .padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(58.dp)
                    .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.86f), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Text("宝", style = MaterialTheme.typography.headlineSmall, color = Primary)
            }
            Spacer(Modifier.size(14.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("小宝贝", style = MaterialTheme.typography.titleLarge)
                Text(
                    if (online) "监护设备在线 · 正在守护" else "设备暂时离线",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Box(
                Modifier
                    .size(9.dp)
                    .background(if (online) SafeGreen else MaterialTheme.colorScheme.error, CircleShape)
            )
        }
    }
}

private data class SettingsRowData(
    val icon: ImageVector,
    val title: String,
    val subtitle: String,
    val tint: Color,
    val onClick: (() -> Unit)? = null,
)

@Composable
private fun SettingsGroup(rows: List<SettingsRowData>) {
    Card(
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column {
            rows.forEachIndexed { index, row ->
                SettingsRow(row)
                if (index != rows.lastIndex) {
                    HorizontalDivider(
                        modifier = Modifier.padding(start = 66.dp),
                        color = MaterialTheme.colorScheme.outline.copy(alpha = 0.55f),
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsRow(row: SettingsRowData) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(38.dp)
                .background(row.tint.copy(alpha = 0.11f), RoundedCornerShape(12.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(row.icon, null, tint = row.tint, modifier = Modifier.size(20.dp))
        }
        Spacer(Modifier.size(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(row.title, style = MaterialTheme.typography.titleSmall)
            Text(
                row.subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (row.onClick != null) {
            IconButton(onClick = row.onClick) {
                Icon(Icons.Default.ChevronRight, contentDescription = "打开")
            }
        }
    }
}

@Composable
private fun ConnectionModeCard(
    activeMode: ServerEndpointMode,
    cloudUrl: String,
    localUrl: String,
    status: ServerConnectionStatus,
    onUseCloud: () -> Unit,
    onUseLocal: () -> Unit,
) {
    val usingCloud = activeMode == ServerEndpointMode.Cloud
    val accent = if (usingCloud) Primary else Secondary
    val activeUrl = if (usingCloud) cloudUrl else localUrl

    Card(
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(42.dp)
                        .background(accent.copy(alpha = 0.12f), RoundedCornerShape(14.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        if (usingCloud) Icons.Default.Cloud else Icons.Default.Router,
                        contentDescription = null,
                        tint = accent,
                    )
                }
                Spacer(Modifier.size(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        if (usingCloud) "当前使用云端服务" else "当前使用电脑备用服务",
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        if (status == ServerConnectionStatus.Connecting) "正在切换连接…" else activeUrl,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
                Box(
                    modifier = Modifier
                        .background(accent.copy(alpha = 0.12f), RoundedCornerShape(50.dp))
                        .padding(horizontal = 10.dp, vertical = 5.dp),
                ) {
                    Text(
                        if (usingCloud) "云端" else "Plan B",
                        style = MaterialTheme.typography.labelMedium,
                        color = accent,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (usingCloud) {
                    Button(
                        onClick = onUseCloud,
                        modifier = Modifier.weight(1f),
                        enabled = false,
                    ) { Text("正在使用云端") }
                    OutlinedButton(
                        onClick = onUseLocal,
                        modifier = Modifier.weight(1f),
                    ) { Text("使用电脑备用") }
                } else {
                    OutlinedButton(
                        onClick = onUseCloud,
                        modifier = Modifier.weight(1f),
                    ) { Text("使用云端") }
                    Button(
                        onClick = onUseLocal,
                        modifier = Modifier.weight(1f),
                        enabled = false,
                    ) { Text("正在使用备用") }
                }
            }

            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(
                        MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f),
                        RoundedCornerShape(16.dp),
                    )
                    .padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text(
                    "手动切换 · 不会自动接管",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    if (usingCloud) {
                        "云端连接强制使用 HTTPS。需要备用时，请确认电脑端服务已经准备好，再手动切换。"
                    } else {
                        "使用前请确认电脑服务已启动，手机、电脑和 P4 在同一局域网，并将 P4 也指向相同服务地址。"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    "App 切换只改变手机连接，不会修改 P4 网关的服务器配置。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontWeight = FontWeight.Medium,
                )
            }
        }
    }
}

@Composable
private fun AdvancedConnectionCard(
    expanded: Boolean,
    onExpandedChange: () -> Unit,
    baseUrl: String,
    username: String,
    password: String,
    passwordVisible: Boolean,
    hasSavedPassword: Boolean,
    activeMode: ServerEndpointMode,
    status: ServerConnectionStatus,
    onBaseUrlChange: (String) -> Unit,
    onUsernameChange: (String) -> Unit,
    onPasswordChange: (String) -> Unit,
    onPasswordVisibilityChange: () -> Unit,
    onReconnect: () -> Unit,
    onSave: () -> Unit,
) {
    val usingCloud = activeMode == ServerEndpointMode.Cloud
    val urlValid = endpointUrlValid(activeMode, baseUrl)
    val credentialsValid = !usingCloud || (
        username.isNotBlank() && (hasSavedPassword || password.isNotBlank())
    )
    val saveEnabled = urlValid && credentialsValid

    Card(
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(1.dp),
    ) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(38.dp)
                        .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(12.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        if (usingCloud) Icons.Default.Cloud else Icons.Default.Router,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                    )
                }
                Spacer(Modifier.size(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        if (usingCloud) "云端连接资料" else "电脑备用连接资料",
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Text(
                        if (usingCloud) "编辑云端地址与登录凭据" else "编辑当前电脑服务地址与可选凭据",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                IconButton(onClick = onExpandedChange) {
                    Icon(
                        if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                        contentDescription = if (expanded) "收起" else "展开",
                    )
                }
            }

            if (expanded) {
                HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.55f))
                Text(
                    if (usingCloud) {
                        "保存后会重建手机端遥测和媒体连接。为保护凭据，云端地址必须使用 HTTPS。"
                    } else {
                        "默认地址为 http://192.168.137.1:8088，HTTP 仅允许私网或本机 IPv4。账号和密码可留空；请先在电脑上启动服务。"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = baseUrl,
                    onValueChange = onBaseUrlChange,
                    label = { Text(if (usingCloud) "云端服务地址" else "电脑服务地址") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    isError = baseUrl.isNotBlank() && !urlValid,
                    supportingText = if (baseUrl.isNotBlank() && !urlValid) {
                        {
                            Text(
                                if (usingCloud) {
                                    "请输入完整的 HTTPS 地址"
                                } else {
                                    "请输入 HTTPS 地址；HTTP 仅允许私网或本机 IPv4 地址"
                                }
                            )
                        }
                    } else {
                        null
                    },
                )
                OutlinedTextField(
                    value = username,
                    onValueChange = onUsernameChange,
                    label = {
                        Text(if (usingCloud) "云端账户" else "电脑服务账户（可选）")
                    },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    isError = usingCloud && username.isBlank(),
                )
                OutlinedTextField(
                    value = password,
                    onValueChange = onPasswordChange,
                    label = {
                        Text(
                            when {
                                hasSavedPassword -> "密码（留空则保持已保存密码）"
                                usingCloud -> "云端密码"
                                else -> "电脑服务密码（可选）"
                            }
                        )
                    },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    isError = usingCloud && !hasSavedPassword && password.isBlank(),
                    trailingIcon = {
                        IconButton(onClick = onPasswordVisibilityChange) {
                            Icon(
                                if (passwordVisible) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                                contentDescription = if (passwordVisible) "隐藏密码" else "显示密码",
                            )
                        }
                    },
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp, Alignment.End),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(onClick = onReconnect) { Text("重新连接") }
                    Button(onClick = onSave, enabled = saveEnabled) {
                        Text(if (status == ServerConnectionStatus.Connecting) "保存并重连" else "保存并连接")
                    }
                }
            }
        }
    }
}

private fun endpointUrlValid(mode: ServerEndpointMode, value: String): Boolean {
    return isAllowedServerUrl(value, mode)
}

private fun networkLabel(transport: NetworkTransport, metered: Boolean): String {
    val type = when (transport) {
        NetworkTransport.Wifi -> "Wi-Fi"
        NetworkTransport.Cellular -> "蜂窝网络"
        NetworkTransport.Ethernet -> "有线网络"
        NetworkTransport.None -> "未连接"
        NetworkTransport.Other -> "网络已连接"
    }
    return if (metered && transport != NetworkTransport.None) "$type · 计费网络" else type
}

private fun openNotificationSettings(context: Context) {
    val intent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
            .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
    } else {
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
            .setData(Uri.parse("package:${context.packageName}"))
    }
    runCatching { context.startActivity(intent) }
}
