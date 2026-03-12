package com.friendorfoe.data.local

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

/**
 * Room database for Friend or Foe local data storage.
 *
 * Currently stores detection history. Future versions may cache
 * aircraft metadata for offline use.
 */
@Database(
    entities = [HistoryEntity::class],
    version = 1,
    exportSchema = true
)
abstract class FriendOrFoeDatabase : RoomDatabase() {

    abstract fun historyDao(): HistoryDao

    companion object {
        private const val DATABASE_NAME = "friendorfoe.db"

        fun create(context: Context): FriendOrFoeDatabase {
            return Room.databaseBuilder(
                context.applicationContext,
                FriendOrFoeDatabase::class.java,
                DATABASE_NAME
            )
                .fallbackToDestructiveMigration()
                .build()
        }
    }
}
