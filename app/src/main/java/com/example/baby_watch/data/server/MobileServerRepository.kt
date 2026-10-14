package com.example.baby_watch.data.server

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.SystemClock
import com.example.baby_watch.notification.AlertDispatcher
import com.example.baby_watch.service.log.LogManager
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import okhttp3.Call
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject
import java.io.IOException
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong

object MobileServerRepository {
    private data class MatrixPayload(
        val endpointGeneration: Long,
        val json: String,
    )

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val transport = ServerHttpTransport()
    private val mediaController = ServerMediaController(transport, scope)

    private val _state = MutableStateFlow(MonitorRepositoryState())
    val state: StateFlow<MonitorRepositoryState> = _state.asStateFlow()

    private val _events = MutableStateFlow<List<SafetyEvent>>(emptyList())
    val events: StateFlow<List<SafetyEvent>> = _events.asStateFlow()

    private val _lastCommand = MutableStateFlow<MobileCommandStatus?>(null)
    val lastCommand: StateFlow<MobileCommandStatus?> = _lastCommand.asStateFlow()

    private val _thermalMatrix = MutableStateFlow<MatrixFrame?>(null)
    val thermalMatrix: StateFlow<MatrixFrame?> = _thermalMatrix.asStateFlow()

    private val _pressureMatrix = MutableStateFlow<MatrixFrame?>(null)
    val pressureMatrix: StateFlow<MatrixFrame?> = _pressureMatrix.asStateFlow()

    private val _stories = MutableStateFlow<List<AiStory>>(emptyList())
    val stories: StateFlow<List<AiStory>> = _stories.asStateFlow()

    private val _advisories = MutableStateFlow<List<AiAdvisory>>(emptyList())
    val advisories: StateFlow<List<AiAdvisory>> = _advisories.asStateFlow()

    private val _history = MutableStateFlow<List<TrendSample>>(emptyList())
    val history: StateFlow<List<TrendSample>> = _history.asStateFlow()

