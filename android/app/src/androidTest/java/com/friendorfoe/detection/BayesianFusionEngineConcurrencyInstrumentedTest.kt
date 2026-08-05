package com.friendorfoe.detection

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.friendorfoe.domain.model.DetectionSource
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.time.Duration
import java.time.Instant
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class BayesianFusionEngineConcurrencyInstrumentedTest {

    @Test
    fun cachedObservationIsCountedOnceAndDecayIsIdempotentAtOneInstant() {
        val engine = BayesianFusionEngine()
        val observedAt = Instant.parse("2026-01-01T00:00:00Z")

        val first = engine.updateWithEvidence(
            candidateId = "candidate-1",
            source = DetectionSource.WIFI,
            sensorConfidence = 0.3f,
            observedAt = observedAt,
        )
        val duplicate = engine.updateWithEvidence(
            candidateId = "candidate-1",
            source = DetectionSource.WIFI,
            sensorConfidence = 0.3f,
            observedAt = observedAt,
        )
        val evaluationTime = observedAt.plusSeconds(30)
        val decayedOnce = engine.getFusedProbability("candidate-1", evaluationTime)
        val decayedTwice = engine.getFusedProbability("candidate-1", evaluationTime)

        assertEquals(first, duplicate, 0.000001f)
        assertEquals(decayedOnce, decayedTwice, 0.000001f)
    }

    @Test
    fun concurrentUpdatesReadsPrunesAndResetsRemainSafe() {
        val engine = BayesianFusionEngine()
        val executor = Executors.newFixedThreadPool(4)
        val start = CountDownLatch(1)
        val base = Instant.parse("2026-01-01T00:00:00Z")

        val futures = listOf(
            executor.submit {
                start.await()
                repeat(2_000) { index ->
                    engine.updateWithEvidence(
                        candidateId = "candidate-${index % 600}",
                        source = DetectionSource.REMOTE_ID,
                        sensorConfidence = 0.9f,
                        observedAt = base.plusMillis(index.toLong()),
                    )
                }
            },
            executor.submit {
                start.await()
                repeat(2_000) { index ->
                    engine.getFusedProbability(
                        candidateId = "candidate-${index % 600}",
                        now = base.plusMillis(index.toLong()),
                    )
                }
            },
            executor.submit {
                start.await()
                repeat(500) { index ->
                    engine.pruneStale(
                        now = base.plusSeconds(index.toLong()),
                        maxAge = Duration.ofSeconds(30),
                    )
                }
            },
            executor.submit {
                start.await()
                repeat(100) { engine.reset() }
            },
        )

        start.countDown()
        futures.forEach { it.get(15, TimeUnit.SECONDS) }
        executor.shutdownNow()

        val probability = engine.getFusedProbability("candidate-1", base.plusSeconds(1))
        assertTrue(probability in 0f..1f)
    }
}
