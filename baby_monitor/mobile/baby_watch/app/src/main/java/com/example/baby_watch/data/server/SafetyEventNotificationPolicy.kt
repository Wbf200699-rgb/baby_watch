package com.example.baby_watch.data.server

/**
 * Decides whether a server safety-event transition deserves a user notification.
 *
 * The server can emit several rows for one incident (open, update, recovery and resolve).
 * Keeping this state outside Android APIs makes the lifecycle rules deterministic and easy to
 * exercise in local unit tests.
 */
internal class SafetyEventNotificationPolicy {
    data class IncidentLevelBaseline(
        val incidentId: Long,
        val level: Int,
    )

    private data class IncidentState(
        var knownLevel: Int = 0,
        var highestNotifiedLevel: Int = 0,
        var openNotified: Boolean = false,
        var escalationNotified: Boolean = false,
    )

    private val incidents = mutableMapOf<Long, IncidentState>()

    /**
     * Seeds the current level without pretending that an OPEN event has already been delivered.
     * SSE snapshots can arrive before the corresponding event row.
     */
    @Synchronized
    fun observeFusion(activeIncidents: Iterable<IncidentLevelBaseline>) {
        activeIncidents.forEach { baseline ->
            if (baseline.incidentId <= 0L) return@forEach
            val state = incidents.getOrPut(baseline.incidentId, ::IncidentState)
            state.knownLevel = maxOf(state.knownLevel, baseline.level.coerceAtLeast(0))
        }
    }

    @Synchronized
    fun shouldNotify(event: SafetyEvent): Boolean {
        val incidentId = event.incidentId?.takeIf { it > 0L }
        val action = event.action

        // Older deployments have no incident lifecycle fields. Preserve their established
        // behaviour; repository event-id persistence still prevents replay after reconnect.
        if (incidentId == null || action == null) {
            return !event.acknowledged && event.severity > 0
        }

        val effectiveLevel = (event.level ?: event.globalLevel ?: event.severity).coerceAtLeast(0)
        val eligible = !event.acknowledged && event.severity > 0

        if (event.lifecycle == LIFECYCLE_RESOLVED || action == ACTION_RESOLVE) {
            incidents.remove(incidentId)
            return false
        }

        val state = incidents.getOrPut(incidentId, ::IncidentState)
        if (event.lifecycle == LIFECYCLE_RECOVERY_HOLD) {
            state.knownLevel = maxOf(state.knownLevel, effectiveLevel)
            return false
        }

        return when (action) {
            ACTION_OPEN -> {
                val notify = !state.openNotified
                state.openNotified = true
                state.knownLevel = maxOf(state.knownLevel, effectiveLevel)
                if (notify) {
                    state.highestNotifiedLevel = maxOf(state.highestNotifiedLevel, effectiveLevel)
                }
                eligible && notify
            }

            ACTION_UPDATE -> {
                val activeLifecycle = event.lifecycle == LIFECYCLE_ACTIVE ||
                    event.lifecycle == LIFECYCLE_ESCALATED
                val notify = activeLifecycle && effectiveLevel > state.knownLevel
                state.knownLevel = maxOf(state.knownLevel, effectiveLevel)
                if (notify) {
                    state.highestNotifiedLevel = maxOf(state.highestNotifiedLevel, effectiveLevel)
                }
                eligible && notify
            }

            ACTION_ESCALATE -> {
                val notify = !state.escalationNotified ||
                    effectiveLevel > state.highestNotifiedLevel
                state.escalationNotified = true
                state.knownLevel = maxOf(state.knownLevel, effectiveLevel)
                if (notify) {
                    state.highestNotifiedLevel = maxOf(state.highestNotifiedLevel, effectiveLevel)
                }
                eligible && notify
            }

            else -> {
                state.knownLevel = maxOf(state.knownLevel, effectiveLevel)
                false
            }
        }
    }

    @Synchronized
    fun reset() {
        incidents.clear()
    }

    private companion object {
        const val ACTION_OPEN = 1
        const val ACTION_UPDATE = 2
        const val ACTION_ESCALATE = 3
        const val ACTION_RESOLVE = 4

        const val LIFECYCLE_ACTIVE = 2
        const val LIFECYCLE_ESCALATED = 3
        const val LIFECYCLE_RECOVERY_HOLD = 4
        const val LIFECYCLE_RESOLVED = 5
    }
}
