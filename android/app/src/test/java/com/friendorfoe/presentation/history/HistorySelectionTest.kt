package com.friendorfoe.presentation.history

import com.friendorfoe.data.local.HistoryEntity
import com.friendorfoe.data.repository.HistoryStore
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.detail.DetailLookup
import com.friendorfoe.presentation.detail.DetailSelectionLoader
import com.friendorfoe.presentation.components.CollectionBodyState
import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import java.time.Instant

class HistorySelectionTest {
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun selectedHistoryIdWinsOverNewerRowWithSameObjectId() = runTest {
        val old = history(id = 11L, objectId = "abc123", lastSeen = 100L)
        val newer = history(id = 12L, objectId = "abc123", lastSeen = 200L)
        val lookup = RecordingDetailLookup(
            live = liveAircraft(id = "abc123", lastUpdated = 300L),
            snapshots = listOf(old, newer),
        )
        val loader = DetailSelectionLoader(lookup)

        val loaded = loader.loadHistorical(11L)

        assertEquals(11L, loaded?.id)
        assertEquals(100L, loaded?.lastSeen)
        assertEquals(0, lookup.liveReads)
        assertEquals(listOf(11L), lookup.historicalReads)
    }

    @Test
    fun cancellingRowDeletionPerformsNoStoreOperation() = runTest {
        val store = FakeHistoryStore(listOf(history(id = 11L)))
        val viewModel = HistoryViewModel(store)

        viewModel.requestDelete(store.rows.value.single())
        viewModel.dismissDeletion()

        assertEquals(emptyList<Long>(), store.deletedIds)
        assertEquals(0, store.clearCalls)
    }

    @Test
    fun confirmingRowDeletionDeletesOnlySelectedDatabaseIdOnce() = runTest {
        val store = FakeHistoryStore(listOf(history(id = 11L)))
        val viewModel = HistoryViewModel(store)

        viewModel.requestDelete(store.rows.value.single())
        viewModel.confirmDeletion().join()
        viewModel.confirmDeletion().join()

        assertEquals(listOf(11L), store.deletedIds)
        assertEquals(0, store.clearCalls)
    }

    @Test
    fun confirmingClearUsesDistinctStoreOperationOnce() = runTest {
        val store = FakeHistoryStore(listOf(history(id = 11L)))
        val viewModel = HistoryViewModel(store)

        viewModel.requestClearAll()
        viewModel.confirmDeletion().join()
        viewModel.confirmDeletion().join()

        assertEquals(emptyList<Long>(), store.deletedIds)
        assertEquals(1, store.clearCalls)
    }

    @Test
    fun rapidDoubleConfirmationStartsOnlyOneStoreOperation() = runTest {
        val gate = CompletableDeferred<Unit>()
        val store = FakeHistoryStore(listOf(history(id = 11L)), deleteGate = gate)
        val viewModel = HistoryViewModel(store)

        viewModel.requestDelete(store.rows.value.single())
        val first = viewModel.confirmDeletion()
        val second = viewModel.confirmDeletion()
        gate.complete(Unit)
        first.join()
        second.join()

        assertEquals(listOf(11L), store.deletedIds)
    }

    @Test
    fun contentStatePreservesStoreRowOrder() = runTest {
        val store = FakeHistoryStore(
            listOf(
                history(id = 12L, lastSeen = 200L),
                history(id = 11L, lastSeen = 100L),
            ),
        )
        val viewModel = HistoryViewModel(store)

        val content = viewModel.uiState.value.body as CollectionBodyState.Content

        assertEquals(listOf(12L, 11L), content.rows.map(HistoryEntity::id))
    }
}

private class FakeHistoryStore(
    initialRows: List<HistoryEntity>,
    private val deleteGate: CompletableDeferred<Unit>? = null,
) : HistoryStore {
    val rows = MutableStateFlow(initialRows)
    val deletedIds = mutableListOf<Long>()
    var clearCalls = 0

    override fun observeAll(): Flow<List<HistoryEntity>> = rows

    override fun observeByType(objectType: String): Flow<List<HistoryEntity>> = rows

    override suspend fun getById(id: Long): HistoryEntity? = rows.value.firstOrNull { it.id == id }

    override suspend fun getNewestByObjectId(objectId: String): HistoryEntity? =
        rows.value.filter { it.objectId == objectId }.maxByOrNull { it.lastSeen }

    override suspend fun save(entity: HistoryEntity): Long = entity.id

    override suspend fun deleteById(id: Long) {
        deletedIds += id
        deleteGate?.await()
        rows.value = rows.value.filterNot { it.id == id }
    }

    override suspend fun clearAll() {
        clearCalls += 1
        rows.value = emptyList()
    }

    override suspend fun prune(beforeTimeMillis: Long) = Unit
}

