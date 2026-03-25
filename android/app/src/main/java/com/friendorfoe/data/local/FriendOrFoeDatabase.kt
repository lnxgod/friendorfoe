package com.friendorfoe.data.local

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase

/**
 * Room database for Friend or Foe local data storage.
 *
 * Currently stores detection history. Future versions may cache
 * aircraft metadata for offline use.
 */
@Database(
    entities = [HistoryEntity::class, TrackingEntity::class],
    version = 4,
    exportSchema = true
)
abstract class FriendOrFoeDatabase : RoomDatabase() {

    abstract fun historyDao(): HistoryDao
    abstract fun trackingDao(): TrackingDao

    companion object {
        private const val DATABASE_NAME = "friendorfoe.db"

        /** Migration from v2 to v3: no schema changes, preserves data. */
        private val MIGRATION_2_3 = object : Migration(2, 3) {
            override fun migrate(db: SupportSQLiteDatabase) {
                // No schema changes — this migration exists to avoid destructive fallback
            }
        }

        /** Migration from v3 to v4: add indices for query performance. */
        private val MIGRATION_3_4 = object : Migration(3, 4) {
            override fun migrate(db: SupportSQLiteDatabase) {
                db.execSQL("CREATE INDEX IF NOT EXISTS `index_detection_history_object_id` ON `detection_history` (`object_id`)")
                db.execSQL("CREATE INDEX IF NOT EXISTS `index_detection_history_object_type` ON `detection_history` (`object_type`)")
                db.execSQL("CREATE INDEX IF NOT EXISTS `index_detection_history_last_seen` ON `detection_history` (`last_seen`)")
                db.execSQL("CREATE INDEX IF NOT EXISTS `index_position_tracking_object_id` ON `position_tracking` (`object_id`)")
                db.execSQL("CREATE INDEX IF NOT EXISTS `index_position_tracking_timestamp` ON `position_tracking` (`timestamp`)")
            }
        }

        fun create(context: Context): FriendOrFoeDatabase {
            return Room.databaseBuilder(
                context.applicationContext,
                FriendOrFoeDatabase::class.java,
                DATABASE_NAME
            )
                .addMigrations(MIGRATION_2_3, MIGRATION_3_4)
                .build()
        }
    }
}