    private val networkHealth = MutableStateFlow(NetworkHealth())
    private val pressureMatrixPayloadQueue = Channel<MatrixPayload>(
        capacity = 1,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val thermalMatrixPayloadQueue = Channel<MatrixPayload>(
        capacity = 1,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val notificationPolicy = SafetyEventNotificationPolicy()
    private val fusionEpochLock = Any()
    private var lastFusionEvidenceSequence: Long? = null

    val videoState: StateFlow<VideoStreamState> = mediaController.videoState
    val audioState: StateFlow<AudioStreamState> = mediaController.audioState

    private val initializationLock = Any()
    private var appContext: Context? = null
    private var credentialStore: ServerCredentialStore? = null
    private var historyStore: MonitorHistoryStore? = null
    private var connectivityObserver: AndroidConnectivityObserver? = null
    private var credentials: ServerCredentials? = null
    private var monitorJob: Job? = null
    private var sseCall: Call? = null
    private var endpointOperationJob: Job = SupervisorJob()
    private val endpointGeneration = AtomicLong(0L)

    init {
        // Each stream owns a latest-frame queue. A pressure burst cannot evict the newest thermal
        // frame (or vice versa), and neither decoder can block the SSE reader carrying alarms.
        consumeMatrixPayloads(pressureMatrixPayloadQueue)
        consumeMatrixPayloads(thermalMatrixPayloadQueue)
    }

    fun start(context: Context) {
        ensureInitialized(context)
        connectivityObserver?.start(::handleNetworkChanged)
        if (monitorJob?.isActive != true) reconnect()
    }

    fun stop() {
        endpointGeneration.incrementAndGet()
        monitorJob?.cancel()
        monitorJob = null
        sseCall?.cancel()
        sseCall = null
        endpointOperationJob.cancel()
        endpointOperationJob = SupervisorJob()
        mediaController.stop()
        connectivityObserver?.stop()
        _state.update {
            it.copy(
                connectionStatus = if (credentials?.isComplete == true) {
                    ServerConnectionStatus.Error
                } else {
                    ServerConnectionStatus.NotConfigured
                },
                errorMessage = if (credentials?.isComplete == true) "后台监护服务已停止" else null,
            )
        }
    }

    fun ensureInitialized(context: Context) {
        if (appContext != null) return
        synchronized(initializationLock) {
            if (appContext != null) return
            appContext = context.applicationContext
            credentialStore = ServerCredentialStore(context.applicationContext)
            historyStore = MonitorHistoryStore(context.applicationContext)
            connectivityObserver = AndroidConnectivityObserver(context.applicationContext)
            credentials = credentialStore?.load()
            _history.value = credentials
                ?.let { historyStore?.load(eventNamespace(it)) }
                .orEmpty()
            val view = credentialStore?.view() ?: ServerConfigurationView()
            val currentNetwork = connectivityObserver?.current() ?: NetworkHealth()
            networkHealth.value = currentNetwork
            _state.update {
                it.copy(
                    configuration = view,
                    network = currentNetwork,
                    connectionStatus = if (credentials?.isComplete == true) {
                        ServerConnectionStatus.Connecting
                    } else {
                        ServerConnectionStatus.NotConfigured
                    },
                )
            }
        }
    }

    fun saveConfiguration(
        context: Context,
        baseUrl: String,
        username: String,
        password: String,
    ) {
        ensureInitialized(context)
        saveConfiguration(
            context = context,
            mode = credentialStore?.activeMode() ?: ServerEndpointMode.Cloud,
            baseUrl = baseUrl,
            username = username,
            password = password,
        )
    }

    fun saveConfiguration(
        context: Context,
        mode: ServerEndpointMode,
        baseUrl: String,
        username: String,
        password: String,
    ) {
        ensureInitialized(context)
        val store = credentialStore ?: return
        val normalizedUrl = normalizeServerUrl(baseUrl)
        val existingPassword = store.load(mode).password
        val effectivePassword = password.ifBlank { existingPassword }
        val validationError = when {
            !isAllowedServerUrl(normalizedUrl, mode) -> if (mode == ServerEndpointMode.Local) {
                "电脑备用地址须使用局域网私有 IPv4，例如 http://192.168.137.1:8088"
            } else {
                "云端服务地址必须使用 HTTPS"
            }
            mode == ServerEndpointMode.Cloud && username.isBlank() -> "请输入云端用户名"
            mode == ServerEndpointMode.Cloud && effectivePassword.isBlank() -> "请输入云端密码"
            else -> null
        }
        if (validationError != null) {
            _state.update {
                it.copy(
                    connectionStatus = ServerConnectionStatus.Error,
                    errorMessage = validationError,
                )
            }
            return
        }

        store.save(
            mode = mode,
            baseUrl = normalizedUrl,
            username = username,
            newPassword = password.takeIf { it.isNotBlank() },
        )
        if (store.activeMode() != mode) {
            _state.update { it.copy(configuration = store.view(), errorMessage = null) }
            return
        }

        cancelEndpointConnections()
        credentials = store.load(mode)
        clearEndpointState(store.view())
        reconnect()
    }

    /** Selects a stored endpoint explicitly; commands are never carried or replayed across it. */
    fun switchEndpoint(context: Context, mode: ServerEndpointMode) {
        ensureInitialized(context)
        val store = credentialStore ?: return
        cancelEndpointConnections()
        store.setActiveMode(mode)
        credentials = store.load(mode)
        clearEndpointState(store.view())
        reconnect()
    }

    private fun cancelEndpointConnections() {
        endpointGeneration.incrementAndGet()
        monitorJob?.cancel()
        monitorJob = null
        sseCall?.cancel()
        sseCall = null
        // Also cancel acknowledgements, story requests and commands that were created for the old
        // server. They are not restarted on the new endpoint.
        endpointOperationJob.cancel()
        endpointOperationJob = SupervisorJob()
        // Close old sockets before changing credentials. Wanted flags are retained so selected
        // streams can be rebuilt against the newly active endpoint.
        mediaController.onNetworkChanged(false)
    }

    private fun launchEndpointOperation(block: suspend CoroutineScope.() -> Unit): Job =
        scope.launch(context = endpointOperationJob, block = block)

    private fun clearEndpointState(configuration: ServerConfigurationView) {
        while (pressureMatrixPayloadQueue.tryReceive().isSuccess) Unit
        while (thermalMatrixPayloadQueue.tryReceive().isSuccess) Unit
        _events.value = emptyList()
        _lastCommand.value = null
        _thermalMatrix.value = null
        _pressureMatrix.value = null
        _stories.value = emptyList()
        _advisories.value = emptyList()
        // Histories are partitioned by origin, so selecting Plan B can never expose cloud samples
        // (or vice versa).
        _history.value = credentials
            ?.let { historyStore?.load(eventNamespace(it)) }
            .orEmpty()
        notificationPolicy.reset()
        synchronized(fusionEpochLock) { lastFusionEvidenceSequence = null }

        val freshNetwork = networkHealth.value.copy(
            reconnectAttempt = 0,
            retryInSeconds = 0,
            lastConnectedAtMs = null,
            lastMessageAtMs = null,
            lastTelemetryAtMs = null,
        )
        networkHealth.value = freshNetwork
        _state.value = MonitorRepositoryState(
            configuration = configuration,
            connectionStatus = if (credentials?.isComplete == true) {
                ServerConnectionStatus.Connecting
            } else {
                ServerConnectionStatus.NotConfigured
            },
            network = freshNetwork,
        )
    }

    fun reconnect() {
        val activeCredentials = credentials ?: return
        val generation = endpointGeneration.incrementAndGet()
        endpointOperationJob.cancel()
        endpointOperationJob = SupervisorJob()
        monitorJob?.cancel()
        monitorJob = null
        sseCall?.cancel()
        sseCall = null
        mediaController.onNetworkChanged(false)
        mediaController.resetEndpoint(activeCredentials)
        if (!activeCredentials.isComplete) {
            _state.update {
                it.copy(
                    connectionStatus = ServerConnectionStatus.NotConfigured,
                    errorMessage = null,
                )
            }
            return
        }

        mediaController.onNetworkChanged(
            isEndpointNetworkReady(activeCredentials, networkHealth.value)
        )

        _state.update {
            it.copy(
                connectionStatus = ServerConnectionStatus.Connecting,
                network = networkHealth.value.copy(reconnectAttempt = 0, retryInSeconds = 0),
                errorMessage = null,
            )
        }
        monitorJob = scope.launch {
            monitorLoop(activeCredentials, generation)
        }
    }

    fun startVideo() {
        credentials?.takeIf { it.isComplete }?.let(mediaController::startVideo)
    }

    fun stopVideo() {
        mediaController.stopVideo()
    }

    fun startAudio() {
        credentials?.takeIf { it.isComplete }?.let(mediaController::startAudio)
    }

    fun stopAudio() {
        mediaController.stopAudio()
    }

    fun stopMedia() {
        mediaController.stop()
    }

    fun setAudioMuted(muted: Boolean) {
        mediaController.setMuted(muted)
    }

    fun acknowledgeEvent(eventId: Long) {
        val activeCredentials = credentials ?: return
        val operationGeneration = endpointGeneration.get()
        launchEndpointOperation {
            runCatching {
                executePostJson(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/events/$eventId/ack",
                    "{}",
                    retryable = true,
                )
            }.onSuccess {
                if (!isActiveEndpoint(activeCredentials, operationGeneration)) return@onSuccess
                _events.update { events ->
                    events.map { event ->
                        if (event.id == eventId) event.copy(acknowledged = true) else event
                    }
                }
                LogManager.notification("告警已确认", "事件 #$eventId")
            }.onFailure { error ->
                if (error is CancellationException ||
                    !isActiveEndpoint(activeCredentials, operationGeneration)
                ) return@onFailure
                LogManager.system("确认告警失败", userMessage(error))
                _state.update { it.copy(errorMessage = userMessage(error)) }
            }
        }
    }

    fun createStory(
        theme: String,
        childAgeMonths: Int = 18,
        durationSeconds: Int = 60,
        style: String = "calm",
        autoplay: Boolean = true,
    ) {
        val activeCredentials = credentials ?: return
        val operationGeneration = endpointGeneration.get()
        if (!_state.value.capabilities.aiEnabled || theme.isBlank()) return
        launchEndpointOperation {
            val body = JSONObject()
                .put("theme", theme.trim())
                .put("child_age_months", childAgeMonths.coerceIn(0, 72))
                .put("duration_seconds", durationSeconds.coerceIn(30, 240))
                .put("language", "zh-CN")
                .put("style", style.takeIf { it in setOf("calm", "gentle", "playful") } ?: "calm")
                .put("autoplay", autoplay)
                .put("request_id", "phone-story-${UUID.randomUUID()}")
                .toString()
            runCatching {
                MobileProtocolParser.parseStory(
                    executePostJson(
                        activeCredentials,
                        "/api/mobile/v1/devices/${activeCredentials.deviceId}/stories",
                        body,
                        retryable = true,
                    )
                )
            }.onSuccess { story ->
                if (isActiveEndpoint(activeCredentials, operationGeneration)) upsertStory(story)
            }.onFailure { error ->
                if (error !is CancellationException &&
                    isActiveEndpoint(activeCredentials, operationGeneration)
                ) {
                    _state.update { state -> state.copy(errorMessage = userMessage(error)) }
                }
            }
        }
    }

    fun replayStory(storyId: String) {
        val activeCredentials = credentials ?: return
        val operationGeneration = endpointGeneration.get()
        if (storyId.isBlank()) return
        launchEndpointOperation {
            runCatching {
                MobileProtocolParser.parseStory(
                    executePostJson(
                        activeCredentials,
                        "/api/mobile/v1/devices/${activeCredentials.deviceId}/stories/$storyId/replay",
                        "{}",
                        retryable = false,
                    )
                )
            }.onSuccess { story ->
                if (isActiveEndpoint(activeCredentials, operationGeneration)) upsertStory(story)
            }.onFailure { error ->
                if (error !is CancellationException &&
                    isActiveEndpoint(activeCredentials, operationGeneration)
                ) {
                    _state.update { state -> state.copy(errorMessage = userMessage(error)) }
                }
            }
        }
    }

    fun acknowledgeAdvisory(advisoryId: String) {
        val activeCredentials = credentials ?: return
        val operationGeneration = endpointGeneration.get()
        if (advisoryId.isBlank()) return
        launchEndpointOperation {
            runCatching {
                executePostJson(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/advisories/$advisoryId/ack",
                    "{}",
                    retryable = true,
                )
            }.onSuccess {
                if (isActiveEndpoint(activeCredentials, operationGeneration)) {
                    markAdvisoryAcknowledged(advisoryId)
                }
            }.onFailure { error ->
                if (error !is CancellationException &&
                    isActiveEndpoint(activeCredentials, operationGeneration)
                ) {
                    _state.update { state -> state.copy(errorMessage = userMessage(error)) }
                }
            }
        }
    }

    fun sendCommand(action: String, args: Map<String, Any> = emptyMap()) {
        val activeCredentials = credentials ?: return
        val operationGeneration = endpointGeneration.get()
        launchEndpointOperation {
            val body = JSONObject()
                .put("action", action)
                .put("args", JSONObject(args))
                .put("request_id", UUID.randomUUID().toString())
                .toString()
            runCatching {
                executePostJson(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/commands",
                    body,
                    retryable = true,
                )
            }.mapCatching(MobileProtocolParser::parseCommand)
                .onSuccess { command ->
                    if (!isActiveEndpoint(activeCredentials, operationGeneration)) return@onSuccess
                    _lastCommand.value = command
                    LogManager.notification(
                        "命令已提交",
                        "${command.action} · ${command.status}",
                    )
                }
                .onFailure { error ->
                    if (error is CancellationException ||
                        !isActiveEndpoint(activeCredentials, operationGeneration)
                    ) return@onFailure
                    LogManager.system("命令发送失败", userMessage(error))
                    _state.update { it.copy(errorMessage = userMessage(error)) }
                }
        }
    }

    private suspend fun monitorLoop(
        activeCredentials: ServerCredentials,
        generation: Long,
    ) {
        var reconnectAttempt = 0
        while (currentCoroutineContext().isActive && isActiveEndpoint(activeCredentials, generation)) {
            var streamStartedAt = 0L
            if (!isEndpointNetworkReady(activeCredentials, networkHealth.value)) {
                _state.update {
                    it.copy(
                        connectionStatus = ServerConnectionStatus.Error,
                        usingCachedData = true,
                        errorMessage = "网络已断开，监护数据将在联网后自动恢复",
                    )
                }
                networkHealth.filter { isEndpointNetworkReady(activeCredentials, it) }.first()
                if (!isActiveEndpoint(activeCredentials, generation)) return
            }
            try {
                bootstrap(activeCredentials, generation)
                if (!isActiveEndpoint(activeCredentials, generation)) return
                streamStartedAt = SystemClock.elapsedRealtime()
                consumeSse(activeCredentials, generation)
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                currentCoroutineContext().ensureActive()
                if (!isActiveEndpoint(activeCredentials, generation)) return
                val message = userMessage(error)
                _state.update {
                    it.copy(
                        connectionStatus = ServerConnectionStatus.Error,
                        usingCachedData = true,
                        errorMessage = message,
                    )
                }
                LogManager.system("服务端连接中断", message)
            }
            if (streamStartedAt > 0L &&
                SystemClock.elapsedRealtime() - streamStartedAt >= STABLE_STREAM_RESET_MS
            ) {
                reconnectAttempt = 0
            }
            reconnectAttempt++
            val retryMs = calculateBackoffMillis(reconnectAttempt)
            val retrySeconds = ((retryMs + 999L) / 1000L).toInt()
            updateReconnectState(generation, reconnectAttempt, retrySeconds)
            delay(retryMs)
            if (!isActiveEndpoint(activeCredentials, generation)) return
            _state.update {
                it.copy(
                    connectionStatus = ServerConnectionStatus.Connecting,
                    network = it.network.copy(retryInSeconds = 0),
                    errorMessage = "正在重新连接服务端…",
                )
            }
        }
    }

    private suspend fun bootstrap(
        activeCredentials: ServerCredentials,
        generation: Long,
    ) = coroutineScope {
        // Independent startup calls run together so reconnecting on a weak network costs RTTs,
        // not a long serial chain of REST requests.
        val capabilitiesDeferred = async {
            MobileProtocolParser.parseCapabilities(
                executeGet(activeCredentials, "/api/mobile/v1/capabilities")
            )
        }
        val devicesDeferred = async {
            executeGet(activeCredentials, "/api/mobile/v1/devices")
        }
        val capabilities = capabilitiesDeferred.await()
        val devicesJson = devicesDeferred.await()
        if (!isActiveEndpoint(activeCredentials, generation)) return@coroutineScope
        check(MobileProtocolParser.containsDevice(devicesJson, activeCredentials.deviceId)) {
            "服务端未找到设备 ${activeCredentials.deviceId}"
        }

        val snapshotDeferred = async {
            MobileProtocolParser.parseSnapshot(
                executeGet(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/snapshot",
                )
            )
        }
        val matrixDeferred = listOf("thermal", "pressure").associateWith { kind ->
            async {
                runCatching {
                    MobileProtocolParser.parseMatrix(
                        executeGet(
                            activeCredentials,
                            "/api/mobile/v1/devices/${activeCredentials.deviceId}/matrices/$kind",
                        )
                    )
                }
            }
        }
        val eventsDeferred = async {
            MobileProtocolParser.parseEvents(
                executeGet(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/events?limit=100",
                )
            ).sortedByDescending { it.id }
        }
        val aiDeferred = if (capabilities.aiEnabled) {
            async { refreshAi(activeCredentials, generation) }
        } else {
            if (isActiveEndpoint(activeCredentials, generation)) {
                _stories.value = emptyList()
                _advisories.value = emptyList()
            }
            null
        }

        val snapshot = snapshotDeferred.await()
        if (!isActiveEndpoint(activeCredentials, generation)) return@coroutineScope
        observeFusionSnapshot(snapshot.fusion)
        mediaController.onStoryTransportChanged(
            videoSuspended = snapshot.storyTransfer.videoTransportSuspended,
            audioSuspended = snapshot.storyTransfer.audioTransportSuspended,
        )
        val bootstrapNow = System.currentTimeMillis()
        _state.update {
            it.copy(
                connectionStatus = ServerConnectionStatus.Connected,
                capabilities = capabilities,
                snapshot = snapshot,
                network = it.network.copy(
                    reconnectAttempt = 0,
                    retryInSeconds = 0,
                    lastConnectedAtMs = bootstrapNow,
                    lastMessageAtMs = bootstrapNow,
                    lastTelemetryAtMs = bootstrapNow,
                ),
                usingCachedData = false,
                errorMessage = null,
            )
        }
        networkHealth.update {
            it.copy(
                reconnectAttempt = 0,
                retryInSeconds = 0,
                lastConnectedAtMs = bootstrapNow,
                lastMessageAtMs = bootstrapNow,
                lastTelemetryAtMs = bootstrapNow,
            )
        }

        matrixDeferred.values.forEach { deferred ->
            deferred.await().onSuccess {
                if (isActiveEndpoint(activeCredentials, generation)) applyMatrix(it, force = true)
            }
        }

        val serverEvents = eventsDeferred.await()
        if (!isActiveEndpoint(activeCredentials, generation)) return@coroutineScope
        _events.value = mergeSafetyEvents(serverEvents)
        handleFetchedEvents(activeCredentials, generation, serverEvents)
        recordSnapshot(activeCredentials, generation, snapshot, bootstrapNow)
        aiDeferred?.await()
        if (isActiveEndpoint(activeCredentials, generation)) {
            LogManager.system("服务端已连接", activeCredentials.deviceId)
        }
    }

    private fun handleFetchedEvents(
        activeCredentials: ServerCredentials,
        generation: Long,
        serverEvents: List<SafetyEvent>,
    ) {
        if (!isActiveEndpoint(activeCredentials, generation)) return
        val store = credentialStore ?: return
        val namespace = eventNamespace(activeCredentials)
        val maximumId = serverEvents.maxOfOrNull { it.id } ?: 0L
        val lastHandled = store.lastHandledEventId(namespace)
        if (lastHandled == null) {
            store.setLastHandledEventId(namespace, maximumId)
            return
        }
        serverEvents
            .asSequence()
            .filter { it.id > lastHandled }
            .sortedBy { it.id }
            .forEach { handleNewEvent(activeCredentials, generation, it) }
        if (maximumId > lastHandled && isActiveEndpoint(activeCredentials, generation)) {
            store.setLastHandledEventId(namespace, maximumId)
        }
    }

    private fun handleNewEvent(
        activeCredentials: ServerCredentials,
        generation: Long,
        event: SafetyEvent,
    ) {
        if (!isActiveEndpoint(activeCredentials, generation)) return
        if (notificationPolicy.shouldNotify(event)) {
            appContext?.let { AlertDispatcher.dispatch(it, event) }
        }
        if (isActiveEndpoint(activeCredentials, generation)) {
            credentialStore?.setLastHandledEventId(eventNamespace(activeCredentials), event.id)
        }
    }

    private fun observeFusionSnapshot(fusion: FusionSummary) {
        val baselines = fusion.activeIncidents.map { incident ->
            SafetyEventNotificationPolicy.IncidentLevelBaseline(
                incidentId = incident.incidentId,
                level = incident.level,
            )
        }
        synchronized(fusionEpochLock) {
            val sequence = fusion.evidenceSequence.takeIf { fusion.schemaVersion >= 5 && it > 0L }
            val previous = lastFusionEvidenceSequence
            if (sequence != null && previous != null && isSequenceRollback(previous, sequence)) {
                notificationPolicy.reset()
                LogManager.system("设备监护周期已更新", "Fusion 序列已重新开始，告警去重状态已清理")
            }
            if (sequence != null) lastFusionEvidenceSequence = sequence
            notificationPolicy.observeFusion(baselines)
        }
    }

    private suspend fun consumeSse(
        activeCredentials: ServerCredentials,
        generation: Long,
    ) {
        val request = transport.streamRequest(
            activeCredentials,
            "/api/mobile/v1/devices/${activeCredentials.deviceId}/stream",
        )
        val call = transport.streamClient.newCall(request)
        if (!isActiveEndpoint(activeCredentials, generation)) {
            call.cancel()
            return
        }
        sseCall = call
        val cancellationHandle = currentCoroutineContext()[Job]?.invokeOnCompletion {
            call.cancel()
        }
        try {
            call.execute().use { response ->
                if (!response.isSuccessful) {
                    throw ServerHttpException(response.code, "SSE 服务返回 HTTP ${response.code}")
                }
                val source = response.body?.source() ?: throw IOException("服务端没有返回 SSE 数据")
                var eventName = "message"
                val data = StringBuilder()
                while (isActiveEndpoint(activeCredentials, generation) &&
                    currentCoroutineContext().isActive &&
                    !source.exhausted()
                ) {
                    val line = source.readUtf8Line() ?: break
                    when {
                        line.startsWith(":") -> markStreamMessage(generation)
                        line.startsWith("event:") -> eventName = line.substringAfter(':').trim()
                        line.startsWith("data:") -> {
                            if (data.isNotEmpty()) data.append('\n')
                            data.append(line.substringAfter(':').trimStart())
                            if (data.length > MAX_SSE_EVENT_CHARS) {
                                data.clear()
                                eventName = "message"
                                LogManager.system("忽略异常数据", "SSE 单条消息超过安全上限")
                            }
                        }
                        line.isBlank() -> {
                            if (data.isNotEmpty()) {
                                try {
                                    processSseEvent(
                                        activeCredentials,
                                        generation,
                                        eventName,
                                        data.toString(),
                                    )
                                    // Snapshot processing stamps message + telemetry time in the
                                    // same StateFlow transaction as its payload.
                                    when (eventName) {
                                        "snapshot" -> Unit
                                        // Matrix frames have their own StateFlows. Publishing a
                                        // global repository update for every 5 Hz pressure / 3 Hz
                                        // thermal frame would recompose unrelated screens.
                                        "matrix" -> markStreamMessage(
                                            generation,
                                            publishToUi = false,
                                        )
                                        else -> markStreamMessage(generation)
                                    }
                                } catch (error: Exception) {
                                    if (error is CancellationException) throw error
                                    if (isActiveEndpoint(activeCredentials, generation)) {
                                        LogManager.system(
                                            "忽略损坏的实时数据",
                                            "${eventName}: ${userMessage(error)}",
                                        )
                                    }
                                }
                            }
                            eventName = "message"
                            data.clear()
                        }
                    }
                }
                currentCoroutineContext().ensureActive()
                if (!isActiveEndpoint(activeCredentials, generation)) return
                throw IOException("SSE 连接已关闭")
            }
        } finally {
            cancellationHandle?.dispose()
            if (sseCall === call) sseCall = null
        }
    }

    private suspend fun processSseEvent(
        activeCredentials: ServerCredentials,
        generation: Long,
        eventName: String,
        json: String,
    ) {
        if (!isActiveEndpoint(activeCredentials, generation)) return
        when (eventName) {
            "snapshot" -> {
                val next = MobileProtocolParser.parseSnapshot(json)
                if (!isActiveEndpoint(activeCredentials, generation)) return
                observeFusionSnapshot(next.fusion)
                mediaController.onStoryTransportChanged(
                    videoSuspended = next.storyTransfer.videoTransportSuspended,
                    audioSuspended = next.storyTransfer.audioTransportSuspended,
                )
                val now = System.currentTimeMillis()
                networkHealth.update {
                    it.copy(lastMessageAtMs = now, lastTelemetryAtMs = now)
                }
                _state.update {
                    it.copy(
                        connectionStatus = ServerConnectionStatus.Connected,
                        snapshot = next.copy(
                            pressureActivePoints = next.pressureActivePoints
                                ?: it.snapshot.pressureActivePoints,
                        ),
                        network = it.network.copy(
                            lastMessageAtMs = now,
                            lastTelemetryAtMs = now,
                        ),
                        usingCachedData = false,
                        errorMessage = null,
                    )
                }
                recordSnapshot(activeCredentials, generation, next, now)
            }

            "matrix" -> {
                val payload = MatrixPayload(generation, json)
                when (peekMatrixKind(json)?.lowercase()) {
                    "pressure", "pressure_matrix" -> pressureMatrixPayloadQueue.trySend(payload)
                    "thermal", "thermal_matrix", "thermal_camera", "thermal_image" ->
                        thermalMatrixPayloadQueue.trySend(payload)
                    else -> LogManager.system("忽略未知矩阵", "SSE matrix 缺少可识别的 kind")
                }
            }

            "event" -> {
                val parsed = runCatching { MobileProtocolParser.parseEvent(json) }.getOrNull()
                if (!isActiveEndpoint(activeCredentials, generation)) return
                if (parsed == null || parsed.id <= 0L) {
                    refreshEvents(activeCredentials, generation)
                } else {
                    _events.update { current ->
                        mergeSafetyEvents(listOf(parsed) + current)
                    }
                    val lastHandled = credentialStore
                        ?.lastHandledEventId(eventNamespace(activeCredentials)) ?: 0L
                    if (parsed.id > lastHandled) {
                        handleNewEvent(activeCredentials, generation, parsed)
                    }
                }
            }

            "event_ack" -> {
                MobileProtocolParser.parseEventAckId(json)?.let { eventId ->
                    if (!isActiveEndpoint(activeCredentials, generation)) return
                    _events.update { current ->
                        current.map { event ->
                            if (event.id == eventId) event.copy(acknowledged = true) else event
                        }
                    }
                }
            }

            "command" -> runCatching {
                MobileProtocolParser.parseCommand(json)
            }.onSuccess {
                if (isActiveEndpoint(activeCredentials, generation)) _lastCommand.value = it
            }

            "ai_story" -> runCatching {
                MobileProtocolParser.parseStory(json)
            }.onSuccess {
                if (isActiveEndpoint(activeCredentials, generation)) upsertStory(it)
            }

            "ai_advisory" -> runCatching {
                MobileProtocolParser.parseAdvisory(json)
            }.onSuccess {
                if (isActiveEndpoint(activeCredentials, generation)) upsertAdvisory(it)
            }

            "ai_advisory_ack" -> {
                MobileProtocolParser.parseAdvisoryAckId(json)
                    ?.let {
                        if (isActiveEndpoint(activeCredentials, generation)) {
                            markAdvisoryAcknowledged(it)
                        }
                    }
            }

            "log" -> {
                val root = JSONObject(json).optJSONObject("data") ?: JSONObject(json)
                if (isActiveEndpoint(activeCredentials, generation)) {
                    LogManager.notification(
                        root.optString("component", "设备日志"),
                        root.optString("message"),
                    )
                }
            }
        }
    }

    private suspend fun refreshEvents(
        activeCredentials: ServerCredentials,
        generation: Long,
    ) {
        runCatching {
            MobileProtocolParser.parseEvents(
                executeGet(
                    activeCredentials,
                    "/api/mobile/v1/devices/${activeCredentials.deviceId}/events?limit=100",
                )
            )
        }.onSuccess { refreshed ->
            if (!isActiveEndpoint(activeCredentials, generation)) return@onSuccess
            val ordered = mergeSafetyEvents(refreshed)
            _events.value = ordered
            handleFetchedEvents(activeCredentials, generation, refreshed)
        }
    }

    private suspend fun refreshAi(
        activeCredentials: ServerCredentials,
        generation: Long,
    ) = coroutineScope {
        val storiesDeferred = async {
            runCatching {
                MobileProtocolParser.parseStories(
                    executeGet(
                        activeCredentials,
                        "/api/mobile/v1/devices/${activeCredentials.deviceId}/stories?limit=20",
                    )
                )
            }
        }
        val advisoriesDeferred = async {
            runCatching {
                MobileProtocolParser.parseAdvisories(
                    executeGet(
                        activeCredentials,
                        "/api/mobile/v1/devices/${activeCredentials.deviceId}/advisories?limit=50",
                    )
                )
            }
        }
        storiesDeferred.await().onSuccess {
            if (isActiveEndpoint(activeCredentials, generation)) _stories.value = it
        }
        advisoriesDeferred.await().onSuccess {
            if (isActiveEndpoint(activeCredentials, generation)) _advisories.value = it
        }
    }

    private fun upsertStory(story: AiStory) {
        if (story.storyId.isBlank()) return
        _stories.update { current ->
            (listOf(story) + current.filterNot { it.storyId == story.storyId })
                .sortedByDescending { it.updatedAt.ifBlank { it.createdAt } }
                .take(20)
        }
    }

    private fun upsertAdvisory(advisory: AiAdvisory) {
        if (advisory.advisoryId.isBlank()) return
        _advisories.update { current ->
            (listOf(advisory) + current.filterNot { it.advisoryId == advisory.advisoryId })
                .sortedByDescending(AiAdvisory::createdAt)
                .take(50)
        }
    }

    private fun markAdvisoryAcknowledged(advisoryId: String) {
        _advisories.update { current ->
            current.map { advisory ->
                if (advisory.advisoryId == advisoryId) {
                    advisory.copy(acknowledged = true)
                } else {
                    advisory
                }
            }
        }
    }

    private fun consumeMatrixPayloads(queue: Channel<MatrixPayload>) {
        scope.launch(Dispatchers.Default) {
            for (payload in queue) {
                if (endpointGeneration.get() != payload.endpointGeneration) continue
                runCatching { MobileProtocolParser.parseMatrix(payload.json) }
                    .onSuccess {
                        if (endpointGeneration.get() == payload.endpointGeneration) applyMatrix(it)
                    }
                    .onFailure {
                        if (endpointGeneration.get() == payload.endpointGeneration) {
                            LogManager.system("忽略损坏的矩阵数据", userMessage(it))
                        }
                    }
            }
        }
    }

    private fun applyMatrix(matrix: MatrixFrame, force: Boolean = false) {
        when (matrix.kind.lowercase()) {
            "pressure", "pressure_matrix" -> {
                val current = _pressureMatrix.value
                if (!force && current != null && !isNewerSequence(matrix.sequence, current.sequence)) return
                _pressureMatrix.value = matrix
                // Telemetry owns this summary whenever it is present. Matrix parsing only fills an
                // initial gap, so a 5 Hz pressure stream does not re-emit the entire repository.
                if (_state.value.snapshot.pressureActivePoints == null) {
                    val activePoints = matrix.activePoints
                    _state.update { currentState ->
                        if (currentState.snapshot.pressureActivePoints != null) {
                            currentState
                        } else {
                            currentState.copy(
                                snapshot = currentState.snapshot.copy(
                                    pressureActivePoints = activePoints,
                                )
                            )
                        }
                    }
                }
            }

            "thermal", "thermal_matrix", "thermal_camera", "thermal_image" -> {
                val current = _thermalMatrix.value
                if (!force && current != null && !isNewerSequence(matrix.sequence, current.sequence)) return
                _thermalMatrix.value = matrix
            }
        }
    }

    private suspend fun executeGet(credentials: ServerCredentials, path: String): String =
        transport.get(credentials, path)

    private suspend fun executePostJson(
        credentials: ServerCredentials,
        path: String,
        json: String,
        retryable: Boolean,
    ): String = transport.postJson(credentials, path, json, retryable)

    private fun recordSnapshot(
        activeCredentials: ServerCredentials,
        generation: Long,
        snapshot: MonitorSnapshot,
        now: Long = System.currentTimeMillis(),
    ) {
        if (!isActiveEndpoint(activeCredentials, generation)) return
        // Story delivery temporarily pauses RA8 polling. Gateway-only progress snapshots still
        // keep the connection alive, but their sensor payload is the previous sample and must not
        // be recorded as a fresh trend point.
        if (!snapshot.storyTransfer.active) {
            historyStore?.let { store ->
                _history.update {
                    store.append(eventNamespace(activeCredentials), it, snapshot, now)
                }
            }
        }
    }

    private fun markStreamMessage(
        generation: Long,
        publishToUi: Boolean = true,
    ) {
        if (endpointGeneration.get() != generation) return
        val now = System.currentTimeMillis()
        networkHealth.update { it.copy(lastMessageAtMs = now) }
        if (!publishToUi) return
        _state.update {
            it.copy(network = it.network.copy(lastMessageAtMs = now))
        }
    }

    private fun updateReconnectState(
        generation: Long,
        attempt: Int,
        retrySeconds: Int,
    ) {
        if (endpointGeneration.get() != generation) return
        networkHealth.update {
            it.copy(reconnectAttempt = attempt, retryInSeconds = retrySeconds)
        }
        _state.update {
            it.copy(
                network = it.network.copy(
                    reconnectAttempt = attempt,
                    retryInSeconds = retrySeconds,
                )
            )
        }
    }

    private fun handleNetworkChanged(health: NetworkHealth) {
        val previous = networkHealth.value
        val merged = health.copy(
            reconnectAttempt = previous.reconnectAttempt,
            retryInSeconds = previous.retryInSeconds,
            lastConnectedAtMs = previous.lastConnectedAtMs,
            lastMessageAtMs = previous.lastMessageAtMs,
            lastTelemetryAtMs = previous.lastTelemetryAtMs,
        )
        networkHealth.value = merged
        _state.update { it.copy(network = merged) }
        val ready = credentials?.let { isEndpointNetworkReady(it, merged) } == true
        mediaController.onNetworkChanged(ready)
        if (!ready) sseCall?.cancel()
    }

    private fun eventNamespace(activeCredentials: ServerCredentials): String =
        "${activeCredentials.baseUrl}|${activeCredentials.deviceId}"

    private fun isActiveEndpoint(
        candidateCredentials: ServerCredentials,
        generation: Long,
    ): Boolean = credentials == candidateCredentials && endpointGeneration.get() == generation

    private fun isNewerSequence(candidate: Long, current: Long): Boolean {
        if (candidate == current) return false
        val delta = (candidate - current) and 0xFFFF_FFFFL
        return delta in 1L..0x7FFF_FFFFL
    }

    private fun userMessage(error: Throwable): String {
        return error.message?.take(240).orEmpty().ifBlank { "未知网络错误" }
    }

    private const val STABLE_STREAM_RESET_MS = 30_000L
    private const val MAX_SSE_EVENT_CHARS = 1_000_000

}

/** Cloud requires Android's Internet validation; local Plan B only needs a live LAN link. */
internal fun isEndpointNetworkReady(
    credentials: ServerCredentials,
    health: NetworkHealth,
): Boolean = credentials.isComplete && when {
    credentials.isLocalNetworkEndpoint ->
        health.available && health.transport in setOf(
            NetworkTransport.Wifi,
            NetworkTransport.Ethernet,
        )
    else -> health.available && health.validated
}

internal fun isSequenceRollback(previous: Long, candidate: Long): Boolean {
    if (previous <= 0L || candidate <= 0L || candidate == previous) return false
    val delta = (candidate - previous) and 0xFFFF_FFFFL
    return delta !in 1L..0x7FFF_FFFFL
}

internal fun mergeSafetyEvents(
    events: Iterable<SafetyEvent>,
    limit: Int = 200,
): List<SafetyEvent> {
    val incidentIds = mutableSetOf<Long>()
    val legacyEventIds = mutableSetOf<Long>()
    return events
        .sortedByDescending(SafetyEvent::id)
        .filter { event ->
            val incidentId = event.incidentId?.takeIf { it > 0L }
            if (incidentId != null) incidentIds.add(incidentId) else legacyEventIds.add(event.id)
        }
        .take(limit.coerceAtLeast(0))
}

/** Reads only JSON string tokens up to `kind`; large matrix value arrays are never materialized. */
internal fun peekMatrixKind(json: String): String? {
    var index = 0
    while (index < json.length) {
        if (json[index] != '"') {
            index++
            continue
        }
        val token = readJsonString(json, index) ?: return null
        var cursor = token.nextIndex
        while (cursor < json.length && json[cursor].isWhitespace()) cursor++
        if (cursor >= json.length || json[cursor] != ':') {
            index = token.nextIndex
            continue
        }
        if (token.value != "kind") {
            index = token.nextIndex
            continue
        }
        cursor++
        while (cursor < json.length && json[cursor].isWhitespace()) cursor++
        if (cursor >= json.length || json[cursor] != '"') return null
        return readJsonString(json, cursor)?.value
    }
    return null
}

private data class JsonStringToken(
    val value: String,
    val nextIndex: Int,
)

private fun readJsonString(json: String, quoteIndex: Int): JsonStringToken? {
    if (quoteIndex !in json.indices || json[quoteIndex] != '"') return null
    val value = StringBuilder()
    var index = quoteIndex + 1
    while (index < json.length) {
        when (val char = json[index++]) {
            '"' -> return JsonStringToken(value.toString(), index)
            '\\' -> {
                if (index >= json.length) return null
                when (val escaped = json[index++]) {
                    '"', '\\', '/' -> value.append(escaped)
                    'b' -> value.append('\b')
                    'f' -> value.append('\u000c')
                    'n' -> value.append('\n')
                    'r' -> value.append('\r')
                    't' -> value.append('\t')
                    'u' -> {
                        if (index + 4 > json.length) return null
                        val codePoint = json.substring(index, index + 4).toIntOrNull(16) ?: return null
                        value.append(codePoint.toChar())
                        index += 4
                    }
                    else -> return null
                }
            }
            else -> value.append(char)
        }
    }
    return null
}

private class ServerMediaController(
    private val transport: ServerHttpTransport,
    private val scope: CoroutineScope,
) {
    private data class VideoPacket(
        val endpointGeneration: Long,
        val bytes: ByteString,
    )

    private data class AudioPacket(
        val endpointGeneration: Long,
        val bytes: ByteArray,
        val discontinuity: Boolean,
    )

    private val _videoState = MutableStateFlow(VideoStreamState())
    val videoState: StateFlow<VideoStreamState> = _videoState.asStateFlow()

    private val _audioState = MutableStateFlow(AudioStreamState())
    val audioState: StateFlow<AudioStreamState> = _audioState.asStateFlow()

    private val clientDroppedFrames = AtomicLong(0L)
    private val mediaEndpointGeneration = AtomicLong(0L)
    private val audioPlaybackLock = Any()
    private val audioQueue = Channel<AudioPacket>(
        capacity = 4,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val videoQueue = Channel<VideoPacket>(
        capacity = 1,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
        onUndeliveredElement = { clientDroppedFrames.incrementAndGet() },
    )
    @Volatile
    private var activeCredentials: ServerCredentials? = null
    @Volatile
    private var videoSocket: WebSocket? = null
    @Volatile
    private var audioSocket: WebSocket? = null
    private var videoReconnectJob: Job? = null
    private var audioReconnectJob: Job? = null
    private var heartbeatJob: Job? = null
    private var watchdogJob: Job? = null
    private var audioTrack: AudioTrack? = null
    private var heartbeatSequence = 1L
    private var videoReconnectAttempt = 0
    private var audioReconnectAttempt = 0
    private var lastVideoFrameElapsedMs = 0L
    private var lastAudioFrameElapsedMs = 0L
    @Volatile
    private var networkAvailable = true

    @Volatile
    private var videoWanted: Boolean = false

    @Volatile
    private var audioWanted: Boolean = false

    @Volatile
    private var videoTransportSuspended: Boolean = false

    @Volatile
    private var audioTransportSuspended: Boolean = false

    val isWanted: Boolean
        get() = videoWanted || audioWanted

    init {
        scope.launch {
            for (packet in videoQueue) {
                if (mediaEndpointGeneration.get() != packet.endpointGeneration) continue
                runCatching { S3MediaProtocol.decodeVideo(packet.bytes.toByteArray()) }
                    .onSuccess { frame ->
                        if (videoTransportSuspended ||
                            mediaEndpointGeneration.get() != packet.endpointGeneration
                        ) return@onSuccess
                        _videoState.update { previous ->
                            val delta = if (previous.frameCount == 0L) {
                                1L
                            } else {
                                (frame.sequence - previous.sequence) and 0xFFFF_FFFFL
                            }
                            if (previous.frameCount > 0L && delta >= 0x8000_0000L) {
                                return@update previous
                            }
                            val missedFrames = if (delta in 2 until 0x8000_0000L) delta - 1L else 0L
                            val now = System.currentTimeMillis()
                            lastVideoFrameElapsedMs = SystemClock.elapsedRealtime()
                            VideoStreamState(
                                wanted = previous.wanted,
                                connected = true,
                                sequence = frame.sequence,
                                width = frame.meta0,
                                height = frame.meta1,
                                quality = frame.meta2,
                                frameCount = previous.frameCount + 1L,
                                gapCount = previous.gapCount + missedFrames,
                                droppedByClient = clientDroppedFrames.get(),
                                lastFrameAtMs = now,
                                stale = false,
                                reconnectAttempt = 0,
                                frameBytes = frame.payload,
                            )
                        }
                    }
                    .onFailure { error ->
                        if (mediaEndpointGeneration.get() == packet.endpointGeneration) {
                            _videoState.update { it.copy(errorMessage = error.message) }
                        }
                    }
            }
        }
        scope.launch {
            for (packet in audioQueue) {
                if (mediaEndpointGeneration.get() != packet.endpointGeneration) continue
                val track = audioTrack ?: continue
                try {
                    synchronized(audioPlaybackLock) {
                        if (mediaEndpointGeneration.get() != packet.endpointGeneration) {
                            return@synchronized
                        }
                        if (packet.discontinuity) {
                            track.pause()
                            track.flush()
                            track.play()
                        }
                        if (!_audioState.value.muted) {
                            track.setVolume(AudioTrack.getMaxVolume())
                            track.write(packet.bytes, 0, packet.bytes.size, AudioTrack.WRITE_BLOCKING)
                        } else {
                            track.setVolume(0f)
                        }
                    }
                } catch (error: Exception) {
                    if (mediaEndpointGeneration.get() == packet.endpointGeneration) {
                        _audioState.update { it.copy(errorMessage = error.message) }
                    }
                }
            }
        }
    }

    @Synchronized
    fun resetEndpoint(credentials: ServerCredentials) {
        mediaEndpointGeneration.incrementAndGet()
        val muted = _audioState.value.muted
        videoReconnectJob?.cancel()
        audioReconnectJob?.cancel()
        videoReconnectJob = null
        audioReconnectJob = null
        videoTransportSuspended = false
        audioTransportSuspended = false
        videoReconnectAttempt = 0
        audioReconnectAttempt = 0
        lastVideoFrameElapsedMs = 0L
        lastAudioFrameElapsedMs = 0L
        clientDroppedFrames.set(0L)
        while (videoQueue.tryReceive().isSuccess) Unit
        while (audioQueue.tryReceive().isSuccess) Unit
        activeCredentials = credentials
        closeSockets()
        _videoState.value = VideoStreamState(wanted = videoWanted)
        _audioState.value = AudioStreamState(wanted = audioWanted, muted = muted)
        synchronized(audioPlaybackLock) {
            audioTrack?.runCatching {
                pause()
                flush()
                if (audioWanted) play()
            }
        }
        // Repository publishes the endpoint-specific network readiness immediately afterwards.
        // Only onNetworkChanged(true) is allowed to schedule the first socket connection.
        ensureHeartbeat()
        ensureWatchdog()
    }

    @Synchronized
    fun startVideo(credentials: ServerCredentials) {
        if (activeCredentials != credentials) {
            activeCredentials = credentials
            closeSockets()
            if (audioWanted) connectAudio()
        }
        videoWanted = true
        _videoState.update { it.copy(wanted = true, errorMessage = null) }
        if (videoSocket == null) connectVideo()
        ensureHeartbeat()
        ensureWatchdog()
    }

    @Synchronized
    fun onStoryTransportChanged(videoSuspended: Boolean, audioSuspended: Boolean) {
        val videoWasSuspended = videoTransportSuspended
        val audioWasSuspended = audioTransportSuspended
        videoTransportSuspended = videoSuspended
        audioTransportSuspended = audioSuspended

        if (videoSuspended && !videoWasSuspended) {
            videoReconnectJob?.cancel()
            videoReconnectJob = null
            while (videoQueue.tryReceive().isSuccess) Unit
            _videoState.update { it.copy(stale = true, frameBytes = null, errorMessage = null) }
        } else if (videoWasSuspended) {
            lastVideoFrameElapsedMs = SystemClock.elapsedRealtime()
            if (videoWanted && videoSocket == null) scheduleVideoReconnect(immediate = true)
        }

        if (audioSuspended && !audioWasSuspended) {
            audioReconnectJob?.cancel()
            audioReconnectJob = null
            _audioState.update { it.copy(receiving = false, errorMessage = null) }
            while (audioQueue.tryReceive().isSuccess) Unit
            synchronized(audioPlaybackLock) {
                audioTrack?.runCatching {
                    pause()
                    flush()
                    play()
                }
            }
        } else if (audioWasSuspended) {
            lastAudioFrameElapsedMs = SystemClock.elapsedRealtime()
            if (audioWanted && audioSocket == null) scheduleAudioReconnect(immediate = true)
        }
    }

    @Synchronized
    fun stopVideo() {
        videoWanted = false
        videoReconnectJob?.cancel()
        videoReconnectJob = null
        val socket = videoSocket
        videoSocket = null
        socket?.cancel()
        _videoState.value = VideoStreamState()
        stopHeartbeatIfIdle()
    }

    @Synchronized
    fun startAudio(credentials: ServerCredentials) {
        if (activeCredentials != credentials) {
            activeCredentials = credentials
            closeSockets()
            if (videoWanted) connectVideo()
        }
        audioWanted = true
        _audioState.update {
            it.copy(wanted = true, muted = false, errorMessage = null)
        }
        ensureAudioTrack()
        if (audioSocket == null) connectAudio()
        ensureHeartbeat()
        ensureWatchdog()
    }

    @Synchronized
    fun stopAudio() {
        audioWanted = false
        audioReconnectJob?.cancel()
        audioReconnectJob = null
        val socket = audioSocket
        audioSocket = null
        socket?.cancel()
        while (audioQueue.tryReceive().isSuccess) Unit
        releaseAudioTrack()
        _audioState.value = AudioStreamState()
        stopHeartbeatIfIdle()
    }

    @Synchronized
    private fun ensureHeartbeat() {
        if (heartbeatJob?.isActive == true) return
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive && isWanted) {
                delay(20_000)
                val heartbeat = JSONObject()
                    .put("type", "media.heartbeat")
                    .put("version", 1)
                    .put("sequence", heartbeatSequence++)
                    .toString()
                synchronized(this@ServerMediaController) {
                    videoSocket?.send(heartbeat)
                    audioSocket?.send(heartbeat)
                }
            }
        }
    }

    @Synchronized
    private fun ensureWatchdog() {
        if (watchdogJob?.isActive == true) return
        watchdogJob = scope.launch {
            while (isActive && isWanted) {
                delay(3_000L)
                val now = SystemClock.elapsedRealtime()
                synchronized(this@ServerMediaController) {
                    if (videoWanted && !videoTransportSuspended && videoSocket != null &&
                        lastVideoFrameElapsedMs > 0L &&
                        now - lastVideoFrameElapsedMs > MEDIA_STALE_MS
                    ) {
                        val socket = videoSocket
                        videoSocket = null
                        socket?.cancel()
                        _videoState.update {
                            it.copy(
                                connected = false,
                                stale = true,
                                errorMessage = "视频流暂时中断，正在恢复",
                            )
                        }
                        scheduleVideoReconnect()
                    }
                    if (audioWanted && !audioTransportSuspended && audioSocket != null &&
                        lastAudioFrameElapsedMs > 0L &&
                        now - lastAudioFrameElapsedMs > MEDIA_STALE_MS
                    ) {
                        val socket = audioSocket
                        audioSocket = null
                        socket?.cancel()
                        _audioState.update {
                            it.copy(
                                connected = false,
                                receiving = false,
                                stale = true,
                                errorMessage = "音频流暂时中断，正在恢复",
                            )
                        }
                        scheduleAudioReconnect()
                    }
                }
            }
        }
    }

    @Synchronized
    fun onNetworkChanged(available: Boolean) {
        networkAvailable = available
        if (!available) {
            mediaEndpointGeneration.incrementAndGet()
            videoReconnectJob?.cancel()
            audioReconnectJob?.cancel()
            videoReconnectJob = null
            audioReconnectJob = null
            closeSockets()
            while (videoQueue.tryReceive().isSuccess) Unit
            while (audioQueue.tryReceive().isSuccess) Unit
            synchronized(audioPlaybackLock) {
                audioTrack?.runCatching {
                    pause()
                    flush()
                    if (audioWanted) play()
                }
            }
            _videoState.update { it.copy(connected = false, stale = true, errorMessage = "等待网络恢复") }
            _audioState.update {
                it.copy(connected = false, receiving = false, stale = true, errorMessage = "等待网络恢复")
            }
        } else {
            if (videoWanted) scheduleVideoReconnect(immediate = true)
            if (audioWanted) scheduleAudioReconnect(immediate = true)
        }
    }

    @Synchronized
    fun stop() {
        mediaEndpointGeneration.incrementAndGet()
        videoWanted = false
        audioWanted = false
        videoTransportSuspended = false
        audioTransportSuspended = false
        activeCredentials = null
        videoReconnectJob?.cancel()
        audioReconnectJob?.cancel()
        heartbeatJob?.cancel()
        watchdogJob?.cancel()
        videoReconnectJob = null
        audioReconnectJob = null
        heartbeatJob = null
        watchdogJob = null
        closeSockets()
        while (videoQueue.tryReceive().isSuccess) Unit
        while (audioQueue.tryReceive().isSuccess) Unit
        scope.launch {
            synchronized(audioPlaybackLock) { releaseAudioTrack() }
        }
        _videoState.value = VideoStreamState()
        _audioState.value = AudioStreamState()
    }

    @Synchronized
    fun setMuted(muted: Boolean) {
        _audioState.update { it.copy(muted = muted) }
        synchronized(audioPlaybackLock) {
            audioTrack?.setVolume(if (muted) 0f else AudioTrack.getMaxVolume())
        }
    }

    @Synchronized
    private fun connectVideo() {
        if (!videoWanted || !networkAvailable || videoTransportSuspended || videoSocket != null) return
        val credentials = activeCredentials ?: return
        val socketGeneration = mediaEndpointGeneration.get()
        val request = transport.websocketRequest(
            credentials,
            "/api/mobile/v1/devices/${credentials.deviceId}/media/video/ws",
        )
        videoSocket = transport.mediaClient.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                synchronized(this@ServerMediaController) {
                    if (videoSocket !== webSocket ||
                        mediaEndpointGeneration.get() != socketGeneration
                    ) return
                    videoReconnectAttempt = 0
                    lastVideoFrameElapsedMs = SystemClock.elapsedRealtime()
                    _videoState.update {
                        it.copy(
                            connected = true,
                            stale = true,
                            reconnectAttempt = 0,
                            errorMessage = null,
                        )
                    }
                }
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                if (videoSocket !== webSocket || videoTransportSuspended ||
                    mediaEndpointGeneration.get() != socketGeneration
                ) return
                val accepted = videoQueue.trySend(
                    VideoPacket(socketGeneration, bytes)
                ).isSuccess
                if (!accepted) clientDroppedFrames.incrementAndGet()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                synchronized(this@ServerMediaController) {
                    if (videoSocket === webSocket &&
                        mediaEndpointGeneration.get() == socketGeneration
                    ) {
                        videoSocket = null
                        _videoState.update {
                            it.copy(connected = false, stale = true, errorMessage = reason)
                        }
                        scheduleVideoReconnect()
                    }
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                synchronized(this@ServerMediaController) {
                    if (videoSocket === webSocket &&
                        mediaEndpointGeneration.get() == socketGeneration
                    ) {
                        videoSocket = null
                        _videoState.update {
                            it.copy(connected = false, stale = true, errorMessage = t.message)
                        }
                        scheduleVideoReconnect()
                    }
                }
            }
        })
    }

    @Synchronized
    private fun connectAudio() {
        if (!audioWanted || !networkAvailable || audioTransportSuspended || audioSocket != null) return
        val credentials = activeCredentials ?: return
        val socketGeneration = mediaEndpointGeneration.get()
        ensureAudioTrack()
        val request = transport.websocketRequest(
            credentials,
            "/api/mobile/v1/devices/${credentials.deviceId}/media/audio/ws",
        )
        audioSocket = transport.mediaClient.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                synchronized(this@ServerMediaController) {
                    if (audioSocket !== webSocket ||
                        mediaEndpointGeneration.get() != socketGeneration
                    ) return
                    audioReconnectAttempt = 0
                    lastAudioFrameElapsedMs = SystemClock.elapsedRealtime()
                    ensureAudioTrack()
                    _audioState.update { it.copy(connected = true, errorMessage = null) }
                }
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                if (audioSocket !== webSocket || audioTransportSuspended ||
                    mediaEndpointGeneration.get() != socketGeneration
                ) return
                runCatching { S3MediaProtocol.decode(bytes.toByteArray()) }
                    .onSuccess { frame ->
                        if (mediaEndpointGeneration.get() != socketGeneration) return@onSuccess
                        if (frame.mediaType != S3MediaProtocol.TYPE_PCM16 ||
                            frame.meta0 != AUDIO_SAMPLE_RATE ||
                            frame.meta2 != 1 ||
                            frame.meta3 != 16
                        ) {
                            return@onSuccess
                        }
                        audioQueue.trySend(
                            AudioPacket(
                                endpointGeneration = socketGeneration,
                                bytes = frame.payload,
                                discontinuity = frame.discontinuity,
                            )
                        )
                        if (mediaEndpointGeneration.get() != socketGeneration) return@onSuccess
                        _audioState.update {
                            lastAudioFrameElapsedMs = SystemClock.elapsedRealtime()
                            it.copy(
                                connected = true,
                                receiving = true,
                                sequence = frame.sequence,
                                lastFrameAtMs = System.currentTimeMillis(),
                                stale = false,
                                reconnectAttempt = 0,
                                errorMessage = null,
                            )
                        }
                    }
                    .onFailure { error ->
                        if (mediaEndpointGeneration.get() == socketGeneration) {
                            _audioState.update { it.copy(errorMessage = error.message) }
                        }
                    }
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                synchronized(this@ServerMediaController) {
                    if (audioSocket === webSocket &&
                        mediaEndpointGeneration.get() == socketGeneration
                    ) {
                        audioSocket = null
                        _audioState.update {
                            it.copy(
                                connected = false,
                                receiving = false,
                                stale = true,
                                errorMessage = reason,
                            )
                        }
                        scheduleAudioReconnect()
                    }
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                synchronized(this@ServerMediaController) {
                    if (audioSocket === webSocket &&
                        mediaEndpointGeneration.get() == socketGeneration
                    ) {
                        audioSocket = null
                        _audioState.update {
                            it.copy(
                                connected = false,
                                receiving = false,
                                stale = true,
                                errorMessage = t.message,
                            )
                        }
                        scheduleAudioReconnect()
                    }
                }
            }
        })
    }

    @Synchronized
    private fun scheduleVideoReconnect(immediate: Boolean = false) {
        if (!videoWanted || !networkAvailable || videoTransportSuspended ||
            videoReconnectJob?.isActive == true
        ) return
        val scheduledGeneration = mediaEndpointGeneration.get()
        videoReconnectAttempt++
        _videoState.update { it.copy(reconnectAttempt = videoReconnectAttempt) }
        videoReconnectJob = scope.launch {
            if (!immediate) delay(calculateBackoffMillis(videoReconnectAttempt))
            synchronized(this@ServerMediaController) {
                if (mediaEndpointGeneration.get() != scheduledGeneration) return@synchronized
                videoReconnectJob = null
                if (videoWanted && videoSocket == null) connectVideo()
            }
        }
    }

    @Synchronized
    private fun scheduleAudioReconnect(immediate: Boolean = false) {
        if (!audioWanted || !networkAvailable || audioTransportSuspended ||
            audioReconnectJob?.isActive == true
        ) return
        val scheduledGeneration = mediaEndpointGeneration.get()
        audioReconnectAttempt++
        _audioState.update { it.copy(reconnectAttempt = audioReconnectAttempt) }
        audioReconnectJob = scope.launch {
            if (!immediate) delay(calculateBackoffMillis(audioReconnectAttempt))
            synchronized(this@ServerMediaController) {
                if (mediaEndpointGeneration.get() != scheduledGeneration) return@synchronized
                audioReconnectJob = null
                if (audioWanted && audioSocket == null) connectAudio()
            }
        }
    }

    private fun ensureAudioTrack() {
        synchronized(audioPlaybackLock) {
            if (audioTrack != null) return
            val minimum = AudioTrack.getMinBufferSize(
                AUDIO_SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            )
            val bufferBytes = maxOf(minimum, 640 * 8)
            audioTrack = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(AUDIO_SAMPLE_RATE)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
                .also {
                    it.setVolume(if (_audioState.value.muted) 0f else AudioTrack.getMaxVolume())
                    it.play()
                }
        }
    }

    private fun releaseAudioTrack() {
        synchronized(audioPlaybackLock) {
            audioTrack?.runCatching {
                pause()
                flush()
                release()
            }
            audioTrack = null
        }
    }

    private fun stopHeartbeatIfIdle() {
        if (isWanted) return
        heartbeatJob?.cancel()
        heartbeatJob = null
        watchdogJob?.cancel()
        watchdogJob = null
        activeCredentials = null
    }

    @Synchronized
    private fun closeSockets() {
        val video = videoSocket
        val audio = audioSocket
        videoSocket = null
        audioSocket = null
        video?.cancel()
        audio?.cancel()
    }

    private companion object {
        const val AUDIO_SAMPLE_RATE = 16_000
        const val MEDIA_STALE_MS = 6_000L
    }
}
