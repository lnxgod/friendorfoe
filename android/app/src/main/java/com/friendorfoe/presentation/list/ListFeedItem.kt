package com.friendorfoe.presentation.list

import com.friendorfoe.data.badge.BadgeUsbDetection
import com.friendorfoe.data.badge.stableKey
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.SkyObject

enum class ListSourceMarker { BADGE, ADS_B, REMOTE_ID, WIFI }

sealed interface ListFeedItem {
    val key: String
    val sourceMarker: ListSourceMarker

    data class Sky(val value: SkyObject) : ListFeedItem {
        override val key = "sky:${value.id}"
        override val sourceMarker = when (value.source) {
            DetectionSource.ADS_B -> ListSourceMarker.ADS_B
            DetectionSource.REMOTE_ID -> ListSourceMarker.REMOTE_ID
            DetectionSource.WIFI_NAN,
            DetectionSource.WIFI_BEACON,
            DetectionSource.WIFI -> ListSourceMarker.WIFI
        }
    }

    data class Badge(val detection: BadgeUsbDetection) : ListFeedItem {
        override val key = "badge:${detection.stableKey}"
        override val sourceMarker = ListSourceMarker.BADGE
    }
}

internal fun mergeListFeed(
    skyObjects: List<SkyObject>,
    badgeDetections: List<BadgeUsbDetection>,
): List<ListFeedItem> = badgeDetections
    .sortedByDescending { it.receivedAtElapsedMs }
    .distinctBy { it.stableKey }
    .map { ListFeedItem.Badge(it) } + skyObjects.map { ListFeedItem.Sky(it) }
