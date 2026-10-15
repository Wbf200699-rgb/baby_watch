package com.example.baby_watch.data.repository

import android.content.Context
import com.example.baby_watch.service.log.LogManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class HomeData(
    val temperature: Float = 0f,
    val humidity: Float = 0f,
    val co2: Int = 0,
    val alertLevel: Int = 0,
    val alertTitle: String = "暂无异常，系统持续监测中",
    val alertHint: String = "",
    val babyStatus: String = "睡眠中",
    val lastUpdate: Long = 0L,
)

data class LogItem(
    val time: String,
    val type: LogManager.LogType,
    val title: String,
    val detail: String,
)

object DataBridge {
    private val _state = MutableStateFlow(HomeData())
    val state: StateFlow<HomeData> = _state.asStateFlow()

    private val _logs = MutableStateFlow<List<LogItem>>(emptyList())
    val logs: StateFlow<List<LogItem>> = _logs.asStateFlow()

    private var appContext: Context? = null
    private var lastAlertLevel = 0
    private var isStarted = false

    private fun syncLogs() {
        _logs.value = LogManager.getAll().map {
            LogItem(it.time, it.type, it.title, it.detail)
        }
    }

    fun start(ctx: Context) {
        if (isStarted) return
        isStarted = true
        appContext = ctx.applicationContext
        LogManager.setOnLogAddedListener { syncLogs() }
        LogManager.system("系统启动", "守护模式已开启")
        syncLogs()

        data_and_notice.setOnUpdateListener {
            val level = data_and_notice.alertLevel
            val title = data_and_notice.alertTitle.ifEmpty { defaultTitle(level) }
            val hint = data_and_notice.alertHint
            val detail = "T:${data_and_notice.temperature}°C H:${data_and_notice.humidity}% CO2:${data_and_notice.co2}ppm"

            _state.value = HomeData(
                temperature = data_and_notice.temperature,
                humidity = data_and_notice.humidity,
                co2 = data_and_notice.co2,
                alertLevel = level,
                alertTitle = title,
                alertHint = hint,
                babyStatus = data_and_notice.babyStatus.ifEmpty { "睡眠中" },
                lastUpdate = data_and_notice.lastUpdate,
            )

            if (level >= 1) {
                LogManager.alert(level, title, detail)
            } else {
                LogManager.notification("传感器更新", detail)
            }

            if (level != lastAlertLevel) {
                triggerAlert(level, title, hint)
                lastAlertLevel = level
            }
        }
        data_and_notice.start()
    }

    private fun defaultTitle(level: Int): String = when (level) {
        1 -> "状态轻微波动，请留意"
        2 -> "已触发自动安抚"
        3 -> "检测到高风险事件"
        else -> "暂无异常，系统持续监测中"
    }

    private fun triggerAlert(level: Int, title: String, hint: String) {
        val ctx = appContext ?: return
        val eventId = System.currentTimeMillis()
        when (level) {
            1 -> {
                com.example.baby_watch.notification.vibration.vibration.start(ctx)
                com.example.baby_watch.notification.phone_notice.phone_notice.show(ctx, eventId, title, hint)
            }
            2 -> {
                com.example.baby_watch.notification.ring.ring.start(ctx, 5000)
                com.example.baby_watch.notification.phone_notice.phone_notice.show(ctx, eventId, title, hint)
                com.example.baby_watch.notification.text.text.send(ctx)
            }
            3 -> {
                com.example.baby_watch.notification.ring_and_vibration.ring_and_vibration.start(ctx)
                com.example.baby_watch.notification.phone_notice.phone_notice.show(ctx, eventId, title, hint)
                com.example.baby_watch.notification.call.call.dial(ctx)
            }
        }
    }

    fun stop() {
        if (!isStarted) return
        isStarted = false
        data_and_notice.stop()
    }
}
