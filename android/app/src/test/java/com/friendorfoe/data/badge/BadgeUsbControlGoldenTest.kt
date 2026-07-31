package com.friendorfoe.data.badge

import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationRequest
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationTarget
import com.friendorfoe.detection.PrivacyDetectionOrigin
import com.google.gson.JsonParser
import java.io.File
import java.nio.charset.StandardCharsets
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbControlGoldenTest {

    @Test
    fun `every Android badge control matches the shared exact firmware fixture`() {
        val fixture = loadStrictFixture()
        val actual = linkedMapOf(
            "set_mode_local_ap" to badgeSetModeCommandJson("local_ap"),
            "set_mode_backend" to badgeSetModeCommandJson("backend"),
            "set_mode_usb_only" to badgeSetModeCommandJson("usb_only"),
            "reboot" to badgeRebootCommandJson(),
            "display_policy" to badgeDisplayPolicyCommandJson(
                defaultBadgeDisplayPolicy(),
                persist = true,
            ),
            "display_policy_reset" to badgeDisplayPolicyResetCommandJson(
                persist = true,
            ),
            "theme" to badgeThemeCommandJson(defaultBadgeTheme(), persist = true),
            "theme_reset" to badgeThemeResetCommandJson(persist = true),
            "display_nav_next" to badgeDisplayNavCommandJson(BadgeDisplayNavAction.NEXT),
            "display_nav_detail" to badgeDisplayNavCommandJson(BadgeDisplayNavAction.DETAIL),
            "display_nav_page" to badgeDisplayNavCommandJson(BadgeDisplayNavAction.PAGE),
            "display_nav_back" to badgeDisplayNavCommandJson(BadgeDisplayNavAction.BACK),
            "ble_investigate_gatt" to badgeInvestigationCommandJson(
                investigationRequest(
                    requestId = "android-gatt",
                    mode = BleInvestigationMode.GATT,
                    // Accepted mixed/lowercase input must be canonical on wire.
                    mac = "aa:bb:cc:dd:ee:ff",
                    timeoutMs = 7_500,
                ),
            ),
            "ble_investigate_passive" to badgeInvestigationCommandJson(
                investigationRequest(
                    requestId = "android-passive",
                    mode = BleInvestigationMode.PASSIVE_CAPTURE,
                    mac = null,
                    timeoutMs = 12_000,
                ),
            ),
            "ble_chunk_first" to badgeBleInvestigationChunkCommandJson(
                requestId = "android-gatt",
                seq = 0,
            ),
            "ble_chunk_last" to badgeBleInvestigationChunkCommandJson(
                requestId = "android-gatt",
                seq = 63,
            ),
        )

        assertEquals(16, fixture.size)
        assertEquals(fixture.keys, actual.keys)
        actual.forEach { (name, command) ->
            assertNotNull("$name must serialize", command)
            assertEquals(name, fixture.getValue(name), command.toString())
        }
    }

    @Test
    fun `invalid Android control inputs fail closed before a wire shape exists`() {
        assertEquals(null, badgeSetModeCommandJson("Backend"))
        assertEquals(null, badgeSetModeCommandJson(" backend"))
        assertEquals(
            null,
            badgeBleInvestigationChunkCommandJson("android-gatt", -1),
        )
        assertEquals(
            null,
            badgeBleInvestigationChunkCommandJson("android-gatt", 64),
        )
        assertEquals(
            null,
            badgeInvestigationCommandJson(
                investigationRequest(
                    requestId = "bad id",
                    mode = BleInvestigationMode.GATT,
                    mac = "aa:bb:cc:dd:ee:ff",
                    timeoutMs = 7_500,
                ),
            ),
        )
        assertEquals(
            null,
            badgeInvestigationCommandJson(
                investigationRequest(
                    requestId = "missing-target",
                    mode = BleInvestigationMode.GATT,
                    mac = null,
                    timeoutMs = 7_500,
                ),
            ),
        )
        assertEquals(
            null,
            badgeInvestigationCommandJson(
                investigationRequest(
                    requestId = "unexpected-target",
                    mode = BleInvestigationMode.PASSIVE_CAPTURE,
                    mac = "aa:bb:cc:dd:ee:ff",
                    timeoutMs = 7_500,
                ),
            ),
        )
    }

    private fun loadStrictFixture(): LinkedHashMap<String, String> {
        val candidates = listOf(
            File("../test/fixtures/android_badge_usb_controls_v1.tsv"),
            File("test/fixtures/android_badge_usb_controls_v1.tsv"),
            File("../../test/fixtures/android_badge_usb_controls_v1.tsv"),
        )
        val file = candidates.firstOrNull(File::isFile)
        assertNotNull("shared Android/firmware fixture not found", file)
        val bytes = file!!.readBytes()
        assertTrue("fixture must end in LF", bytes.isNotEmpty() && bytes.last() == '\n'.code.toByte())
        assertFalse("fixture must not contain CR", bytes.contains('\r'.code.toByte()))
        assertFalse("fixture must not contain NUL", bytes.contains(0))
        val text = String(bytes, StandardCharsets.UTF_8)
        assertTrue(
            "fixture must be valid UTF-8",
            text.toByteArray(StandardCharsets.UTF_8).contentEquals(bytes),
        )

        val result = linkedMapOf<String, String>()
        text.dropLast(1).split('\n').forEachIndexed { index, line ->
            val tab = line.indexOf('\t')
            assertTrue("row ${index + 1} needs one tab", tab > 0 && tab == line.lastIndexOf('\t'))
            val name = line.substring(0, tab)
            val json = line.substring(tab + 1)
            assertTrue("invalid fixture name $name", name.matches(Regex("[a-z0-9_]+")))
            assertTrue("empty JSON on row ${index + 1}", json.isNotEmpty())
            assertFalse("duplicate fixture name $name", result.containsKey(name))
            assertTrue(
                "fixture JSON must not contain ASCII controls",
                json.all { it.code >= 0x20 && it.code != 0x7f },
            )
            val parsed = JsonParser.parseString(json)
            assertTrue("$name must be a JSON object", parsed.isJsonObject)
            assertEquals("$name must already be canonical JSON", json, parsed.toString())
            result[name] = json
        }
        return result
    }

    private fun investigationRequest(
        requestId: String,
        mode: BleInvestigationMode,
        mac: String?,
        timeoutMs: Long,
    ) = BleInvestigationRequest(
        requestId = requestId,
        target = BleInvestigationTarget(
            mode = mode,
            mac = mac,
            entityKey = "fixture:$requestId",
            observedAtElapsedMs = 1,
            origin = PrivacyDetectionOrigin.BADGE,
        ),
        route = BleInvestigationRoute.BADGE,
        timeoutMs = timeoutMs,
    )
}
