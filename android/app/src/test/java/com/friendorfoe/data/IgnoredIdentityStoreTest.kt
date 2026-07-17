package com.friendorfoe.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class IgnoredIdentityStoreTest {

    @Test
    fun legacy_csv_migrates_and_structured_identities_survive_store_restart() {
        var structured: Set<String>? = null
        var legacyCsv: String? = "AA:BB:CC:DD:EE:FF,mac:11:22:33:44:55:66"

        fun newStore() = IgnoredIdentityStore(
            readStructured = { structured },
            readLegacyCsv = { legacyCsv },
            persistStructured = { identities ->
                structured = identities.toSet()
                legacyCsv = null
            },
        )

        val firstStore = newStore()
        assertEquals(
            setOf(
                "mac:aa:bb:cc:dd:ee:ff",
                "mac:11:22:33:44:55:66",
            ),
            firstStore.get(),
        )
        assertNull(legacyCsv)

        firstStore.add(
            setOf(
                "FP:Meta|Ray-Ban,Wayfarer",
                "mac:AA:BB:CC:DD:EE:00",
            )
        )

        val restartedStore = newStore()
        assertTrue("fp:meta|ray-ban,wayfarer" in restartedStore.get())
        assertTrue("mac:aa:bb:cc:dd:ee:00" in restartedStore.get())
    }
}
