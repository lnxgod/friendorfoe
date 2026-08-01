package com.friendorfoe.data.badge

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import kotlinx.coroutines.CompletableDeferred

fun parseHttpCommandOutcome(statusCode: Int, body: String): BadgeCommandOutcome {
    if (statusCode !in 200..299) {
        return BadgeCommandOutcome.Failed("Badge HTTP command failed ($statusCode)")
    }
    val obj = parseObject(body)
        ?: return BadgeCommandOutcome.Failed("Badge acknowledgement was malformed")
    val ok = obj.strictBooleanOrNull("ok")
        ?: return BadgeCommandOutcome.Failed("Badge acknowledgement did not include ok")
    if (!ok) {
        return BadgeCommandOutcome.Failed(
            obj.strictStringOrNull("error") ?: "Badge command was rejected",
        )
    }
    return acknowledgedOutcome(obj)
}

fun parseUsbControlLine(line: String): BadgeCommandOutcome? {
    val trimmed = line.trim()
    val isSuccess = trimmed.startsWith("FOF_CTL_OK:")
    val isFailure = trimmed.startsWith("FOF_CTL_ERROR:")
    if (!isSuccess && !isFailure) return null
    val body = trimmed.substringAfter(':')
    val obj = parseObject(body)
        ?: return BadgeCommandOutcome.Failed("Badge acknowledgement was malformed")
    val usbOk = obj.optionalStrictBoolean("ok")
        ?: return BadgeCommandOutcome.Failed("Badge acknowledgement ok field was invalid")
    if (isFailure || usbOk.value == false) {
        return BadgeCommandOutcome.Failed(
            obj.strictStringOrNull("error") ?: "Badge command failed",
        )
    }
    return acknowledgedOutcome(obj)
}

fun parseUsbCommandLine(command: BadgeCommand, line: String): BadgeCommandOutcome? = when (command) {
    BadgeCommand.Reboot -> if (line == "FOF_REBOOT:OK") {
        BadgeCommandOutcome.Acknowledged(BadgeControlAcknowledgement("Reboot acknowledged"))
    } else {
        null
    }
    BadgeCommand.EnterBootloader -> if (line == "FOF_BOOTLOADER:OK") {
        BadgeCommandOutcome.Acknowledged(
            BadgeControlAcknowledgement("Bootloader acknowledged"),
        )
    } else {
        null
    }
    else -> parseUsbControlLine(line)
}

fun verifiesDebugPostCommandStatus(
    preSerialPort: String?,
    prePhysicalAtElapsedMs: Long?,
    sentAtElapsedMs: Long,
    postSerialPort: String?,
    postPhysicalAtElapsedMs: Long?,
    postAndroidReceiptAtElapsedMs: Long,
    postLastError: String?,
): Boolean = !preSerialPort.isNullOrBlank() &&
    prePhysicalAtElapsedMs != null &&
    postSerialPort == preSerialPort &&
    postPhysicalAtElapsedMs != null &&
    postPhysicalAtElapsedMs >= prePhysicalAtElapsedMs &&
    postPhysicalAtElapsedMs >= sentAtElapsedMs &&
    postAndroidReceiptAtElapsedMs >= sentAtElapsedMs &&
    postLastError != null &&
    postLastError.isEmpty()

