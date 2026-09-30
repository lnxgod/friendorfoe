package com.friendorfoe.data.local

import android.content.Context
import androidx.room.testing.MigrationTestHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class FriendOrFoeDatabaseMigrationTest {

    @get:Rule
    val helper = MigrationTestHelper(
        InstrumentationRegistry.getInstrumentation(),
        FriendOrFoeDatabase::class.java,
    )

    private val context: Context = ApplicationProvider.getApplicationContext()

    @Before
    fun deleteDatabaseBeforeTest() {
        context.deleteDatabase(TEST_DATABASE)
    }

    @After
    fun deleteDatabaseAfterTest() {
        context.deleteDatabase(TEST_DATABASE)
    }

    @Test
    fun migration_4_to_5_preserves_rows_and_adds_nullable_radio_columns() {
        helper.createDatabase(TEST_DATABASE, 4).apply {
            execSQL(
                """
                INSERT INTO detection_history (
                    object_id, object_type, detection_source, category,
                    display_name, description, latitude, longitude,
                    altitude_meters, user_latitude, user_longitude,
                    distance_meters, confidence, first_seen, last_seen, photo_url
                ) VALUES (
                    'wifi_legacy', 'drone', 'wifi', 'drone',
                    'DJI', 'legacy row', 42.4347, -83.9850,
                    201.0, 42.0, -83.0,
                    19.5, 0.3, 1000, 2000, NULL
                )
                """.trimIndent(),
            )
            close()
        }

        val migrated = helper.runMigrationsAndValidate(
            TEST_DATABASE,
            5,
            true,
            FriendOrFoeDatabase.MIGRATION_4_5,
        )

        migrated.query(
            """
            SELECT object_id, display_name, ssid, bssid, signal_strength_dbm,
                   frequency_mhz, channel_width_mhz
            FROM detection_history
            WHERE object_id = 'wifi_legacy'
            """.trimIndent(),
        ).use { cursor ->
            assertTrue(cursor.moveToFirst())
            assertEquals("wifi_legacy", cursor.getString(cursor.getColumnIndexOrThrow("object_id")))
            assertEquals("DJI", cursor.getString(cursor.getColumnIndexOrThrow("display_name")))
            assertNullColumn(cursor, "ssid")
            assertNullColumn(cursor, "bssid")
            assertNullColumn(cursor, "signal_strength_dbm")
            assertNullColumn(cursor, "frequency_mhz")
            assertNullColumn(cursor, "channel_width_mhz")
        }
        migrated.close()
    }

    @Test
    fun migration_5_to_6_keeps_existing_trail_and_adds_object_time_index() {
        helper.createDatabase(TEST_DATABASE, 5).apply {
            execSQL("INSERT INTO position_tracking (object_id, latitude, longitude, altitude_meters, heading, speed_mps, timestamp) VALUES ('abc123', 32.7, -117.1, 1200, 90, 100, 1000)")
            close()
        }
        val migrated = helper.runMigrationsAndValidate(TEST_DATABASE, 6, true, FriendOrFoeDatabase.MIGRATION_5_6)
        migrated.query("SELECT object_id, altitude_meters FROM position_tracking").use {
            assertTrue(it.moveToFirst())
            assertEquals("abc123", it.getString(0))
            assertEquals(1200.0, it.getDouble(1), 0.0)
        }
        migrated.query("SELECT name FROM sqlite_master WHERE type='index' AND name='index_position_tracking_object_id_timestamp'").use {
            assertTrue(it.moveToFirst())
        }
        migrated.close()
    }

    @Test
    fun migration_6_to_7_keeps_live_trails_and_creates_independent_saved_flights() {
        helper.createDatabase(TEST_DATABASE, 6).apply {
            execSQL("INSERT INTO position_tracking (object_id, latitude, longitude, altitude_meters, heading, speed_mps, timestamp) VALUES ('abc123', 32.7, -117.1, 1200, 90, 100, 1000)")
            close()
        }
        val migrated = helper.runMigrationsAndValidate(TEST_DATABASE, 7, true, FriendOrFoeDatabase.MIGRATION_6_7)
        migrated.query("SELECT object_id FROM position_tracking").use {
            assertTrue(it.moveToFirst()); assertEquals("abc123", it.getString(0))
        }
        migrated.execSQL("INSERT INTO saved_flights VALUES ('saved', 'abc123', 'N123', 1000, 1, '[]')")
        migrated.execSQL("DELETE FROM position_tracking")
        migrated.query("SELECT label FROM saved_flights").use {
            assertTrue(it.moveToFirst()); assertEquals("N123", it.getString(0))
        }
        migrated.close()
    }

    private fun assertNullColumn(databaseCursor: android.database.Cursor, columnName: String) {
        assertTrue(databaseCursor.isNull(databaseCursor.getColumnIndexOrThrow(columnName)))
    }

    companion object {
        private const val TEST_DATABASE = "friendorfoe-migration-test"
    }
}
