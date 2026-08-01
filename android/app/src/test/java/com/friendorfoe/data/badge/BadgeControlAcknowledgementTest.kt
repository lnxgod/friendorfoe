package com.friendorfoe.data.badge

import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlAcknowledgementTest {

    @Test
    fun http2xxOkFalseIsFailure() {
        assertEquals(
            BadgeCommandOutcome.Failed("theme save failed"),
            parseHttpCommandOutcome(200, """{"ok":false,"error":"theme save failed"}"""),
        )
    }

    @Test
    fun malformedOrMissingOkHttpBodiesFailClosed() {
        assertTrue(parseHttpCommandOutcome(200, "not-json") is BadgeCommandOutcome.Failed)
        assertTrue(
            parseHttpCommandOutcome(200, "{\"theme_hash\":1}") is BadgeCommandOutcome.Failed,
        )
    }

    @Test
    fun non2xxHttpResponseFailsEvenWithOkBody() {
        assertTrue(
            parseHttpCommandOutcome(503, """{"ok":true}""") is BadgeCommandOutcome.Failed,
        )
    }

    @Test
    fun networkAppliedFalseFailsEvenWhenOkIsTrue() {
        assertTrue(
            parseHttpCommandOutcome(
                200,
                """{"ok":true,"applied":false,"network_mode":"off"}""",
            ) is BadgeCommandOutcome.Failed,
        )
        assertTrue(
            parseUsbControlLine(
                "FOF_CTL_OK:{\"applied\":false,\"network_mode\":\"off\"}",
            ) is BadgeCommandOutcome.Failed,
        )
    }

    @Test
    fun usbErrorBodyIsFailureAndOkHashIsRetained() {
        assertTrue(
            parseUsbControlLine(
                "FOF_CTL_ERROR:{\"error\":\"bad policy\"}",
            ) is BadgeCommandOutcome.Failed,
        )
        val ok = parseUsbControlLine(
            "FOF_CTL_OK:{\"ok\":true,\"theme_hash\":3282709133}",
        ) as BadgeCommandOutcome.Acknowledged
        assertEquals(0xC3AA2A8DL, ok.acknowledgement.themeHash)
    }

    @Test
    fun acknowledgementHashesRejectFractionalScientificNegativeStringAndOverflowForms() {
        listOf("1.5", "1e3", "-1", "4294967296", "\"123\"").forEach { malformed ->
            assertTrue(
                parseHttpCommandOutcome(
                    200,
                    """{"ok":true,"theme_hash":$malformed}""",
                ) is BadgeCommandOutcome.Failed,
            )
            assertTrue(
                parseUsbControlLine(
                    "FOF_CTL_OK:{\"ok\":true,\"display_policy_hash\":$malformed}",
                ) is BadgeCommandOutcome.Failed,
            )
        }
    }

    @Test
    fun presentAcknowledgementFieldsWithWrongTypesOrUnknownValuesFailClosed() {
        listOf(
            """{"ok":true,"applied":"true"}""",
            """{"ok":true,"network_mode":1}""",
            """{"ok":true,"network_mode":"future_mode"}""",
        ).forEach { body ->
            assertTrue(parseHttpCommandOutcome(200, body) is BadgeCommandOutcome.Failed)
        }
        listOf(
            "FOF_CTL_OK:{\"ok\":\"true\"}",
            "FOF_CTL_OK:{\"applied\":1}",
            "FOF_CTL_OK:{\"network_mode\":\"future_mode\"}",
        ).forEach { line ->
            assertTrue(parseUsbControlLine(line) is BadgeCommandOutcome.Failed)
        }
    }

    @Test
    fun usbOnlyPersistenceAcknowledgesRuntimeOff() {
        val ok = parseUsbControlLine(
            "FOF_CTL_OK:{\"ok\":true,\"applied\":true,\"network_mode\":\"off\"}",
        ) as BadgeCommandOutcome.Acknowledged
        assertEquals(true, ok.acknowledgement.networkApplied)
        assertEquals(BadgeRuntimeNetworkMode.OFF, ok.acknowledgement.runtimeNetworkMode)
        assertEquals(
            BadgeRuntimeNetworkMode.OFF,
            BadgeNetworkMode.USB_ONLY.expectedRuntimeMode(),
        )
    }

    @Test
    fun commandClientOutlivesShortPollButLateAckStillTimesOut() {
        val clients = badgeHttpClients(OkHttpClient())
        assertEquals(1_200, clients.status.readTimeoutMillis)
        assertEquals(6_000, clients.command.readTimeoutMillis)
        assertFalse(clients.status.retryOnConnectionFailure)
        assertFalse(clients.command.retryOnConnectionFailure)
        val acknowledged = BadgeCommandOutcome.Acknowledged(
            BadgeControlAcknowledgement("Theme acknowledged", themeHash = 0xC3AA2A8DL),
        )
        assertEquals(acknowledged, enforceAckDeadline(acknowledged, elapsedMs = 5_000))
        assertEquals(
            BadgeCommandOutcome.TimedOut,
            enforceAckDeadline(acknowledged, elapsedMs = 5_001),
        )
    }

    @Test
    fun cachedDebugPostStatusThatPredatesCommandCannotVerifyMutation() {
        assertFalse(
            verifiesDebugPostCommandStatus(
                preSerialPort = "/dev/cu.usbmodem1",
                prePhysicalAtElapsedMs = 1_000,
                sentAtElapsedMs = 3_000,
                postSerialPort = "/dev/cu.usbmodem1",
                postPhysicalAtElapsedMs = 2_000,
                postAndroidReceiptAtElapsedMs = 4_000,
                postLastError = "",
            ),
        )
        assertTrue(
            verifiesDebugPostCommandStatus(
                preSerialPort = "/dev/cu.usbmodem1",
                prePhysicalAtElapsedMs = 1_000,
                sentAtElapsedMs = 3_000,
                postSerialPort = "/dev/cu.usbmodem1",
                postPhysicalAtElapsedMs = 3_000,
                postAndroidReceiptAtElapsedMs = 4_000,
                postLastError = "",
            ),
        )
    }

    @Test
    fun statusAndMutationCallsUseTheirDedicatedClientsExactlyOnce() {
        val statusCalls = AtomicInteger(0)
        val commandCalls = AtomicInteger(0)
        val clients = BadgeHttpClients(
            status = respondingClient(statusCalls, "status"),
            command = respondingClient(commandCalls, "command"),
        )
        val request = Request.Builder().url("http://192.168.4.1/api/badge/status").build()

        assertEquals("status", executeBadgeStatusCall(clients, request).body)
        assertEquals(1, statusCalls.get())
        assertEquals(0, commandCalls.get())

        assertEquals("command", executeBadgeCommandCall(clients, request).body)
        assertEquals(1, statusCalls.get())
        assertEquals(1, commandCalls.get())
    }

    private fun respondingClient(calls: AtomicInteger, body: String): OkHttpClient =
        OkHttpClient.Builder()
            .addInterceptor { chain ->
                calls.incrementAndGet()
                Response.Builder()
                    .request(chain.request())
                    .protocol(Protocol.HTTP_1_1)
                    .code(200)
                    .message("OK")
                    .body(body.toResponseBody())
                    .build()
            }
            .build()
}
