package com.example.baby_watch.data.server

import android.content.Context
import androidx.core.content.edit
import org.json.JSONArray
import org.json.JSONObject

internal class MonitorHistoryStore(context: Context) {
    private val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    fun load(
        namespace: String = DEFAULT_HISTORY_NAMESPACE,
        nowMs: Long = System.currentTimeMillis(),
    ): List<TrendSample> {
        val cutoff = nowMs - RETENTION_MS
        val key = sampleKey(namespace)
        val raw = preferences.getString(key, null)
            ?: if (namespace == DEFAULT_HISTORY_NAMESPACE) {
                // Versions before endpoint profiles only ever connected to the fixed cloud origin.
                // Migrate that history to cloud once; never expose it in the local Plan B profile.
                preferences.getString(KEY_SAMPLES, null)?.also { legacy ->
                    preferences.edit { putString(key, legacy) }
                }
            } else {
                null
            }
            ?: return emptyList()
        return runCatching {
            val array = JSONArray(raw)
            buildList {
                for (index in 0 until array.length()) {
                    val item = array.optJSONObject(index) ?: continue
                    val timestamp = item.optLong("at", 0L)
                    if (timestamp < cutoff) continue
                    add(
                        TrendSample(
                            timestampMs = timestamp,
                            temperatureC = item.optionalFloat("temperature"),
                            humidityPercent = item.optionalFloat("humidity"),
                            co2Ppm = item.optionalInt("co2"),
                            cryConfidence = item.optDouble("cry", 0.0).toFloat(),
                            severity = item.optInt("severity", 0),
                            safetyState = item.optInt("state", 0),
                            occupied = item.optionalBoolean("occupied"),
                        )
                    )
                }
            }.sortedBy(TrendSample::timestampMs).takeLast(MAX_SAMPLES)
        }.getOrDefault(emptyList())
    }

    fun append(
        namespace: String,
        current: List<TrendSample>,
        snapshot: MonitorSnapshot,
        nowMs: Long = System.currentTimeMillis(),
    ): List<TrendSample> {
        val previous = current.lastOrNull()
        val importantChange = previous != null &&
            (previous.severity != snapshot.inference.severity ||
                previous.safetyState != snapshot.inference.state)
        if (previous != null) {
            val elapsedMs = nowMs - previous.timestampMs
            val severityEscalated = snapshot.inference.severity > previous.severity
            if (!importantChange && elapsedMs < SAMPLE_INTERVAL_MS) return current
            if (importantChange && !severityEscalated && elapsedMs < CHANGE_DEBOUNCE_MS) return current
        }
        val bedAverage = snapshot.fivePointTemperaturesC.filterNotNull()
            .takeIf(List<Float>::isNotEmpty)
            ?.average()
            ?.toFloat()
        val sample = TrendSample(
            timestampMs = nowMs,
            temperatureC = snapshot.bme688.temperatureC
                ?: snapshot.scd41.temperatureC
                ?: bedAverage,
            humidityPercent = snapshot.bme688.humidityPercent ?: snapshot.scd41.humidityPercent,
            co2Ppm = snapshot.scd41.co2Ppm,
            cryConfidence = snapshot.inference.cryConfidence,
            severity = snapshot.inference.severity,
            safetyState = snapshot.inference.state,
            occupied = snapshot.fusedPresence(),
        )
        val cutoff = nowMs - RETENTION_MS
        val updated = (current + sample)
            .asSequence()
            .filter { it.timestampMs >= cutoff }
            .takeLast(MAX_SAMPLES)
            .toList()
        save(namespace, updated)
        return updated
    }

    fun append(
        current: List<TrendSample>,
        snapshot: MonitorSnapshot,
        nowMs: Long = System.currentTimeMillis(),
    ): List<TrendSample> = append(DEFAULT_HISTORY_NAMESPACE, current, snapshot, nowMs)

    private fun save(namespace: String, samples: List<TrendSample>) {
        val array = JSONArray()
        samples.forEach { sample ->
            array.put(
                JSONObject()
                    .put("at", sample.timestampMs)
                    .putOptional("temperature", sample.temperatureC)
                    .putOptional("humidity", sample.humidityPercent)
                    .putOptional("co2", sample.co2Ppm)
                    .put("cry", sample.cryConfidence)
                    .put("severity", sample.severity)
                    .put("state", sample.safetyState)
                    .putOptional("occupied", sample.occupied)
            )
        }
        preferences.edit { putString(sampleKey(namespace), array.toString()) }
    }

    private fun sampleKey(namespace: String): String =
        "${KEY_SAMPLES}_${namespace.hashCode().toUInt().toString(16)}"

    private companion object {
        const val PREFERENCES_NAME = "monitor_history"
        const val KEY_SAMPLES = "samples_v1"
        const val DEFAULT_HISTORY_NAMESPACE = "$DEFAULT_SERVER_URL|$FIXED_DEVICE_ID"
        const val CHANGE_DEBOUNCE_MS = 5_000L
        const val SAMPLE_INTERVAL_MS = 60_000L
        const val RETENTION_MS = 7L * 24L * 60L * 60L * 1000L
        const val MAX_SAMPLES = 2048
    }
}

private fun MonitorSnapshot.fusedPresence(): Boolean? {
    if (fusion.schemaVersion >= 5 && fusion.evidence.available) {
        return when (fusion.contexts["presence"]?.label?.lowercase()) {
            "present" -> true
            "absent" -> false
            else -> null
        }
    }
    return inference.occupied ?: pressure.occupied
}

private fun JSONObject.putOptional(name: String, value: Any?): JSONObject =
    put(name, value ?: JSONObject.NULL)

private fun JSONObject.optionalFloat(name: String): Float? =
    if (isNull(name) || !has(name)) null else optDouble(name).toFloat()

private fun JSONObject.optionalInt(name: String): Int? =
    if (isNull(name) || !has(name)) null else optInt(name)

private fun JSONObject.optionalBoolean(name: String): Boolean? =
    if (isNull(name) || !has(name)) null else optBoolean(name)

private fun <T> Sequence<T>.takeLast(count: Int): List<T> = toList().takeLast(count)
