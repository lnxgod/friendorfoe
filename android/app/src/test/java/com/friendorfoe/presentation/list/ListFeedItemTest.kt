package com.friendorfoe.presentation.list

import com.friendorfoe.data.badge.BadgeUsbDetection
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertSame
import org.junit.Test

class ListFeedItemTest {

    @Test
    fun `badge rows are deduplicated newest first and keep badge provenance`() {
        val old = badgeDetection(id = "d1", receivedAt = 10)
        val updated = old.copy(rssi = -41, receivedAtElapsedMs = 20)

        val feed = mergeListFeed(emptyList(), listOf(old, updated))

        assertEquals(1, feed.size)
        val row = feed.single() as ListFeedItem.Badge
        assertEquals(-41, row.detection.rssi)
        assertEquals(ListSourceMarker.BADGE, row.sourceMarker)
    }

    @Test
    fun `ordinary sky rows keep their transport source markers and order`() {
        val adsB = aircraft("shared-id", DetectionSource.ADS_B)
        val remoteId = aircraft("remote", DetectionSource.REMOTE_ID)
        val wifi = aircraft("wifi", DetectionSource.WIFI_BEACON)

        val feed = mergeListFeed(listOf(adsB, remoteId, wifi), emptyList())
        val rows = feed.map { it as ListFeedItem.Sky }

        assertEquals(
            listOf(ListSourceMarker.ADS_B, ListSourceMarker.REMOTE_ID, ListSourceMarker.WIFI),
            rows.map { it.sourceMarker },
        )
        assertSame(adsB, rows[0].value)
        assertSame(remoteId, rows[1].value)
        assertSame(wifi, rows[2].value)
    }

    @Test
    fun `badge and sky keys cannot collide when their ids match`() {
        val sky = aircraft("shared-id", DetectionSource.ADS_B)
        val badge = badgeDetection(id = "shared-id", receivedAt = 30)

        val feed = mergeListFeed(listOf(sky), listOf(badge))

        assertEquals(2, feed.size)
        assertNotEquals(feed[0].key, feed[1].key)
        assertEquals("badge:id:shared-id", feed[0].key)
        assertEquals("sky:shared-id", feed[1].key)
    }

    private fun badgeDetection(id: String, receivedAt: Long) = BadgeUsbDetection(
        id = id,
        manufacturer = "DJI",
        source = 0,
        confidence = 0.9f,
        rssi = -50,
        receivedAtElapsedMs = receivedAt,
    )

    private fun aircraft(id: String, source: DetectionSource) = Aircraft(
        id = id,
        position = Position(0.0, 0.0, 1_000.0),
        source = source,
        category = ObjectCategory.COMMERCIAL,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        icaoHex = id,
    )
}