private class RecordingDetailLookup(
    private val live: SkyObject,
    snapshots: List<HistoryEntity>,
) : DetailLookup {
    private val byId = snapshots.associateBy(HistoryEntity::id)
    var liveReads = 0
    val historicalReads = mutableListOf<Long>()

    override fun currentObject(objectId: String): SkyObject? {
        liveReads += 1
        return live.takeIf { it.id == objectId }
    }

    override suspend fun newestSnapshot(objectId: String): HistoryEntity? =
        byId.values.filter { it.objectId == objectId }.maxByOrNull { it.lastSeen }

    override suspend fun snapshot(historyId: Long): HistoryEntity? {
        historicalReads += historyId
        return byId[historyId]
    }
}

private fun history(
    id: Long = 1L,
    objectId: String = "history-object",
    objectType: String = "aircraft",
    detectionSource: String = "ads_b",
    category: String = "commercial",
    displayName: String = "TEST123",
    description: String? = "Stored description",
    latitude: Double = 37.6213,
    longitude: Double = -122.3790,
    altitudeMeters: Double = 1_234.0,
    userLatitude: Double = 37.7749,
    userLongitude: Double = -122.4194,
    distanceMeters: Double? = 2_500.0,
    confidence: Float = 0.91f,
    firstSeen: Long = 50L,
    lastSeen: Long = 100L,
    photoUrl: String? = "https://example.test/stored.jpg",
) = HistoryEntity(
    id = id,
    objectId = objectId,
    objectType = objectType,
    detectionSource = detectionSource,
    category = category,
    displayName = displayName,
    description = description,
    latitude = latitude,
    longitude = longitude,
    altitudeMeters = altitudeMeters,
    userLatitude = userLatitude,
    userLongitude = userLongitude,
    distanceMeters = distanceMeters,
    confidence = confidence,
    firstSeen = firstSeen,
    lastSeen = lastSeen,
    photoUrl = photoUrl,
)

private fun liveAircraft(
    id: String = "live-object",
    position: Position = Position(40.0, -73.0, 9_000.0),
    source: DetectionSource = DetectionSource.ADS_B,
    category: ObjectCategory = ObjectCategory.COMMERCIAL,
    confidence: Float = 0.99f,
    firstSeen: Long = 250L,
    lastUpdated: Long = 300L,
    distanceMeters: Double? = 100.0,
    screenX: Float? = 10f,
    screenY: Float? = 20f,
    icaoHex: String = id,
    callsign: String? = "LIVE999",
    registration: String? = "N999ZZ",
    aircraftType: String? = "B738",
    aircraftModel: String? = "Live model",
    airline: String? = "Live airline",
    operatorName: String? = "Live operator",
    origin: String? = "SFO",
    destination: String? = "JFK",
    squawk: String? = "1200",
    isOnGround: Boolean = false,
    photoUrl: String? = "https://example.test/live.jpg",
    classificationSignals: List<String>? = listOf("live"),
) = Aircraft(
    id = id,
    position = position,
    source = source,
    category = category,
    confidence = confidence,
    firstSeen = Instant.ofEpochMilli(firstSeen),
    lastUpdated = Instant.ofEpochMilli(lastUpdated),
    distanceMeters = distanceMeters,
    screenX = screenX,
    screenY = screenY,
    icaoHex = icaoHex,
    callsign = callsign,
    registration = registration,
    aircraftType = aircraftType,
    aircraftModel = aircraftModel,
    airline = airline,
    operatorName = operatorName,
    origin = origin,
    destination = destination,
    squawk = squawk,
    isOnGround = isOnGround,
    photoUrl = photoUrl,
    classificationSignals = classificationSignals,
)
