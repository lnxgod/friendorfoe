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
    version = 3,
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

        fun create(context: Context): FriendOrFoeDatabase {
            return Room.databaseBuilder(
                context.applicationContext,
                FriendOrFoeDatabase::class.java,
                DATABASE_NAME
            )
                .addMigrations(MIGRATION_2_3)
                .build()
        }
    }
}
