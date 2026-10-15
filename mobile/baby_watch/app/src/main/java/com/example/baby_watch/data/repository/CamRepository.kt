package com.example.baby_watch.data.repository

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

private const val TAG = "Esp32Cam"

// ── 配置 ──

object Esp32CamConfig {
    private const val PREFS_NAME = "esp32cam_prefs"
    private const val KEY_HOST = "cam_host"
    const val DEFAULT_HOST = "192.168.31.43"

    private val _host = MutableStateFlow(DEFAULT_HOST)
    val host: StateFlow<String> = _host.asStateFlow()

    fun load(context: Context) {
        _host.value = getSavedHost(context)
    }

    fun getSavedHost(context: Context): String {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_HOST, DEFAULT_HOST) ?: DEFAULT_HOST
    }

    fun saveHost(context: Context, host: String) {
        val cleanHost = host.trim()
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putString(KEY_HOST, cleanHost).apply()
        _host.value = cleanHost.ifBlank { DEFAULT_HOST }
    }

    fun buildUrl(host: String): String {
        val cleanHost = host.trim().trimEnd('/')
        return if (cleanHost.contains("://")) cleanHost
        else "http://$cleanHost"
    }
}

// ── 状态 ──

enum class CamStatus { Idle, Connecting, Connected, Error }

data class Esp32CamState(
    val status: CamStatus = CamStatus.Idle,
    val frame: Bitmap? = null,
    val errorMessage: String? = null,
)

// ── 全局管理器 ──

object Esp32CamManager {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var streamJob: Job? = null
    private var currentHost: String? = null

    private val _state = MutableStateFlow(Esp32CamState())
    val state: StateFlow<Esp32CamState> = _state.asStateFlow()

    fun connect(host: String) {
        if (host == currentHost && _state.value.status == CamStatus.Connected) return
        disconnect()
        currentHost = host
        _state.update { it.copy(status = CamStatus.Connecting, errorMessage = null) }
        streamJob = scope.launch { resolveAndStream(host) }
    }

    fun disconnect() {
        streamJob?.cancel()
        streamJob = null
        _state.update { it.copy(status = CamStatus.Idle, frame = null, errorMessage = null) }
    }

    // ── 端点探测 ──

    private suspend fun resolveAndStream(host: String) {
        val base = Esp32CamConfig.buildUrl(host)

        var url = "$base/stream"
        if (tryConnectStream(url)) return

        for (ep in listOf("/capture", "/snapshot", "/jpg")) {
            url = "$base$ep"
            if (tryConnectJpeg(url)) {
                Log.d(TAG, "JPEG 端点: $url")
                fetchJpegLoop(url)
                return
            }
        }
        _state.update { it.copy(status = CamStatus.Error, errorMessage = "无法连接摄像头") }
        Log.e(TAG, "所有端点失败: $base")
    }

    private suspend fun tryConnectStream(url: String): Boolean {
        try {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 3000; readTimeout = 3000
            }
            if (conn.responseCode == 200) {
                val ct = conn.contentType ?: ""
                conn.disconnect()
                if (ct.contains("multipart", ignoreCase = true) ||
                    ct.contains("video", ignoreCase = true)) {
                    Log.d(TAG, "MJPEG 端点: $url")
                    runMjpegLoop(url)
                    return true
                }
            }
            conn.disconnect()
        } catch (_: Exception) {}
        return false
    }

    private suspend fun tryConnectJpeg(url: String): Boolean = try {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 3000; readTimeout = 3000
        }
        val bmp = BitmapFactory.decodeStream(conn.inputStream)
        conn.disconnect()
        bmp != null
    } catch (_: Exception) { false }

    // ── MJPEG 流 ──

    private suspend fun runMjpegLoop(url: String) {
        while (currentCoroutineContext().isActive) {
            try {
                runMjpegStream(url)
            } catch (e: Exception) {
                Log.e(TAG, "MJPEG 断开: ${e.message}")
            }
            _state.update { it.copy(status = CamStatus.Error, errorMessage = "重连中...") }
            delay(1000)
        }
    }

    private suspend fun runMjpegStream(url: String) {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 3000; readTimeout = 0
        }
        val input = BufferedInputStream(conn.inputStream)
        val buffer = ByteArray(4096)
        val frameBuffer = ByteArrayOutputStream()
        var inFrame = false

        while (currentCoroutineContext().isActive) {
            val n = input.read(buffer)
            if (n < 0) break
            var i = 0
            while (i < n) {
                if (!inFrame) {
                    if (i + 1 < n && buffer[i] == 0xFF.toByte() && buffer[i + 1] == 0xD8.toByte()) {
                        inFrame = true
                        frameBuffer.reset()
                        frameBuffer.write(buffer, i, n - i)
                        i = n
                    } else i++
                } else {
                    frameBuffer.write(buffer[i].toInt() and 0xFF)
                    if (i + 1 < n && buffer[i] == 0xFF.toByte() && buffer[i + 1] == 0xD9.toByte()) {
                        frameBuffer.write(0xD9)
                        i += 2
                        inFrame = false
                        val jpeg = frameBuffer.toByteArray()
                        val bmp = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size)
                        frameBuffer.reset()
                        if (bmp != null) {
                            _state.update { it.copy(status = CamStatus.Connected, frame = bmp) }
                        }
                    } else i++
                }
            }
        }
        input.close()
    }

    // ── JPEG 轮询 ──

    private suspend fun fetchJpegLoop(url: String) {
        while (currentCoroutineContext().isActive) {
            try {
                val bmp = withContext(Dispatchers.IO) {
                    val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                        connectTimeout = 3000; readTimeout = 3000
                    }
                    try { BitmapFactory.decodeStream(conn.inputStream) }
                    finally { conn.disconnect() }
                }
                if (bmp != null) _state.update { it.copy(status = CamStatus.Connected, frame = bmp) }
            } catch (_: Exception) {
                _state.update { it.copy(status = CamStatus.Error, errorMessage = "重连中...") }
                delay(1000)
            }
            delay(180)
        }
    }
}
