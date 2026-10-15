package com.example.baby_watch.device.mic

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.net.URI
import java.util.concurrent.TimeUnit

enum class AudioStatus { Idle, Connecting, Connected, Receiving, Error }

data class Esp32AudioState(
    val status: AudioStatus = AudioStatus.Idle,
    val url: String = "",
    val isMuted: Boolean = false,
    val receivedPackets: Long = 0,
    val receivedBytes: Long = 0,
    val lastPacketBytes: Int = 0,
    val peakAmplitude: Int = 0,
    val errorMessage: String? = null,
)

class Esp32AudioClient(
    private val url: String,
    private val isMuted: () -> Boolean = { false },
    private val onStateChange: (Esp32AudioState) -> Unit = {},
) {
    companion object {
        private const val TAG = "Esp32AudioClient"
        private const val SAMPLE_RATE = 16000
        private const val CHANNEL_CONFIG = AudioFormat.CHANNEL_OUT_MONO
        private const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
        private const val PACKET_BYTES = 640
        private const val PLAYBACK_GAIN = 8
    }

    private val lock = Any()
    private var audioTrack: AudioTrack? = null
    private var client: OkHttpClient? = null
    private var webSocket: WebSocket? = null
    private var started = false
    private var receivedPackets = 0L
    private var receivedBytes = 0L

    fun start() {
        synchronized(lock) {
            if (started) return
            started = true
            receivedPackets = 0L
            receivedBytes = 0L
            audioTrack = createAudioTrack()
            client = OkHttpClient.Builder()
                .readTimeout(0, TimeUnit.MILLISECONDS)
                .build()
        }
        onStateChange(Esp32AudioState(status = AudioStatus.Connecting, url = url))

        val request = Request.Builder().url(url).build()
        val socketClient = synchronized(lock) { client }
        val socket = socketClient?.newWebSocket(request, listener)
        synchronized(lock) {
            webSocket = socket
        }
    }

    fun stop() {
        val socket: WebSocket?
        val socketClient: OkHttpClient?
        val track: AudioTrack?
        synchronized(lock) {
            if (!started) return
            started = false
            socket = webSocket
            socketClient = client
            track = audioTrack
            webSocket = null
            client = null
            audioTrack = null
        }
        onStateChange(Esp32AudioState(status = AudioStatus.Idle, url = url))

        socket?.close(1000, "manual close")
        socketClient?.dispatcher?.executorService?.shutdown()

        track?.run {
            try {
                pause()
                flush()
            } catch (e: IllegalStateException) {
                Log.w(TAG, "AudioTrack stop ignored: ${e.message}")
            } finally {
                release()
            }
        }
    }

    private val listener = object : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            Log.i(TAG, "WebSocket connected: $url")
            synchronized(lock) {
                audioTrack?.run {
                    setVolume(if (isMuted()) 0f else AudioTrack.getMaxVolume())
                    play()
                }
            }
            onStateChange(Esp32AudioState(status = AudioStatus.Connected, url = url))
        }

        override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
            val pcm = bytes.toByteArray()
            val peakAmplitude = amplifyPcm16LeInPlace(pcm)
            val track = synchronized(lock) { audioTrack }
            if (!isMuted() && track != null && track.playState == AudioTrack.PLAYSTATE_PLAYING) {
                track.setVolume(AudioTrack.getMaxVolume())
                track.write(pcm, 0, pcm.size, AudioTrack.WRITE_BLOCKING)
            } else if (track != null) {
                track.setVolume(0f)
            }

            val state = synchronized(lock) {
                receivedPackets += 1
                receivedBytes += pcm.size
                Esp32AudioState(
                    status = AudioStatus.Receiving,
                    url = url,
                    receivedPackets = receivedPackets,
                    receivedBytes = receivedBytes,
                    lastPacketBytes = pcm.size,
                    peakAmplitude = peakAmplitude,
                )
            }
            onStateChange(state)
            Log.d(TAG, "received audio bytes=${pcm.size} peak=$peakAmplitude")
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            Log.w(TAG, "WebSocket closing: code=$code reason=$reason")
            webSocket.close(code, reason)
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            Log.w(TAG, "WebSocket closed: code=$code reason=$reason")
            onStateChange(Esp32AudioState(status = AudioStatus.Idle, url = url))
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            Log.e(TAG, "WebSocket failure: ${t.message}", t)
            onStateChange(
                Esp32AudioState(
                    status = AudioStatus.Error,
                    url = url,
                    errorMessage = t.message ?: "audio websocket failed",
                ),
            )
        }
    }

    private fun createAudioTrack(): AudioTrack {
        val minBufferSize = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            CHANNEL_CONFIG,
            AUDIO_FORMAT,
        )
        val bufferSize = maxOf(minBufferSize, PACKET_BYTES * 8)

        return AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AUDIO_FORMAT)
                    .setChannelMask(CHANNEL_CONFIG)
                    .build(),
            )
            .setBufferSizeInBytes(bufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }

    private fun amplifyPcm16LeInPlace(pcm: ByteArray): Int {
        var peak = 0
        var index = 0
        while (index + 1 < pcm.size) {
            val sample = ((pcm[index].toInt() and 0xFF) or (pcm[index + 1].toInt() shl 8)).toShort().toInt()
            val abs = kotlin.math.abs(sample)
            if (abs > peak) peak = abs

            val amplified = (sample * PLAYBACK_GAIN).coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
            pcm[index] = (amplified and 0xFF).toByte()
            pcm[index + 1] = ((amplified ushr 8) and 0xFF).toByte()
            index += 2
        }
        return peak
    }
}

object Esp32AudioManager {
    private val _state = MutableStateFlow(Esp32AudioState())
    val state: StateFlow<Esp32AudioState> = _state.asStateFlow()

    private var currentUrl: String? = null
    private var client: Esp32AudioClient? = null
    private var muted = false

    fun connect(host: String) {
        val url = Esp32MicConfig.buildAudioUrl(host)
        val status = _state.value.status
        if (url == currentUrl && status != AudioStatus.Idle && status != AudioStatus.Error) {
            return
        }
        disconnect()
        currentUrl = url
        client = Esp32AudioClient(
            url = url,
            isMuted = { muted },
        ) { nextState ->
            _state.value = nextState.copy(isMuted = muted)
        }.also { it.start() }
    }

    fun disconnect() {
        client?.stop()
        client = null
        currentUrl = null
        _state.update { Esp32AudioState(status = AudioStatus.Idle, url = it.url, isMuted = muted) }
    }

    fun setMuted(enabled: Boolean) {
        muted = enabled
        _state.update { it.copy(isMuted = enabled) }
    }
}

object Esp32MicConfig {
    fun buildAudioUrl(host: String): String {
        val cleanHost = host.trim().trimEnd('/')
        if (cleanHost.isBlank()) return "ws://192.168.31.43:82/audio"

        val uriText = if (cleanHost.contains("://")) cleanHost else "http://$cleanHost"
        val uri = runCatching { URI(uriText) }.getOrNull()
        val address = uri?.host ?: cleanHost.substringBefore('/').substringBefore(':')
        return "ws://$address:82/audio"
    }
}
