package com.friendorfoe.presentation.ar

import java.util.concurrent.Executor
import java.util.concurrent.RejectedExecutionException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CaptureFrameDispatcherTest {
    @Test
    fun conversionFailureClosesFrameAndReturnsNull() {
        val frame = TestFrame()
        var result: String? = "not called"

        dispatchCapturedFrame(
            frame = frame,
            processingExecutor = Executor(Runnable::run),
            resultExecutor = Executor(Runnable::run),
            closeFrame = TestFrame::close,
            convertFrame = { error("bad image") },
            onResult = { result = it },
        )

        assertEquals(1, frame.closeCalls)
        assertNull(result)
    }

    @Test
    fun executorRejectionClosesFrameWithoutRunningConverter() {
        val frame = TestFrame()
        var conversionCalls = 0
        var result: String? = "not called"

        dispatchCapturedFrame(
            frame = frame,
            processingExecutor = Executor { throw RejectedExecutionException("stopped") },
            resultExecutor = Executor(Runnable::run),
            closeFrame = TestFrame::close,
            convertFrame = {
                conversionCalls += 1
                "converted"
            },
            onResult = { result = it },
        )

        assertEquals(1, frame.closeCalls)
        assertEquals(0, conversionCalls)
        assertNull(result)
    }

    @Test
    fun conversionIsQueuedOnTheProcessingExecutor() {
        val frame = TestFrame()
        val queued = mutableListOf<Runnable>()
        var result: String? = null

        dispatchCapturedFrame(
            frame = frame,
            processingExecutor = Executor(queued::add),
            resultExecutor = Executor(Runnable::run),
            closeFrame = TestFrame::close,
            convertFrame = { "converted" },
            onResult = { result = it },
        )

        assertEquals(0, frame.closeCalls)
        assertNull(result)
        assertEquals(1, queued.size)

        queued.single().run()

        assertEquals(1, frame.closeCalls)
        assertTrue(result == "converted")
    }
}

private class TestFrame {
    var closeCalls: Int = 0

    fun close() {
        closeCalls += 1
    }
}
