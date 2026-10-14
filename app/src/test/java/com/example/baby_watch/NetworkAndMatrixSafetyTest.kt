package com.example.baby_watch

import com.example.baby_watch.data.server.MobileProtocolParser
import com.example.baby_watch.data.server.SafetyEvent
import com.example.baby_watch.data.server.SafetyEventNotificationPolicy
import com.example.baby_watch.data.server.calculateBackoffMillis
import com.example.baby_watch.data.server.isSequenceRollback
import com.example.baby_watch.data.server.mergeSafetyEvents
import com.example.baby_watch.data.server.peekMatrixKind
import org.junit.Assert.assertFalse
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkAndMatrixSafetyTest {
    @Test
    fun reconnectBackoffGrowsExponentiallyAndCapsAtFifteenSeconds() {
        val delays = (1..8).map { attempt ->
            calculateBackoffMillis(attempt = attempt, jitterMillis = 0L)
        }

        assertEquals(
            listOf(500L, 1_000L, 2_000L, 4_000L, 8_000L, 15_000L, 15_000L, 15_000L),
            delays,
        )
    }

    @Test
    fun reconnectBackoffIncludesProvidedJitter() {
        assertEquals(
            1_125L,
            calculateBackoffMillis(attempt = 2, jitterMillis = 125L),
        )
    }

    @Test
    fun matrixParserRejectsZeroAndOversizedDimensions() {
        assertThrows(IllegalArgumentException::class.java) {
            MobileProtocolParser.parseMatrix(
                """{"matrix":{"kind":"thermal","rows":0,"cols":8,"values":[]}}"""
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            MobileProtocolParser.parseMatrix(
                """{"matrix":{"kind":"pressure","rows":129,"cols":8,"values":[]}}"""
            )
        }
    }

    @Test
    fun matrixParserRejectsPayloadWhoseLengthDoesNotMatchShape() {
        assertThrows(IllegalArgumentException::class.java) {
            MobileProtocolParser.parseMatrix(
                """{"matrix":{"kind":"pressure","rows":2,"cols":2,"values":[1,2,3]}}"""
            )
        }
    }

    @Test
    fun matrixParserAcceptsMaximumSupportedShapeWithoutValues() {
        val matrix = MobileProtocolParser.parseMatrix(
            """{"matrix":{"kind":"thermal","rows":128,"cols":128,"values":[]}}"""
        )

        assertEquals(128, matrix.rows)
        assertEquals(128, matrix.cols)
        assertEquals(emptyList<Int>(), matrix.values)
    }

    @Test
    fun matrixKindPeekStopsAtMetadataWithoutParsingValues() {
        assertEquals(
            "pressure",
            peekMatrixKind(
                """{"matrix":{"sequence":8,"kind":"pressure","values":[this is deliberately not JSON]}}"""
            ),
        )
        assertEquals(
            "thermal",
            peekMatrixKind(
                """{"note":"kind: pressure","matrix":{"kind":"thermal","values":[1,2,3]}}"""
            ),
        )
        assertEquals(null, peekMatrixKind("""{"matrix":{"values":[1,2,3]}}"""))
    }

    @Test
    fun incidentOpenNotifiesOnlyOnceAndFusionBaselineDoesNotSuppressIt() {
        val policy = SafetyEventNotificationPolicy()
        policy.observeFusion(
            listOf(SafetyEventNotificationPolicy.IncidentLevelBaseline(incidentId = 41L, level = 2))
        )

        assertTrue(policy.shouldNotify(structuredEvent(id = 1, incidentId = 41, action = 1, level = 2)))
        assertFalse(policy.shouldNotify(structuredEvent(id = 2, incidentId = 41, action = 1, level = 2)))
    }

    @Test
    fun deviceRestartSequenceRollbackResetsIncidentDeduplication() {
        assertFalse(isSequenceRollback(previous = 41L, candidate = 42L))
        assertTrue(isSequenceRollback(previous = 41L, candidate = 1L))
        assertFalse(isSequenceRollback(previous = 0xFFFF_FFFEL, candidate = 1L))

        val policy = SafetyEventNotificationPolicy()
        val open = structuredEvent(id = 1, incidentId = 1, action = 1, level = 2)
        assertTrue(policy.shouldNotify(open))
        assertFalse(policy.shouldNotify(open.copy(id = 2)))
        policy.reset()
        assertTrue(policy.shouldNotify(open.copy(id = 3)))
    }

    @Test
    fun updateOnlyNotifiesWhenActiveIncidentLevelIncreases() {
        val policy = SafetyEventNotificationPolicy()
        assertTrue(policy.shouldNotify(structuredEvent(id = 1, action = 1, level = 1)))

        assertFalse(
            policy.shouldNotify(structuredEvent(id = 2, action = 2, lifecycle = 2, level = 1))
        )
        assertTrue(
            policy.shouldNotify(structuredEvent(id = 3, action = 2, lifecycle = 2, level = 2))
        )
        assertFalse(
            policy.shouldNotify(structuredEvent(id = 4, action = 2, lifecycle = 4, level = 3))
        )
    }

    @Test
    fun escalationNotifiesOnFirstEscalationAndThenOnlyAtHigherLevel() {
        val policy = SafetyEventNotificationPolicy()
        assertTrue(policy.shouldNotify(structuredEvent(id = 1, action = 1, level = 1)))

        assertTrue(
            policy.shouldNotify(structuredEvent(id = 2, action = 3, lifecycle = 3, level = 2))
        )
        assertFalse(
            policy.shouldNotify(structuredEvent(id = 3, action = 3, lifecycle = 3, level = 2))
        )
        assertTrue(
            policy.shouldNotify(structuredEvent(id = 4, action = 3, lifecycle = 3, level = 3))
        )
    }

    @Test
    fun recoveryResolveAcknowledgedAndZeroSeverityNeverNotify() {
        val policy = SafetyEventNotificationPolicy()
        val recovery = structuredEvent(id = 1, action = 2, lifecycle = 4, level = 2)

        assertFalse(policy.shouldNotify(recovery))
        assertFalse(recovery.requiresAttention)
        assertEquals("恢复确认中", recovery.lifecycleStatusLabel)
        assertFalse(
            policy.shouldNotify(structuredEvent(id = 2, action = 4, lifecycle = 5, level = 0))
        )
        assertFalse(
            policy.shouldNotify(structuredEvent(id = 3, action = 1, acknowledged = true))
        )
        assertFalse(
            policy.shouldNotify(structuredEvent(id = 4, incidentId = 12, action = 1, severity = 0))
        )
    }

    @Test
    fun legacyEventKeepsSeverityNotificationBehaviour() {
        val policy = SafetyEventNotificationPolicy()

        assertTrue(policy.shouldNotify(legacyEvent(id = 1, severity = 1)))
        assertFalse(policy.shouldNotify(legacyEvent(id = 2, severity = 0)))
        assertFalse(policy.shouldNotify(legacyEvent(id = 3, severity = 2, acknowledged = true)))
    }

    @Test
    fun eventTimelineKeepsNewestLifecyclePerIncidentAndLegacyPerEventId() {
        val oldOpen = structuredEvent(id = 10, incidentId = 7, action = 1)
        val latestRecovery = structuredEvent(id = 12, incidentId = 7, action = 2, lifecycle = 4)
        val legacy = legacyEvent(id = 11, severity = 1)

        assertEquals(
            listOf(12L, 11L),
            mergeSafetyEvents(listOf(oldOpen, legacy, latestRecovery)).map(SafetyEvent::id),
        )
    }

    private fun structuredEvent(
        id: Long,
        incidentId: Long = 9L,
        action: Int,
        lifecycle: Int = 2,
        level: Int = 1,
        severity: Int = 1,
        acknowledged: Boolean = false,
    ) = SafetyEvent(
        id = id,
        sourceEventId = null,
        eventType = "test",
        state = "1",
        severity = severity,
        confidence = 90f,
        receivedAt = "",
        acknowledged = acknowledged,
        incidentId = incidentId,
        action = action,
        lifecycle = lifecycle,
        level = level,
    )

    private fun legacyEvent(
        id: Long,
        severity: Int,
        acknowledged: Boolean = false,
    ) = SafetyEvent(
        id = id,
        sourceEventId = null,
        eventType = "legacy",
        state = "1",
        severity = severity,
        confidence = 80f,
        receivedAt = "",
        acknowledged = acknowledged,
    )
}
