package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.preferences.FindingPreferenceKey
import org.junit.Assert.assertEquals
import org.junit.Test

class IgnoredFindingRowsTest {
    @Test
    fun rowsAreDecodedSortedAndKeepSourceIdentityVisible() {
        val rows = ignoredFindingRows(
            setOf(
                requireNotNull(FindingPreferenceKey.create("phone_ble", "fp:two")).encoded,
                requireNotNull(FindingPreferenceKey.create("backend", "entity:one")).encoded,
                "malformed",
            ),
        )

        assertEquals(listOf("Backend", "Phone Bluetooth"), rows.map { it.sourceLabel })
        assertEquals(listOf("entity:one", "fp:two"), rows.map { it.stableId })
    }
}
