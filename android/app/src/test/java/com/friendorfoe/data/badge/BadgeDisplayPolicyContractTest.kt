package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeDisplayPolicyContractTest {

    @Test
    fun defaultsContainExactThirteenClassesAndHash() {
        val policy = BadgeDisplayPolicy.firmwareDefaults()

        assertEquals(13, policy.classes.size)
        assertEquals(100, policy.classes.getValue("drone").priority)
        assertEquals(BadgeDisplayLane.BOTH, policy.classes.getValue("drone").lane)
        assertEquals(
            BadgeMinimumProximity.CLOSE,
            policy.classes.getValue("hid").minProximity
        )
        assertEquals(0x0DAD6299L, policy.firmwareHash())
    }

    @Test
    fun disableAndReenablePreserveStoredPriority() {
        val defaults = BadgeDisplayPolicy.firmwareDefaults()
        val original = defaults.copy(
            classes = defaults.classes + (
                "tracker" to defaults.classes.getValue("tracker").copy(priority = 42)
            )
        )

        val disabled = original.withEnabled("tracker", false)
        val enabled = disabled.withEnabled("tracker", true)

        assertFalse(disabled.classes.getValue("tracker").enabled)
        assertEquals(BadgeDisplayLane.OFF, disabled.classes.getValue("tracker").lane)
        assertTrue(enabled.classes.getValue("tracker").enabled)
        assertEquals(42, enabled.classes.getValue("tracker").priority)
    }

    @Test
    fun invalidEnabledOffLaneAndUnknownClassSetAreRejected() {
        val defaults = BadgeDisplayPolicy.firmwareDefaults()
        val enabledOff = defaults.copy(
            classes = defaults.classes + (
                "drone" to defaults.classes.getValue("drone").copy(
                    enabled = true,
                    lane = BadgeDisplayLane.OFF
                )
            )
        )
        val missingClass = defaults.copy(classes = defaults.classes - "auracast")

        assertTrue(BadgeDisplayPolicy.validate(enabledOff).isFailure)
        assertTrue(BadgeDisplayPolicy.validate(missingClass).isFailure)
    }

    @Test
    fun hashAndSerializationUseCanonicalClassOrder() {
        val defaults = BadgeDisplayPolicy.firmwareDefaults()
        val reordered = defaults.copy(
            classes = defaults.classes.entries.reversed().associate { it.toPair() }
        )

        assertEquals(0x0DAD6299L, reordered.firmwareHash())
        assertEquals(
            BadgeDisplayPolicy.classOrder,
            reordered.toJsonObject()["classes"].asJsonObject.keySet().toList()
        )
    }
}
