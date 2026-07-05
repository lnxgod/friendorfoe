package com.friendorfoe.detection

import com.friendorfoe.domain.model.ObjectCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MilitaryClassifierTest {

    @Test
    fun readsbMilitaryDbFlagClassifiesAsMilitary() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = null,
            typeCode = null,
            registration = null,
            dbFlags = 1
        )

        assertEquals(ObjectCategory.MILITARY, result.category)
        assertTrue(result.signals.contains("DBFLAGS:MILITARY"))
    }

    @Test
    fun lawEnforcementCallsignAloneClassifiesAsGovernment() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = "POLICE1",
            typeCode = null,
            registration = null
        )

        assertEquals(ObjectCategory.GOVERNMENT, result.category)
        assertTrue(result.signals.contains("CALLSIGN:LAW_ENFORCEMENT"))
    }

    @Test
    fun ownerNameClassifiesPublicSafetyAircraftAsGovernment() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = null,
            typeCode = null,
            registration = "N12345",
            ownerName = "SAN DIEGO COUNTY SHERIFF"
        )

        assertEquals(ObjectCategory.GOVERNMENT, result.category)
        assertTrue(result.signals.contains("OWNER:PUBLIC_SAFETY"))
    }
}
