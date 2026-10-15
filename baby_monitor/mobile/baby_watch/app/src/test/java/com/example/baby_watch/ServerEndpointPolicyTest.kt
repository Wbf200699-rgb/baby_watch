package com.example.baby_watch

import com.example.baby_watch.data.server.FIXED_DEVICE_ID
import com.example.baby_watch.data.server.ServerCredentials
import com.example.baby_watch.data.server.ServerEndpointMode
import com.example.baby_watch.data.server.NetworkHealth
import com.example.baby_watch.data.server.NetworkTransport
import com.example.baby_watch.data.server.isAllowedServerUrl
import com.example.baby_watch.data.server.isEndpointNetworkReady
import com.example.baby_watch.data.server.normalizeServerUrl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ServerEndpointPolicyTest {
    @Test
    fun cloudRequiresHttpsAndBasicCredentials() {
        assertTrue(isAllowedServerUrl("https://renesas.zyxserver.online", ServerEndpointMode.Cloud))
        assertFalse(isAllowedServerUrl("http://renesas.zyxserver.online", ServerEndpointMode.Cloud))

        assertFalse(cloudCredentials(username = "", password = "").isComplete)
        assertFalse(cloudCredentials(username = "admin", password = "").isComplete)
        assertTrue(cloudCredentials(username = "admin", password = "secret").isComplete)
    }

    @Test
    fun localHttpAcceptsOnlyLiteralLoopbackAndRfc1918Ipv4() {
        listOf(
            "http://127.0.0.1:8088",
            "http://10.1.2.3:8088",
            "http://172.16.0.1:8088",
            "http://172.31.255.254:8088",
            "http://192.168.137.1:8088",
        ).forEach { url ->
            assertTrue(url, isAllowedServerUrl(url, ServerEndpointMode.Local))
        }

        listOf(
            "http://localhost:8088",
            "http://server.local:8088",
            "http://8.8.8.8:8088",
            "http://172.15.0.1:8088",
            "http://172.32.0.1:8088",
            "http://169.254.1.1:8088",
            "http://192.168.001.1:8088",
            "http://192.168.1.1:0",
            "http://192.168.1.1:8088/api",
            "http://192.168.1.1:8088?redirect=https://example.com",
        ).forEach { url ->
            assertFalse(url, isAllowedServerUrl(url, ServerEndpointMode.Local))
        }
    }

    @Test
    fun localProfileAllowsEmptyAuthentication() {
        val credentials = ServerCredentials(
            baseUrl = "http://192.168.137.1:8088",
            deviceId = FIXED_DEVICE_ID,
            username = "",
            password = "",
            mode = ServerEndpointMode.Local,
        )

        assertTrue(credentials.isComplete)
        assertTrue(credentials.isLocalNetworkEndpoint)
    }

    @Test
    fun localPlanBWorksOnUnvalidatedLanButNeverUsesCellular() {
        val local = ServerCredentials(
            baseUrl = "http://192.168.137.1:8088",
            deviceId = FIXED_DEVICE_ID,
            username = "",
            password = "",
            mode = ServerEndpointMode.Local,
        )
        val isolatedWifi = NetworkHealth(
            available = true,
            validated = false,
            transport = NetworkTransport.Wifi,
        )

        assertTrue(isEndpointNetworkReady(local, isolatedWifi))
        assertTrue(
            isEndpointNetworkReady(
                local,
                isolatedWifi.copy(transport = NetworkTransport.Ethernet),
            )
        )
        assertFalse(
            isEndpointNetworkReady(
                local,
                isolatedWifi.copy(transport = NetworkTransport.Cellular),
            )
        )
    }

    @Test
    fun cloudStillRequiresAndroidValidatedNetwork() {
        val cloud = cloudCredentials(username = "admin", password = "secret")
        val wifi = NetworkHealth(
            available = true,
            validated = false,
            transport = NetworkTransport.Wifi,
        )

        assertFalse(isEndpointNetworkReady(cloud, wifi))
        assertTrue(isEndpointNetworkReady(cloud, wifi.copy(validated = true)))
    }

    @Test
    fun serverUrlNormalizationTrimsWhitespaceAndTrailingSlash() {
        assertEquals(
            "http://192.168.137.1:8088",
            normalizeServerUrl("  HTTP://192.168.137.1:8088///  "),
        )
    }

    private fun cloudCredentials(username: String, password: String) = ServerCredentials(
        baseUrl = "https://renesas.zyxserver.online",
        deviceId = FIXED_DEVICE_ID,
        username = username,
        password = password,
        mode = ServerEndpointMode.Cloud,
    )
}
