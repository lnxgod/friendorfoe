package com.friendorfoe.data.repository

import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.remote.BackendHealthClient
import com.friendorfoe.di.ApplicationScope
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

sealed interface SessionHealth {
    data object Untested : SessionHealth
    data class Checking(val endpoint: BackendEndpoint) : SessionHealth
    data class Healthy(val endpoint: BackendEndpoint) : SessionHealth
    data class Failed(
        val endpoint: BackendEndpoint,
        val message: String,
    ) : SessionHealth
}

@Singleton
class BackendSessionHealthRepository @Inject constructor(
    private val healthClient: BackendHealthClient,
    @ApplicationScope private val scope: CoroutineScope,
) {
    private val transitionLock = Any()
    private val generation = AtomicLong(0L)
    private var checkJob: Job? = null

    private val _health = MutableStateFlow<SessionHealth>(SessionHealth.Untested)
    val health: StateFlow<SessionHealth> = _health.asStateFlow()

    private val _serverVersion = MutableStateFlow<String?>(null)
    val serverVersion: StateFlow<String?> = _serverVersion.asStateFlow()

    fun invalidate() = synchronized(transitionLock) {
        invalidateLocked()
    }

    private fun invalidateLocked() {
        generation.incrementAndGet()
        checkJob?.cancel()
        checkJob = null
        _serverVersion.value = null
        _health.value = SessionHealth.Untested
    }

    fun check(endpoint: BackendEndpoint, enabled: Boolean) = synchronized(transitionLock) {
        invalidateLocked()
        if (!enabled) return

        val requestGeneration = generation.get()
        _health.value = SessionHealth.Checking(endpoint)
        checkJob = scope.launch {
            val result = try {
                Result.success(healthClient.check(endpoint))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                Result.failure(failure)
            }
            currentCoroutineContext().ensureActive()
            synchronized(transitionLock) publish@{
                if (generation.get() != requestGeneration) return@publish

                result.fold(
                    onSuccess = { response ->
                        _serverVersion.value = response.version
                        _health.value = SessionHealth.Healthy(endpoint)
                    },
                    onFailure = {
                        _serverVersion.value = null
                        _health.value = SessionHealth.Failed(endpoint, "Connection failed")
                    },
                )
            }
        }
    }

    fun recordConnected(endpoint: BackendEndpoint, serverVersion: String? = null) =
        synchronized(transitionLock) {
            generation.incrementAndGet()
            checkJob?.cancel()
            checkJob = null
            _serverVersion.value = serverVersion
            _health.value = SessionHealth.Healthy(endpoint)
        }
}
