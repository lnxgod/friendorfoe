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
    fun militaryTransportCallsignAloneClassifiesAsMilitary() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = "RCH123",
            typeCode = null,
            registration = null
        )

        assertEquals(ObjectCategory.MILITARY, result.category)
        assertTrue(result.signals.contains("CALLSIGN:REACH_AMC"))
    }

    @Test
    fun exclusiveMilitaryTypeCodeAloneClassifiesAsMilitary() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = null,
            typeCode = "C17",
            registration = null
        )

        assertEquals(ObjectCategory.MILITARY, result.category)
        assertTrue(result.signals.contains("TYPE:C17"))
    }

    @Test
    fun dualUseMilitaryTypeCodeNeedsAnotherSignal() {
        val weakResult = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = null,
            typeCode = "C130",
            registration = null
        )
        val combinedResult = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = "HAWK1",
            typeCode = "C130",
            registration = null
        )

        assertEquals(null, weakResult.category)
        assertEquals(ObjectCategory.MILITARY, combinedResult.category)
        assertTrue(combinedResult.signals.contains("CALLSIGN:HAWK"))
        assertTrue(combinedResult.signals.contains("TYPE:C130"))
    }

    @Test
    fun emergencyPublicSafetyCallsignClassifiesAsGovernment() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = "CALFIRE12",
            typeCode = null,
            registration = null
        )

        assertEquals(ObjectCategory.GOVERNMENT, result.category)
        assertTrue(result.signals.contains("CALLSIGN:PUBLIC_SAFETY"))
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

    @Test
    fun ownerNameClassifiesMilitaryAircraftAsMilitary() {
        val result = MilitaryClassifier.classify(
            icaoHex = null,
            callsign = null,
            typeCode = null,
            registration = null,
            ownerName = "UNITED STATES AIR FORCE"
        )

        assertEquals(ObjectCategory.MILITARY, result.category)
        assertTrue(result.signals.contains("OWNER:USAF"))
    }
}
