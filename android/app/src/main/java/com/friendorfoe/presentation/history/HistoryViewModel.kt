package com.friendorfoe.presentation.history

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import javax.inject.Inject

/**
 * ViewModel for the History screen.
 *
 * Loads all detection history from [HistoryRepository] and groups entries
 * by date for display with sticky headers (Today, Yesterday, or formatted date).
 */
@HiltViewModel
class HistoryViewModel @Inject constructor(
    historyRepository: HistoryRepository
) : ViewModel() {

    companion object {
        private val DATE_FORMATTER = DateTimeFormatter.ofPattern("MMMM d, yyyy", Locale.getDefault())
    }

    /**
     * History entries grouped by date label.
     *
     * Keys are date group labels: "Today", "Yesterday", or a formatted date
     * like "March 10, 2026". Values are lists of [HistoryEntity] within that group,
     * ordered by most recent first.
     */
    val groupedHistory: StateFlow<Map<String, List<HistoryEntity>>> =
        historyRepository.getAllHistory()
            .map { entries -> groupByDate(entries) }
            .stateIn(
                scope = viewModelScope,
                started = SharingStarted.WhileSubscribed(5000),
                initialValue = emptyMap()
            )

    /**
     * Groups history entries by date, using friendly labels for recent dates.
     */
    private fun groupByDate(entries: List<HistoryEntity>): Map<String, List<HistoryEntity>> {
        if (entries.isEmpty()) return emptyMap()

        val zone = ZoneId.systemDefault()
        val today = LocalDate.now(zone)
        val yesterday = today.minusDays(1)

        // Use LinkedHashMap to preserve insertion order (most recent date first)
        val grouped = linkedMapOf<String, MutableList<HistoryEntity>>()

        for (entry in entries) {
            val entryDate = Instant.ofEpochMilli(entry.lastSeen)
                .atZone(zone)
                .toLocalDate()

            val label = when (entryDate) {
                today -> "Today"
                yesterday -> "Yesterday"
                else -> entryDate.format(DATE_FORMATTER)
            }

            grouped.getOrPut(label) { mutableListOf() }.add(entry)
        }

        return grouped
    }
}
