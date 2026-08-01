package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.di.ApplicationScope
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.launch

fun interface PrivacyAlertPublisher {
    fun publish(finding: PrivacyFinding): Boolean
}

@Singleton
class PrivacyAlertCoordinator internal constructor(
    private val states: Flow<PrivacyCurrentState>,
    private val policy: PrivacyAlertPolicy,
    private val publisher: PrivacyAlertPublisher,
    private val clock: MonotonicClock,
    private val scope: CoroutineScope,
) {
    @Inject
    constructor(
        repository: PrivacyFindingRepository,
        policy: PrivacyAlertPolicy,
        publisher: PrivacyAlertPublisher,
        clock: MonotonicClock,
        @ApplicationScope scope: CoroutineScope,
    ) : this(repository.currentState, policy, publisher, clock, scope)

    private val started = AtomicBoolean(false)

    fun start() {
        if (!started.compareAndSet(false, true)) return
        scope.launch {
            states.collect { state ->
                val now = clock.nowElapsedMs()
                policy.newAlerts(state.alertEligible, now).forEach { finding ->
                    val published = runCatching { publisher.publish(finding) }.getOrDefault(false)
                    if (published) policy.markPublished(finding, now)
                }
            }
        }
    }
}

@Singleton
class PrivacyAlertBootstrap @Inject constructor(
    private val coordinator: PrivacyAlertCoordinator,
) {
    private val started = AtomicBoolean(false)

    fun start() {
        if (started.compareAndSet(false, true)) coordinator.start()
    }
}
