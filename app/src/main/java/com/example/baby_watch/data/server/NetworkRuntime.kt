package com.example.baby_watch.data.server

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import okhttp3.Call
import okhttp3.Callback
import okhttp3.ConnectionPool
import okhttp3.Credentials
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.math.min
import kotlin.random.Random
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

internal class AndroidConnectivityObserver(context: Context) {
    private val connectivityManager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private var callback: ConnectivityManager.NetworkCallback? = null

    fun current(): NetworkHealth = connectivityManager.activeNetwork
        ?.let(connectivityManager::getNetworkCapabilities)
        .toNetworkHealth()

    fun start(onChanged: (NetworkHealth) -> Unit) {
        if (callback != null) return
        val listener = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = publish(onChanged)
            override fun onLost(network: Network) = publish(onChanged)
            override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
                publish(onChanged)
            }
        }
        callback = listener
        connectivityManager.registerDefaultNetworkCallback(listener)
        publish(onChanged)
    }

    fun stop() {
        callback?.let { runCatching { connectivityManager.unregisterNetworkCallback(it) } }
        callback = null
    }

    private fun publish(onChanged: (NetworkHealth) -> Unit) {
        onChanged(current())
    }
}

private fun NetworkCapabilities?.toNetworkHealth(): NetworkHealth {
    if (this == null) {
        return NetworkHealth(
            available = false,
            validated = false,
            transport = NetworkTransport.None,
        )
    }
    val transport = when {
        hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> NetworkTransport.Wifi
        hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> NetworkTransport.Cellular
        hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> NetworkTransport.Ethernet
        else -> NetworkTransport.Other
    }
    return NetworkHealth(
        // A Windows hotspot or an isolated nursery LAN may intentionally have no Internet
        // capability. `available` therefore means that a usable link exists; callers still use
        // `validated` when they need the public cloud.
        available = transport != NetworkTransport.None,
        validated = hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
        metered = !hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED),
        transport = transport,
    )
}

internal class ServerHttpTransport {
    private val connectionPool = ConnectionPool(8, 5, TimeUnit.MINUTES)

