package com.friendorfoe.data

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Test

class AircraftRangePreferenceTest {
    @Test fun groupedViewPreferencePersistsAcrossPreferenceInstances() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val prefs = DetectionPrefs(context)
        val storage = context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)
        val key = "list_group_aircraft_by_type"
        val original = storage.all[key] as Boolean?
        try {
            prefs.groupAircraftByType = true
            assertEquals(true, DetectionPrefs(context).groupAircraftByType)
            assertEquals(true, prefs.settings.value.groupAircraftByType)
            prefs.groupAircraftByType = false
            assertEquals(false, DetectionPrefs(context).groupAircraftByType)
        } finally {
            val editor = storage.edit()
            if (original == null) editor.remove(key) else editor.putBoolean(key, original)
            editor.commit()
        }
    }

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
