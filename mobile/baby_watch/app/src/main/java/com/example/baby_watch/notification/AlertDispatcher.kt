package com.example.baby_watch.notification

import android.content.Context
import androidx.core.content.edit
import com.example.baby_watch.data.server.SafetyEvent
import com.example.baby_watch.data.server.alertTierForSeverity
import com.example.baby_watch.service.log.LogManager

object AlertDispatcher {
    fun dispatch(context: Context, event: SafetyEvent) {
        val tier = alertTierForSeverity(event.severity)
        if (tier == 0) return

        val appContext = context.applicationContext
        val title = event.title
        val detail = event.detail
        LogManager.alert(tier, title, detail)

        runCatching {
            com.example.baby_watch.notification.phone_notice.phone_notice.show(
                appContext,
                event.id,
                title,
                detail,
            )
        }.onFailure {
            LogManager.system("通知失败", it.message.orEmpty())
        }

        // Every event remains visible in the notification tray, but repeated sensor bursts must
        // not continuously vibrate or ring the caregiver's phone.
        if (!claimAudibleWindow(appContext, tier)) return

        when (tier) {
            1 -> runCatching {
                com.example.baby_watch.notification.vibration.vibration.start(appContext)
            }.onFailure {
                LogManager.system("震动失败", it.message.orEmpty())
            }

            2 -> {
                runCatching {
                    com.example.baby_watch.notification.ring.ring.start(appContext, 5000)
                }.onFailure {
                    LogManager.system("响铃失败", it.message.orEmpty())
                }
            }

            3 -> {
                runCatching {
                    com.example.baby_watch.notification.ring_and_vibration.ring_and_vibration.start(
                        appContext
                    )
                }.onFailure {
                    LogManager.system("紧急响铃失败", it.message.orEmpty())
                }
            }
        }
    }

    @Synchronized
    private fun claimAudibleWindow(context: Context, tier: Int): Boolean {
        val now = System.currentTimeMillis()
        val cooldownMs = when (tier) {
            1 -> 20_000L
            2 -> 45_000L
            else -> 60_000L
        }
        val preferences = context.getSharedPreferences("alert_delivery_policy", Context.MODE_PRIVATE)
        val key = "last_signal_tier_$tier"
        val previous = preferences.getLong(key, 0L)
        if (previous > 0L && now >= previous && now - previous < cooldownMs) return false
        preferences.edit { putLong(key, now) }
        return true
    }
}