private fun acknowledgedOutcome(obj: JsonObject): BadgeCommandOutcome {
    val applied = obj.optionalStrictBoolean("applied")
        ?: return BadgeCommandOutcome.Failed("Badge applied field was invalid")
    if (applied.value == false) {
        return BadgeCommandOutcome.Failed(
            obj.strictStringOrNull("error") ?: "Badge reported that the command was not applied",
        )
    }
    val rawRuntimeMode = obj.optionalStrictString("network_mode")
        ?: return BadgeCommandOutcome.Failed("Badge network mode field was invalid")
    val runtimeMode = rawRuntimeMode.value?.let { raw ->
        BadgeRuntimeNetworkMode.entries.firstOrNull { it.wireValue == raw }
            ?: return BadgeCommandOutcome.Failed("Badge network mode was unknown")
    }
    val themeHash = obj.strictUnsignedFirmwareHash("theme_hash")
        ?: return BadgeCommandOutcome.Failed("Badge theme hash was invalid")
    val displayPolicyHash = obj.strictUnsignedFirmwareHash("display_policy_hash")
        ?: return BadgeCommandOutcome.Failed("Badge display policy hash was invalid")
    val policyHash = obj.strictUnsignedFirmwareHash("policy_hash")
        ?: return BadgeCommandOutcome.Failed("Badge policy hash was invalid")
    return BadgeCommandOutcome.Acknowledged(
        BadgeControlAcknowledgement(
            message = obj.strictStringOrNull("message") ?: "Badge command acknowledged",
            themeHash = themeHash.value,
            policyHash = displayPolicyHash.value ?: policyHash.value,
            networkApplied = applied.value,
            runtimeNetworkMode = runtimeMode,
        ),
    )
}

private fun parseObject(body: String): JsonObject? = runCatching {
    JsonParser.parseString(body).takeIf { it.isJsonObject }?.asJsonObject
}.getOrNull()

private fun JsonObject.strictBooleanOrNull(key: String): Boolean? = runCatching {
    get(key)?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isBoolean }?.asBoolean
}.getOrNull()

private fun JsonObject.strictStringOrNull(key: String): String? = runCatching {
    get(key)?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }?.asString
}.getOrNull()

private data class OptionalBoolean(val value: Boolean?)

private fun JsonObject.optionalStrictBoolean(key: String): OptionalBoolean? {
    if (!has(key)) return OptionalBoolean(null)
    val element = get(key) ?: return null
    if (!element.isJsonPrimitive || !element.asJsonPrimitive.isBoolean) return null
    return OptionalBoolean(element.asBoolean)
}

private data class OptionalString(val value: String?)

private fun JsonObject.optionalStrictString(key: String): OptionalString? {
    if (!has(key)) return OptionalString(null)
    val element = get(key) ?: return null
    if (!element.isJsonPrimitive || !element.asJsonPrimitive.isString) return null
    return OptionalString(element.asString)
}

private data class OptionalFirmwareHash(val value: Long?)

private fun JsonObject.strictUnsignedFirmwareHash(key: String): OptionalFirmwareHash? {
    if (!has(key)) return OptionalFirmwareHash(null)
    val element = get(key) ?: return null
    if (!element.isJsonPrimitive || !element.asJsonPrimitive.isNumber) return null
    val raw = element.toString()
    if (!raw.matches(Regex("[0-9]+"))) return null
    val value = raw.toLongOrNull() ?: return null
    if (value !in 1L..0xFFFF_FFFFL) return null
    return OptionalFirmwareHash(value)
}

private data class PendingBadgeCommand(
    val transportGeneration: Long,
    val command: BadgeCommand,
    val outcome: CompletableDeferred<BadgeCommandOutcome>,
)

internal class BadgeUsbCommandCoordinator {
    private val lock = Any()
    private var pending: PendingBadgeCommand? = null
    private var poisoned = false
    private var transportGeneration = 1L

    fun currentTransportGeneration(): Long = synchronized(lock) { transportGeneration }

    fun begin(
        generation: Long,
        command: BadgeCommand,
        outcome: CompletableDeferred<BadgeCommandOutcome>,
    ): Boolean = synchronized(lock) {
        if (generation != transportGeneration || poisoned || pending != null) return@synchronized false
        pending = PendingBadgeCommand(generation, command, outcome)
        true
    }

