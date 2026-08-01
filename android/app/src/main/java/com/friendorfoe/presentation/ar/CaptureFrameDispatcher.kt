package com.friendorfoe.presentation.ar

import java.util.concurrent.Executor
import java.util.concurrent.RejectedExecutionException

internal fun <Frame, Result : Any> dispatchCapturedFrame(
    frame: Frame,
    processingExecutor: Executor,
    resultExecutor: Executor,
    closeFrame: (Frame) -> Unit,
    convertFrame: (Frame) -> Result,
    onResult: (Result?) -> Unit,
    onFailure: (Throwable) -> Unit = {},
) {
    val task = Runnable {
        val result = try {
            convertFrame(frame)
        } catch (failure: Throwable) {
            onFailure(failure)
            null
        } finally {
            closeFrame(frame)
        }
        resultExecutor.execute { onResult(result) }
    }

    try {
        processingExecutor.execute(task)
    } catch (rejected: RejectedExecutionException) {
        closeFrame(frame)
        onFailure(rejected)
        resultExecutor.execute { onResult(null) }
    }
}
