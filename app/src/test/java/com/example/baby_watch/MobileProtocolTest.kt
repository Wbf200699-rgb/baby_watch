package com.example.baby_watch

import com.example.baby_watch.data.server.S3MediaProtocol
import com.example.baby_watch.data.server.FusionSummary
import com.example.baby_watch.data.server.MatrixFrame
import com.example.baby_watch.data.server.MobileProtocolParser
import com.example.baby_watch.data.server.StoryTransferSummary
import com.example.baby_watch.data.server.alertTierForSeverity
import com.example.baby_watch.data.server.incidentTypeLabel
import com.example.baby_watch.data.server.safetyStateLabel
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MobileProtocolTest {
    @Test
    fun mobileV1SnapshotParsesDeviceTelemetryDiagnosticsAndMedia() {
        val json = """
            {
              "type":"mobile.snapshot",
              "protocol_version":1,
              "server_time":"2026-07-27T10:00:00Z",
              "device":{
                "id":"esp32cam-01",
                "online":true,
                "firmware":"esp32s3-0.2.0",
                "hardware":"ESP32-S3-WROOM-CAM-N16R8",
                "wifi_rssi":-51,
                "last_seen":"2026-07-27T09:59:59Z",
                "capabilities":["s3_camera_jpeg_wss","s3_external_microphone"]
              },
              "telemetry":{
                "sensors":{
                  "bed_temperature":{"t_mc":[25100,25200,25300,25400,25500]},
                  "thermal":{"ctr_cd":263},
                  "audio_player":{"s":5,"track":2,"count":4,"vol":70,"file":"LULLABY.WAV"}
                },
                "inference":{"state":10,"candidate":10,"confidence":96.2,"severity":0,"faults":0}
              },
              "diagnostics":{"overall":"ok","items":[]},
              "vision_overlay":{
                "schema_version":1,
                "enabled":true,
                "calibration_status":"calibrated",
                "source_frame":{"sequence":77,"age_ms":180},
                "stale":false,
                "expired":false,
                "boxes":[{
                  "class_id":0,"label":"infant","score_x1000":812,
                  "x":0.12,"y":0.18,"width":0.51,"height":0.62
                }]
              },
              "matrices":{
                "pressure":{"available":true,"sequence":7,"rows":2,"cols":2,"unit":"raw","received_at":"now"},
                "thermal":{"available":true,"sequence":8,"rows":2,"cols":2,"unit":"deci_c","received_at":"now"}
              },
              "media":{
                "video":{"available":true,"listeners":1},
                "audio":{"available":true,"listeners":1}
              }
            }
        """.trimIndent()

        val snapshot = MobileProtocolParser.parseSnapshot(json)

        assertEquals("esp32cam-01", snapshot.device.id)
        assertEquals(true, snapshot.device.online)
        assertEquals(listOf(25.1f, 25.2f, 25.3f, 25.4f, 25.5f), snapshot.fivePointTemperaturesC)
        assertEquals(26.3f, snapshot.thermalCenterC)
        assertEquals("ok", snapshot.diagnosticsOverall)
        assertEquals(true, snapshot.videoAvailable)
        assertEquals(true, snapshot.audioAvailable)
        assertEquals(true, snapshot.visionOverlay.enabled)
        assertEquals(false, snapshot.visionOverlay.stale)
        assertEquals(false, snapshot.visionOverlay.expired)
        assertEquals("calibrated", snapshot.visionOverlay.calibrationStatus)
        assertEquals(180L, snapshot.visionOverlay.sourceAgeMs)
        assertEquals("infant", snapshot.visionOverlay.boxes.single().label)
        assertEquals(0.51f, snapshot.visionOverlay.boxes.single().width)
    }

    @Test
    fun mobileV1DeviceListMatrixAndEventsUseStableWrappers() {
        val devices = """
            {"type":"mobile.devices","protocol_version":1,
             "devices":[{"id":"esp32cam-01","online":true}]}
        """.trimIndent()
        val matrixJson = """
            {"type":"mobile.matrix","protocol_version":1,"device_id":"esp32cam-01",
             "matrix":{"kind":"pressure","sequence":3,"rows":1,"cols":2,
                       "unit":"raw","received_at":"now","values":[0,8]}}
        """.trimIndent()
        val eventsJson = """
            {"type":"mobile.events","protocol_version":1,"events":[{
              "id":5,"source_event_id":11,"event_type":"near_edge","state":"1",
              "severity":4,"confidence":92.0,"received_at":"now",
              "acknowledged":0,"payload_json":{}
            }]}
        """.trimIndent()

        assertEquals(true, MobileProtocolParser.containsDevice(devices, "esp32cam-01"))
        val matrix = MobileProtocolParser.parseMatrix(matrixJson)
        assertEquals(listOf(0, 8), matrix.values)
        assertEquals(1, matrix.activePoints)
        assertEquals(5L, MobileProtocolParser.parseEvents(eventsJson).single().id)
    }

    @Test
    fun mobileV1CapabilitiesAndCommandResponseAreValidated() {
        val capabilities = MobileProtocolParser.parseCapabilities(
            """{
              "type":"mobile.capabilities","protocol_version":1,
              "matrix_kinds":["thermal","pressure"],
              "environment_sensors":{"bme688":{"gas_semantics":"raw_resistance_only","bsec":false}},
              "ai":{"enabled":true},
              "commands":[{"action":"audio.next"}]
            }""".trimIndent()
        )
        assertEquals(true, capabilities.aiEnabled)
        assertEquals(true, capabilities.bme688Supported)
        assertEquals(setOf("thermal", "pressure"), capabilities.matrixKinds)
        assertEquals(setOf("audio.next"), capabilities.commands)
        assertThrows(IllegalArgumentException::class.java) {
            MobileProtocolParser.parseCapabilities(
                """{"type":"mobile.capabilities","protocol_version":2}"""
            )
        }

        val command = MobileProtocolParser.parseCommand("""
            {"type":"mobile.command","protocol_version":1,"duplicate":false,
             "command":{"id":23,"action":"audio.next","status":"queued","result":null}}
        """.trimIndent())
        assertEquals(23L, command.id)
        assertEquals("audio.next", command.action)
        assertEquals("queued", command.status)
    }

    @Test
    fun newEnvironmentPressureAndMatrixQualityFieldsAreParsed() {
        val snapshot = MobileProtocolParser.parseSnapshot(
            """{
              "protocol_version":1,
              "device":{"id":"esp32cam-01","online":true,"matrix_connected":true},
              "telemetry":{
                "ra8_tick":125000,
                "sensors":{"environment":{
                  "s":3,"co2":728,"t_mc":24600,"rh_mp":51200,"last":123000,
                  "crc_err":1,"i2c_err":0,
                  "bme688":{"s":3,"addr":118,"t_mc":24750,"rh_mp":50820,
                    "p_pa":100825,"gas_ohm":125400,"flags":7,"last":123450,"err":0}
                }},
                "inference":{"occupied":1,"p_total":5032,"p_peak":92,"p_points":14,
                  "p_edge":410,"px100":735,"py100":820}
              }
            }""".trimIndent()
        )
        assertEquals(true, snapshot.device.matrixConnected)
        assertEquals(true, snapshot.scd41.valid)
        assertEquals(728, snapshot.scd41.co2Ppm)
        assertEquals(24.6f, snapshot.scd41.temperatureC)
        assertEquals(true, snapshot.bme688.climateValid)
        assertEquals(true, snapshot.bme688.gasValid)
        assertEquals(1008.25f, snapshot.bme688.pressureHpa)
        assertEquals(125.4f, snapshot.bme688.rawGasKohm)
        assertEquals(5032L, snapshot.pressure.total)
        assertEquals(14, snapshot.pressureActivePoints)

        val matrix = MobileProtocolParser.parseMatrix(
            """{"matrix":{"kind":"pressure","sequence":9,"rows":1,"cols":2,
              "unit":"raw","received_at":"now","minimum":0,"maximum":99,"fps":3.98,
              "frame_age_ms":2301,"drops":4,"delayed":false,"values":[0,99]}}"""
        )
        assertEquals(0, matrix.minimum)
        assertEquals(99, matrix.maximum)
        assertEquals(3.98f, matrix.fps)
        assertEquals(2301L, matrix.frameAgeMs)
        assertEquals(4L, matrix.drops)
        assertEquals(true, matrix.delayed)
    }

    @Test
    fun aiStoryAndAdvisoryWrappersAreParsed() {
        val story = MobileProtocolParser.parseStory(
            """{"type":"mobile.ai_story","story":{"story_id":"abc","status":"ready",
              "title":"月亮","story_text":"从前……","audio_size_bytes":1200,
              "audio_duration_ms":60000,"command_id":null,"error":"",
              "created_at":"a","updated_at":"b"}}"""
        )
        assertEquals("abc", story.storyId)
        assertEquals(true, story.canReplay)

        val advisory = MobileProtocolParser.parseAdvisory(
            """{"type":"mobile.ai_advisory","data":{"advisory_id":"tip-1",
              "level":"attention","title":"请查看婴儿状态","summary":"检测到变化",
              "evidence":["压力重心变化"],"actions":["查看实时画面"],
              "confidence":0.82,"requires_user_attention":true,"source_sequence":123,
              "acknowledged":false,"created_at":"now"}}"""
        )
        assertEquals("tip-1", advisory.advisoryId)
        assertEquals(listOf("查看实时画面"), advisory.actions)
        assertEquals(true, advisory.requiresUserAttention)
    }

    @Test
    fun s3mdFrameDecodesNetworkOrderHeaderAndPayload() {
        val payload = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 1, 2, 0xFF.toByte(), 0xD9.toByte())
        val message = ByteBuffer.allocate(S3MediaProtocol.HEADER_BYTES + payload.size)
            .order(ByteOrder.BIG_ENDIAN)
            .put("S3MD".toByteArray())
            .put(1)
            .put(S3MediaProtocol.TYPE_JPEG.toByte())
            .putShort(1)
            .putInt(0xFFFF_FFFE.toInt())
            .putLong(123_456L)
            .putInt(payload.size)
            .putShort(640)
            .putShort(480)
            .putShort(12)
            .putShort(0)
            .put(payload)
            .array()

        val decoded = S3MediaProtocol.decodeVideo(message)

        assertEquals(S3MediaProtocol.TYPE_JPEG, decoded.mediaType)
        assertEquals(4_294_967_294L, decoded.sequence)
        assertEquals(123_456L, decoded.timestampUs)
        assertEquals(640, decoded.meta0)
        assertEquals(480, decoded.meta1)
        assertEquals(12, decoded.meta2)
        assertEquals(true, decoded.discontinuity)
        assertArrayEquals(payload, decoded.payload)
    }

    @Test
    fun s3mdFrameRejectsMismatchedPayloadLength() {
        val message = ByteBuffer.allocate(S3MediaProtocol.HEADER_BYTES)
            .order(ByteOrder.BIG_ENDIAN)
            .put("S3MD".toByteArray())
            .put(1)
            .put(S3MediaProtocol.TYPE_PCM16.toByte())
            .putShort(0)
            .putInt(1)
            .putLong(20_000L)
            .putInt(640)
            .putShort(16_000.toShort())
            .putShort(320)
            .putShort(1)
            .putShort(16)
            .array()

        assertThrows(IllegalArgumentException::class.java) {
            S3MediaProtocol.decode(message)
        }
    }

    @Test
    fun severityMappingMatchesConfirmedPhoneAlertPolicy() {
        assertEquals(0, alertTierForSeverity(0))
        assertEquals(1, alertTierForSeverity(1))
        assertEquals(1, alertTierForSeverity(2))
        assertEquals(2, alertTierForSeverity(3))
        assertEquals(2, alertTierForSeverity(4))
        assertEquals(3, alertTierForSeverity(5))
    }

    @Test
    fun storyTransferSummaryParsesCompleteGatewayHealthAndPrefersStoryActive() {
        val snapshot = MobileProtocolParser.parseSnapshot(
            """{
              "telemetry":{"health":{"gateway":{
                "story_active":"0",
                "story_exchange_retries":7,
                "story_exchange_timeouts":3,
                "story_download_error":-1,
                "story_http_status":503,
                "story_received_bytes":1048576,
                "story_media_suspend_failures":2,
                "audio_transport_suspended":"true",
                "video_transport_suspended":1
              }}}
            }"""
        )

        assertEquals(
            StoryTransferSummary(
                active = false,
                exchangeRetries = 7L,
                exchangeTimeouts = 3L,
                downloadError = -1,
                httpStatus = 503,
                receivedBytes = 1_048_576L,
                mediaSuspendFailures = 2L,
                audioTransportSuspended = true,
                videoTransportSuspended = true,
            ),
            snapshot.storyTransfer,
        )
    }

    @Test
    fun storyTransferSummaryDefaultsMissingFieldsAndFallsBackForLegacySnapshot() {
        val legacy = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"health":{"gateway":{"video_transport_suspended":"1"}}}}"""
        ).storyTransfer

        assertEquals(true, legacy.active)
        assertEquals(0L, legacy.exchangeRetries)
        assertEquals(0L, legacy.exchangeTimeouts)
        assertEquals(0, legacy.downloadError)
        assertEquals(0, legacy.httpStatus)
        assertEquals(0L, legacy.receivedBytes)
        assertEquals(0L, legacy.mediaSuspendFailures)
        assertEquals(false, legacy.audioTransportSuspended)
        assertEquals(true, legacy.videoTransportSuspended)

        val absent = MobileProtocolParser.parseSnapshot("""{"telemetry":{}}""").storyTransfer
        assertEquals(StoryTransferSummary(), absent)
    }

    @Test
    fun matrixFrameKeepsServerShapeUnitAndValuesForUiRendering() {
        val matrix = MatrixFrame(
            kind = "thermal",
            sequence = 42,
            rows = 2,
            cols = 3,
            unit = "cdeg",
            receivedAt = "2026-07-26T10:00:00Z",
            values = listOf(2500, 2600, 2700, 2800, 2900, 3000),
        )

        assertEquals("thermal", matrix.kind)
        assertEquals(42L, matrix.sequence)
        assertEquals(2, matrix.rows)
        assertEquals(3, matrix.cols)
        assertEquals("cdeg", matrix.unit)
        assertEquals(listOf(2500, 2600, 2700, 2800, 2900, 3000), matrix.values)
        assertEquals(6, matrix.activePoints)
    }

    @Test
    fun normalizedFusionV5ParsesAllExplainableEvidence() {
        val snapshot = MobileProtocolParser.parseSnapshot(
            """{
              "telemetry":{"inference":{"state":12,"faults":7}},
              "fusion":{
                "schema_version":5,"evidence_sequence":91,
                "legacy":{"state":12,"label":"air_quality","confidence":83,"severity":2},
                "contexts":{
                  "presence":{"code":1,"label":"present"},
                  "airway_view":{"code":2,"label":"partial"}
                },
                "alarm":{"level":2,"label":"L2","primary_type":15,
                  "primary_label":"environment_air","primary_incident_id":77,
                  "primary_lifecycle":3,"active_count":1},
                "active_incidents":[{"type":15,"type_label":"environment_air",
                  "lifecycle":3,"lifecycle_label":"escalated","level":2,
                  "severity":3,"confidence":84,"source_mask":24,
                  "incident_id":77,"since_tick":123456}],
                "evidence":{
                  "available":true,
                  "presence_score_x1000":901,"presence_source_count":3,
                  "pressure_presence_x1000":812,"vision_presence_x1000":723,
                  "thermal_presence_x1000":634,"activity_score_x1000":545,
                  "airway_risk_x1000":456,"edge_risk_x1000":367,
                  "thermal_hotspot":{"points":29,"x_x1000":278,"y_x1000":189,
                    "mean_cdeg":-325},
                  "bed_temperature":{"left_milli_c":-500,"center_milli_c":25125,
                    "right_milli_c":26375,"spread_milli_c":26875,"hotspot_region":3}
                }
              }
            }""".trimIndent(),
        )

        val fusion = snapshot.fusion
        assertEquals(5, fusion.schemaVersion)
        assertEquals(91L, fusion.evidenceSequence)
        assertEquals(12, fusion.legacyState)
        assertEquals("air_quality", fusion.legacyLabel)
        assertEquals(83, fusion.legacyConfidence)
        assertEquals(2, fusion.legacySeverity)
        assertEquals("present", fusion.contexts.getValue("presence").label)
        assertEquals(2, fusion.alarm.level)
        assertEquals("environment_air", fusion.alarm.primaryLabel)
        assertEquals(77L, fusion.alarm.primaryIncidentId)
        assertEquals(77L, fusion.activeIncidents.single().incidentId)
        assertEquals("escalated", fusion.activeIncidents.single().lifecycleLabel)

        val evidence = fusion.evidence
        assertEquals(true, evidence.available)
        assertEquals(901, evidence.presenceScoreX1000)
        assertEquals(3, evidence.presenceSourceCount)
        assertEquals(812, evidence.pressurePresenceX1000)
        assertEquals(723, evidence.visionPresenceX1000)
        assertEquals(634, evidence.thermalPresenceX1000)
        assertEquals(545, evidence.activityScoreX1000)
        assertEquals(456, evidence.airwayRiskX1000)
        assertEquals(367, evidence.edgeRiskX1000)
        assertEquals(29, evidence.thermalHotspot.points)
        assertEquals(278, evidence.thermalHotspot.xX1000)
        assertEquals(189, evidence.thermalHotspot.yX1000)
        assertEquals(-32.5f, evidence.thermalHotspot.meanC)
        assertEquals(-0.5f, evidence.bedTemperature.leftC)
        assertEquals(25.125f, evidence.bedTemperature.centerC)
        assertEquals(26.375f, evidence.bedTemperature.rightC)
        assertEquals(26.875f, evidence.bedTemperature.spreadC)
        assertEquals(3, evidence.bedTemperature.hotspotRegion)
        assertEquals("右侧", evidence.bedTemperature.hotspotRegionLabel)
        assertEquals(7, snapshot.inference.faults)
    }

    @Test
    fun fusionSourceValidityMaskOverridesEvidenceAndFallsBackWhenMissing() {
        fun parse(validField: String): FusionSummary = MobileProtocolParser.parseSnapshot(
            """{
              "telemetry":{"inference":{"validField"}},
              "fusion":{"schema_version":5,"evidence":{"available":true}}
            }""".replace("\"validField\"", validField),
        ).fusion

        val selective = parse("\"valid\":10")
        assertEquals(10, selective.validSourceMask)
        assertEquals(true, selective.thermalEvidenceAvailable)
        assertEquals(false, selective.bedTemperatureEvidenceAvailable)
        assertEquals(true, selective.pressureEvidenceAvailable)
        assertEquals(false, selective.visionEvidenceAvailable)

        val none = parse("\"valid\":0")
        assertEquals(false, none.thermalEvidenceAvailable)
        assertEquals(false, none.bedTemperatureEvidenceAvailable)
        assertEquals(false, none.pressureEvidenceAvailable)
        assertEquals(false, none.visionEvidenceAvailable)

        val backwardCompatible = parse("")
        assertEquals(null, backwardCompatible.validSourceMask)
        assertEquals(true, backwardCompatible.thermalEvidenceAvailable)
        assertEquals(true, backwardCompatible.bedTemperatureEvidenceAvailable)
        assertEquals(true, backwardCompatible.pressureEvidenceAvailable)
        assertEquals(true, backwardCompatible.visionEvidenceAvailable)
    }

    @Test
    fun rawFusionEvidenceFallbackRequiresSeventeenValuesAndClampsScores() {
        val available = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"inference":{
              "sv":5,"ev":42,"state":8,"conf":79,"severity":3,
              "ctx":[1,2],"alarm":[2,8,123,2,1,256],
              "ai":[[8,2,2,3,79,24,123,9000]],
              "ex":[1100,5,-1,500,900,1001,44,88,15,-5,1200,-325,
                    -500,25000,26000,26500,3]
            }}}""".trimIndent(),
        ).fusion

        assertEquals(5, available.schemaVersion)
        assertEquals("present", available.contexts.getValue("presence").label)
        assertEquals("edge_climbing", available.alarm.primaryLabel)
        assertEquals(123L, available.activeIncidents.single().incidentId)
        assertEquals(true, available.evidence.available)
        assertEquals(1000, available.evidence.presenceScoreX1000)
        assertEquals(3, available.evidence.presenceSourceCount)
        assertEquals(0, available.evidence.pressurePresenceX1000)
        assertEquals(1000, available.evidence.activityScoreX1000)
        assertEquals(0, available.evidence.thermalHotspot.xX1000)
        assertEquals(1000, available.evidence.thermalHotspot.yX1000)
        assertEquals(-32.5f, available.evidence.thermalHotspot.meanC)
        assertEquals(-0.5f, available.evidence.bedTemperature.leftC)

        val unavailable = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"inference":{"sv":5,"ex":[1,2,3]}},
              "fusion":{"schema_version":5,"evidence":{"available":false,
                "presence_score_x1000":999,
                "thermal_hotspot":{"mean_cdeg":2500},
                "bed_temperature":{"left_milli_c":25000}}}}""",
        ).fusion.evidence
        assertEquals(false, unavailable.available)
        assertEquals(0, unavailable.presenceScoreX1000)
        assertEquals(null, unavailable.thermalHotspot.meanC)
        assertEquals(null, unavailable.bedTemperature.leftC)
    }

    @Test
    fun schemaV5AirQualityAndNestedEventLifecycleAreParsed() {
        assertEquals("空气质量需关注", safetyStateLabel(12))
        assertEquals("空气质量异常", incidentTypeLabel(15))

        val nested = MobileProtocolParser.parseEvent(
            """{"data":{"id":41,"source_event_id":9,"event_type":"ra8_safety",
              "state":"12","severity":2,"confidence":82,"received_at":"now",
              "payload_json":{"event_id":9,"payload":{"incident":77,"type":15,
                "action":2,"lifecycle":4,"level":1,"global_level":2}}}}""",
        )
        assertEquals(77L, nested.incidentId)
        assertEquals(15, nested.type)
        assertEquals(2, nested.action)
        assertEquals(4, nested.lifecycle)
        assertEquals(1, nested.level)
        assertEquals(2, nested.globalLevel)
        assertEquals("空气质量异常", nested.title)

        val direct = MobileProtocolParser.parseEvent(
            """{"id":42,"state":12,"payload":{"incident":78,"type":15,
              "action":1,"lifecycle":2,"level":2,"global_level":2}}""",
        )
        assertEquals(78L, direct.incidentId)
        assertEquals(1, direct.action)
    }

    @Test
    fun rawFusionV8ParsesSuspectedFeverAndDeciCelsiusHotspot() {
        assertEquals("疑似发热", safetyStateLabel(13))
        assertEquals("疑似发热", incidentTypeLabel(16))

        val fusion = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"inference":{
              "sv":8,"ev":108,"state":13,"conf":91,"severity":3,
              "ctx":[1,0,0,1,1,1,0,2,1,1,0],
              "alarm":[2,16,901,2,1,0],
              "ai":[[16,2,2,3,91,2,901,123456]],
              "ex":[900,3,850,800,750,100,0,0,12,500,400,375,
                    25100,25200,25300,200,2]
            }}}""".trimIndent(),
        ).fusion

        assertEquals(8, fusion.schemaVersion)
        assertEquals(13, fusion.legacyState)
        assertEquals("suspected_fever", fusion.legacyLabel)
        assertEquals("high", fusion.contexts.getValue("body_temperature").label)
        assertEquals(16, fusion.alarm.primaryType)
        assertEquals("suspected_fever", fusion.alarm.primaryLabel)
        assertEquals("suspected_fever", fusion.activeIncidents.single().typeLabel)
        assertEquals(37.5f, fusion.evidence.thermalHotspot.meanC)
    }

    @Test
    fun cryPercentageUsesDirectPercentAndRawTenthsOnlyAsFallback() {
        val direct = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"inference":{"cry":1,"cry_raw":900}}}""",
        )
        val raw = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"inference":{"cry_raw":29}}}""",
        )

        assertEquals(1f, direct.inference.cryConfidence)
        assertEquals(2.9f, raw.inference.cryConfidence)
    }

    @Test
    fun relayActuatorsParseReadyStatusAndNumericBooleans() {
        val snapshot = MobileProtocolParser.parseSnapshot(
            """{"telemetry":{"sensors":{
              "warm_light":{"s":1,"on":1,"pin":711},
              "soother":{"s":0,"on":0,"pin":914}
            }}}""",
        )

        assertEquals(1, snapshot.warmLight.status)
        assertEquals(true, snapshot.warmLight.ready)
        assertEquals(true, snapshot.warmLight.on)
        assertEquals(711, snapshot.warmLight.pin)
        assertEquals(0, snapshot.soother.status)
        assertEquals(false, snapshot.soother.ready)
        assertEquals(false, snapshot.soother.on)
        assertEquals(914, snapshot.soother.pin)
    }
}