    fun acceptSerialLine(generation: Long, line: String): Boolean {
        val completion = synchronized(lock) {
            if (generation != transportGeneration || poisoned) return@synchronized null
            val current = pending ?: return@synchronized null
            if (current.transportGeneration != generation) return@synchronized null
            val parsed = parseUsbCommandLine(current.command, line) ?: return@synchronized null
            pending = null
            current.outcome to parsed
        } ?: return false
        completion.first.complete(completion.second)
        return true
    }

    fun timeout(
        generation: Long,
        command: BadgeCommand,
        outcome: CompletableDeferred<BadgeCommandOutcome>,
    ) = synchronized(lock) {
        if (generation != transportGeneration) return@synchronized
        poisoned = true
        val current = pending
        if (current?.transportGeneration == generation &&
            current.command == command && current.outcome === outcome
        ) {
            pending = null
        }
    }

    fun cancelAfterAttempt(
        generation: Long,
        command: BadgeCommand,
        outcome: CompletableDeferred<BadgeCommandOutcome>,
    ) {
        timeout(generation, command, outcome)
    }

    fun clearExact(outcome: CompletableDeferred<BadgeCommandOutcome>) {
        synchronized(lock) {
            val current = pending ?: return@synchronized
            if (current.outcome === outcome) pending = null
        }
    }

    fun invalidateTransport(message: String): Long {
        val invalidation = synchronized(lock) {
            val current = pending
            pending = null
            poisoned = false
            transportGeneration += 1
            current?.outcome to transportGeneration
        }
        invalidation.first?.complete(BadgeCommandOutcome.Failed(message))
        return invalidation.second
    }

    fun resetTransportGeneration(): Long = invalidateTransport("Badge transport changed")
}

internal class BadgeBleCommandCoordinator {
    private val lock = Any()
    private var pending: PendingBadgeCommand? = null
    private var poisoned = false
    private var transportGeneration = 1L

    fun currentTransportGeneration(): Long = synchronized(lock) { transportGeneration }

    fun begin(
        generation: Long,
        command: BadgeCommand,
        outcome: CompletableDeferred<BadgeCommandOutcome>,
    ): Boolean = synchronized(lock) {
        if (generation != transportGeneration || poisoned || pending != null) return@synchronized false
        pending = PendingBadgeCommand(generation, command, outcome)
        true
    }

    fun acceptWriteCallback(generation: Long, success: Boolean): Boolean {
        val completion = synchronized(lock) {
            if (generation != transportGeneration || poisoned) return@synchronized null
            val current = pending ?: return@synchronized null
            if (current.transportGeneration != generation) return@synchronized null
            pending = null
            current.outcome
        } ?: return false
        completion.complete(
            if (success) {
                BadgeCommandOutcome.Accepted(
                    "Badge BLE command accepted; checking readback",
                )
            } else {
                BadgeCommandOutcome.Failed("Badge BLE write failed")
            },
        )
        return true
    }

    fun timeout(generation: Long, outcome: CompletableDeferred<BadgeCommandOutcome>) =
        synchronized(lock) {
            if (generation != transportGeneration) return@synchronized
            poisoned = true
            val current = pending
            if (current?.transportGeneration == generation && current.outcome === outcome) {
                pending = null
            }
        }

    fun cancelAfterAttempt(
        generation: Long,
        outcome: CompletableDeferred<BadgeCommandOutcome>,
    ) {
        timeout(generation, outcome)
    }

    fun clearExact(outcome: CompletableDeferred<BadgeCommandOutcome>) {
        synchronized(lock) {
            val current = pending ?: return@synchronized
            if (current.outcome === outcome) pending = null
        }
    }

    fun invalidateTransport(message: String): Long {
        val invalidation = synchronized(lock) {
            val current = pending
            pending = null
            poisoned = false
            transportGeneration += 1
            current?.outcome to transportGeneration
        }
        invalidation.first?.complete(BadgeCommandOutcome.Failed(message))
        return invalidation.second
    }

    fun resetTransportGeneration(): Long = invalidateTransport("Badge BLE transport changed")
}
