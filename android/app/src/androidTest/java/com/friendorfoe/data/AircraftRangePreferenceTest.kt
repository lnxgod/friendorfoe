package com.friendorfoe.data

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Test

class AircraftRangePreferenceTest {
    @Test
    fun rangeDefaultsToTenMilesAndPersistsChangesWithLiveSnapshots() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val storage = context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)
        val key = "aircraft_range_miles"
        val original = storage.all[key] as Int?
        try {
            storage.edit().remove(key).commit()
            val prefs = DetectionPrefs(context)
            assertEquals(10, prefs.aircraftRangeMiles)
            assertEquals(10, prefs.settings.value.aircraftRangeMiles)

            for ((requested, expected) in listOf(5 to 5, 15 to 15, 0 to 1, 100 to 50, 10 to 10)) {
                prefs.aircraftRangeMiles = requested
                val snapshot = withTimeout(2_000) {
                    prefs.settings.first { it.aircraftRangeMiles == expected }
                }
                assertEquals(expected, snapshot.aircraftRangeMiles)
                assertEquals(expected, DetectionPrefs(context).aircraftRangeMiles)
                assertEquals(expected, storage.getInt(key, -1))
            }
        } finally {
            val editor = storage.edit()
            if (original == null) editor.remove(key) else editor.putInt(key, original)
            editor.commit()
        }
    }
}
