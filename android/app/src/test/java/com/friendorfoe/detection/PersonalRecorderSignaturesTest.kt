package com.friendorfoe.detection

import org.junit.Assert.*
import org.junit.Test

class PersonalRecorderSignaturesTest {
    @Test fun `recorder names use boundaries and ignore case`() {
        val examples = mapOf("PLAUDAB12" to "Plaud", "PLAUD NOTE" to "Plaud", "Plaud Note Pro" to "Plaud",
            "NotePin_123" to "Plaud", "omi" to "Omi", "Limitless Pendant" to "Limitless",
            "Bee_AB12" to "Bee", "friend_123" to "Friend", "Fieldy" to "Fieldy")
        for ((name, brand) in examples) {
            assertEquals(name, brand, PersonalRecorderSignatures.match(name)?.manufacturer)
        }
        assertEquals(PrivacyCategory.VOICE_RECORDER, GlassesDetector.categorizeDeviceType("AI Voice Recorder"))
    }

    @Test fun `unrelated names and shared UUIDs do not match`() {
        for (name in listOf("Pebblebee", "Beech", "Beeline", "Friend", "Friendly Speaker",
            "Pendant", "Compass", "My PLAUD phone", "Plaudify", "Fieldyard", "OMIRON")) {
            assertNull(name, PersonalRecorderSignatures.match(name))
        }
        for (uuid in listOf("00001910-0000-1000-8000-00805f9b34fb",
            "4fafc201-1fb5-459e-8fcc-c5c9c331914b", "0000180f-0000-1000-8000-00805f9b34fb")) {
            assertNull(PersonalRecorderSignatures.match(null, listOf(uuid)))
        }
    }

    @Test fun `custom services detect unnamed devices and beat mutable names`() {
        val examples = mapOf(
            "19b10000-e8f2-537e-4f6c-d104768a1214" to "Omi",
            "632de001-604c-446b-a80f-7963e950f3fb" to "Limitless",
            "03d5d5c4-a86c-11ee-9d89-8f2089a49e7e" to "Bee",
            "1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da" to "Friend")
        for ((uuid, brand) in examples) {
            assertEquals(brand, PersonalRecorderSignatures.match(null, listOf(uuid.uppercase()))?.manufacturer)
            assertEquals(brand, PersonalRecorderSignatures.match("PLAUD NOTE", listOf(uuid))?.manufacturer)
            assertNull(PersonalRecorderSignatures.match(null, listOf(uuid.dropLast(1) + "0")))
        }
    }
}
