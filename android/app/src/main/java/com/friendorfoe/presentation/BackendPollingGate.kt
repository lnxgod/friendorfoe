package com.friendorfoe.presentation

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.DetectionSettings
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.isActive

internal data class BackendPollGate(
    val enabled: Boolean,
    val endpoint: BackendEndpoint?,
)

internal suspend fun <T> collectBackendWhileEnabled(
    settings: Flow<DetectionSettings>,
    intervalMs: Long,
    clear: () -> Unit,
    fetch: suspend () -> T,
    publish: (T) -> Unit,
    onFailure: (Throwable) -> Unit = {},
) {
    settings.map { current ->
        BackendPollGate(
            enabled = current.sensorBackendEnabled,
            endpoint = BackendEndpoint.parse(current.backendUrl).getOrNull(),
        )
    }.distinctUntilChanged().collectLatest { gate ->
        clear()
        if (!gate.enabled || gate.endpoint == null) return@collectLatest

        while (currentCoroutineContext().isActive) {
            try {
                val value = fetch()
                currentCoroutineContext().ensureActive()
                publish(value)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                currentCoroutineContext().ensureActive()
                onFailure(failure)
            }
            delay(intervalMs)
        }
    }
}