    val restClient: OkHttpClient = OkHttpClient.Builder()
        .connectionPool(connectionPool)
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .writeTimeout(15, TimeUnit.SECONDS)
        .callTimeout(25, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()

    val streamClient: OkHttpClient = restClient.newBuilder()
        .callTimeout(0, TimeUnit.MILLISECONDS)
        .readTimeout(40, TimeUnit.SECONDS)
        .build()

    val mediaClient: OkHttpClient = restClient.newBuilder()
        .callTimeout(0, TimeUnit.MILLISECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .pingInterval(15, TimeUnit.SECONDS)
        .build()

    suspend fun get(credentials: ServerCredentials, path: String): String {
        val request = requestBuilder(credentials, path).get().build()
        return execute(request, maxAttempts = 3)
    }

    suspend fun postJson(
        credentials: ServerCredentials,
        path: String,
        json: String,
        retryable: Boolean,
    ): String {
        val request = requestBuilder(credentials, path)
            .post(json.toRequestBody(JSON_MEDIA_TYPE))
            .build()
        return execute(request, maxAttempts = if (retryable) 3 else 1)
    }

    fun streamRequest(credentials: ServerCredentials, path: String): Request =
        requestBuilder(credentials, path)
            .header("Accept", "text/event-stream")
            .build()

    fun websocketRequest(credentials: ServerCredentials, path: String): Request {
        val websocketBase = credentials.baseUrl
            .replaceFirst("https://", "wss://")
            .replaceFirst("http://", "ws://")
        return requestBuilder(credentials, websocketBase + path, absolute = true).build()
    }

    private suspend fun execute(request: Request, maxAttempts: Int): String {
        var lastError: Throwable? = null
        repeat(maxAttempts) { index ->
            try {
                return executeOnce(request)
            } catch (error: Throwable) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                lastError = error
                val canRetry = index + 1 < maxAttempts && error.isRetryableNetworkFailure()
                if (!canRetry) throw error
                delay(calculateBackoffMillis(index + 1))
            }
        }
        throw lastError ?: IOException("网络请求失败")
    }

    /**
     * OkHttp's synchronous execute() cannot be interrupted reliably by cancelling a coroutine.
     * Enqueueing the call and binding cancellation to Call.cancel() prevents an old server
     * request from surviving a configuration switch and writing stale state afterwards.
     */
    private suspend fun executeOnce(request: Request): String =
        suspendCancellableCoroutine { continuation ->
            val call = restClient.newCall(request)
            continuation.invokeOnCancellation { call.cancel() }
            call.enqueue(object : Callback {
                override fun onFailure(call: Call, e: IOException) {
                    if (continuation.isActive) continuation.resumeWithException(e)
                }

                override fun onResponse(call: Call, response: Response) {
                    response.use {
                        runCatching {
                            val body = response.body?.string().orEmpty()
                            if (!response.isSuccessful) {
                                val detail = runCatching {
                                    val error = JSONObject(body)
                                    error.optString("error", error.optString("detail"))
                                }.getOrDefault("")
                                throw ServerHttpException(
                                    response.code,
                                    detail.ifBlank { "服务端返回 HTTP ${response.code}" },
                                )
                            }
                            body
                        }.onSuccess { body ->
                            if (continuation.isActive) continuation.resume(body)
                        }.onFailure { error ->
                            if (continuation.isActive) continuation.resumeWithException(error)
                        }
                    }
                }
            })
        }

    private fun requestBuilder(
        credentials: ServerCredentials,
        path: String,
        absolute: Boolean = false,
    ): Request.Builder {
        checkTransportEndpointAllowed(credentials.baseUrl, credentials.mode)
        return Request.Builder()
            .url(if (absolute) path else credentials.baseUrl + path)
            .apply {
                // The direct-Python Plan B server normally runs without Basic authentication.
                // Omitting the header (instead of sending an empty credential) keeps that mode
                // compatible while preserving Basic auth for Docker/cloud deployments.
                if (credentials.username.isNotBlank() || credentials.password.isNotBlank()) {
                    header(
                        "Authorization",
                        Credentials.basic(
                            credentials.username,
                            credentials.password,
                            Charsets.UTF_8,
                        ),
                    )
                }
            }
            .header("Cache-Control", "no-cache")
            .header("User-Agent", "BabyWatch-Android/2")
    }

    private companion object {
        val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()
    }
}

/**
 * Android must opt into cleartext globally because a user-entered private IPv4 address cannot be
 * expressed by a static Network Security Config. Keep the real security boundary here: HTTPS is
 * accepted everywhere, while HTTP is accepted only for a literal RFC1918/loopback IPv4 address.
 * Host names are deliberately not resolved, avoiding DNS rebinding of cleartext traffic.
 */
internal fun checkTransportEndpointAllowed(baseUrl: String, mode: ServerEndpointMode) {
    if (!isAllowedServerUrl(baseUrl, mode)) {
        val message = if (mode == ServerEndpointMode.Local) {
            "电脑备用地址仅支持 HTTPS 或局域网私有 IPv4 HTTP"
        } else {
            "云端服务地址必须使用 HTTPS"
        }
        throw IOException(message)
    }
}

internal class ServerHttpException(
    val statusCode: Int,
    message: String,
) : IOException(message)

internal fun Throwable.isRetryableNetworkFailure(): Boolean = when (this) {
    is ServerHttpException -> statusCode in setOf(408, 425, 429, 500, 502, 503, 504)
    is IOException -> true
    else -> false
}

internal fun calculateBackoffMillis(attempt: Int, jitterMillis: Long? = null): Long {
    val base = min(500L * (1L shl (attempt - 1).coerceIn(0, 5)), 15_000L)
    val jitter = jitterMillis ?: Random.nextLong(0L, (base / 3L).coerceAtLeast(1L))
    return base + jitter
}
