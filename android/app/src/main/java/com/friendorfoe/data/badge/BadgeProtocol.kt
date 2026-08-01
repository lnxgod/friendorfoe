package com.friendorfoe.data.badge

import com.google.gson.JsonObject

enum class BadgeDisplayLane(val wireValue: String, val firmwareByte: Int) {
    OFF("off", 0),
    LOWER("lower", 1),
    TOP("top", 2),
    BOTH("both", 3)
}

enum class BadgeMinimumProximity(val wireValue: String, val firmwareByte: Int) {
    PRESENT("present", 0),
    NEAR("near", 1),
    CLOSE("close", 2)
}

enum class BadgeDisplayAction(val wireValue: String) {
    NEXT("next"),
    DETAIL("detail"),
    BACK("back")
}

enum class BadgeNetworkMode(val wireValue: String) {
    USB_ONLY("usb_only"),
    LOCAL_AP("local_ap"),
    BACKEND("backend")
}

enum class BadgeRecoveryCommand {
    REBOOT,
    BOOTLOADER
}

enum class BadgeRecoveryAcknowledgement {
    REBOOT_OK,
    BOOTLOADER_OK
}

data class BadgeTheme(
    val version: Int,
    internal val palette: String,
    val background: String,
    val intensity: Int,
    val accents: Map<String, Int>
) {
    companion object {
        val accentOrder = listOf(
            "drone",
            "meta",
            "tracker",
            "flock",
            "wifi_attack",
            "clear"
        )
        val allowedPalettes = setOf("field", "night", "neon", "mono")
        val allowedBackgrounds = setOf("dark", "dim", "scanline")

        fun firmwareDefaults() = BadgeTheme(
            version = 1,
            palette = "field",
            background = "dark",
            intensity = 100,
            accents = linkedMapOf(
                "drone" to 0xFEA0,
                "meta" to 0xF833,
                "tracker" to 0xF81F,
                "flock" to 0xA81F,
                "wifi_attack" to 0x07FF,
                "clear" to 0x2F65
            )
        )

        fun validate(value: BadgeTheme): Result<BadgeTheme> = runCatching {
            require(value.version == 1) { "Unsupported theme version" }
            require(value.palette in allowedPalettes) { "Unknown theme palette" }
            require(value.background in allowedBackgrounds) { "Unknown theme background" }
            require(value.intensity in 25..100) { "Theme intensity is outside the firmware range" }
            require(value.accents.keys == accentOrder.toSet()) { "Theme accent set is incomplete" }
            require(value.accents.values.all { it in 1..0xFFFF }) {
                "Theme accents must be nonzero RGB565 values"
            }
            value
        }
    }

    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        addProperty("palette", palette)
        addProperty("background", background)
        addProperty("brightness", intensity)
        add("accents", JsonObject().apply {
            accentOrder.forEach { key -> addProperty(key, accents.getValue(key)) }
        })
    }
}

data class BadgeDisplayRule(
    val enabled: Boolean,
    val lane: BadgeDisplayLane,
    val minProximity: BadgeMinimumProximity,
    val priority: Int
)

data class BadgeDisplayPolicy(
    val version: Int,
    val classes: Map<String, BadgeDisplayRule>
) {
    companion object {
        val classOrder = listOf(
            "drone",
            "meta",
            "tracker",
            "wifi_attack",
            "skimmer",
            "camera",
            "flock",
            "lock",
            "hid",
            "beacon",
            "event_badge",
            "auracast",
            "scanner_status"
        )

        fun firmwareDefaults() = BadgeDisplayPolicy(
            version = 1,
            classes = linkedMapOf(
                "drone" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.BOTH,
                    BadgeMinimumProximity.PRESENT,
                    100
                ),
                "meta" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.BOTH,
                    BadgeMinimumProximity.PRESENT,
                    95
                ),
                "tracker" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    70
                ),
                "wifi_attack" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.BOTH,
                    BadgeMinimumProximity.PRESENT,
                    90
                ),
                "skimmer" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.BOTH,
                    BadgeMinimumProximity.NEAR,
                    88
                ),
                "camera" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    65
                ),
                "flock" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.BOTH,
                    BadgeMinimumProximity.PRESENT,
                    85
                ),
                "lock" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    55
                ),
                "hid" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.CLOSE,
                    45
                ),
                "beacon" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    30
                ),
                "event_badge" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    35
                ),
                "auracast" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.NEAR,
                    20
                ),
                "scanner_status" to BadgeDisplayRule(
                    true,
                    BadgeDisplayLane.LOWER,
                    BadgeMinimumProximity.PRESENT,
                    10
                )
            )
        )

        fun validate(value: BadgeDisplayPolicy): Result<BadgeDisplayPolicy> = runCatching {
            require(value.version == 1) { "Unsupported display policy version" }
            require(value.classes.keys == classOrder.toSet()) {
                "Display policy class set is incomplete"
            }
            value.classes.values.forEach { row ->
                require(row.priority in 0..100) { "Display priority is outside the firmware range" }
                require(!row.enabled || row.lane != BadgeDisplayLane.OFF) {
                    "Enabled display rows cannot use the off lane"
                }
            }
            value
        }
    }

    fun withEnabled(key: String, enabled: Boolean): BadgeDisplayPolicy {
        val current = classes.getValue(key)
        val defaults = firmwareDefaults().classes.getValue(key)
        val next = if (enabled) {
            current.copy(
                enabled = true,
                lane = if (current.lane == BadgeDisplayLane.OFF) defaults.lane else current.lane,
                minProximity = if (current.lane == BadgeDisplayLane.OFF) {
                    defaults.minProximity
                } else {
                    current.minProximity
                },
                priority = current.priority
            )
        } else {
            current.copy(enabled = false, lane = BadgeDisplayLane.OFF)
        }
        return copy(classes = LinkedHashMap(classes).apply { put(key, next) })
    }

    fun toJsonObject(): JsonObject = JsonObject().apply {
        addProperty("version", version)
        add("classes", JsonObject().apply {
            classOrder.forEach { key ->
                val row = classes.getValue(key)
                add(key, JsonObject().apply {
                    addProperty("enabled", row.enabled)
                    addProperty("lane", row.lane.wireValue)
                    addProperty("min_proximity", row.minProximity.wireValue)
                    addProperty("priority", row.priority)
                })
            }
        })
    }
}

data class BadgeDisplayPolicyClassInfo(
    val key: String,
    val label: String
)

val BadgeDisplayPolicyClasses = listOf(
    BadgeDisplayPolicyClassInfo("drone", "Drone"),
    BadgeDisplayPolicyClassInfo("meta", "Meta Glasses"),
    BadgeDisplayPolicyClassInfo("tracker", "Tracker"),
    BadgeDisplayPolicyClassInfo("wifi_attack", "WiFi Attack"),
    BadgeDisplayPolicyClassInfo("skimmer", "Skimmer"),
    BadgeDisplayPolicyClassInfo("camera", "Camera"),
    BadgeDisplayPolicyClassInfo("flock", "Flock/ALPR"),
    BadgeDisplayPolicyClassInfo("lock", "Lock"),
    BadgeDisplayPolicyClassInfo("hid", "BLE HID"),
    BadgeDisplayPolicyClassInfo("beacon", "Venue Beacon"),
    BadgeDisplayPolicyClassInfo("event_badge", "Event Badge"),
    BadgeDisplayPolicyClassInfo("auracast", "Auracast"),
    BadgeDisplayPolicyClassInfo("scanner_status", "Scanner Status")
)

data class BadgeThemeAccentInfo(
    val key: String,
    val label: String,
    val defaultRgb565: Int
)

val BadgeThemeAccentClasses = listOf(
    BadgeThemeAccentInfo("drone", "Drone", 0xFEA0),
    BadgeThemeAccentInfo("meta", "Meta", 0xF833),
    BadgeThemeAccentInfo("tracker", "Tracker", 0xF81F),
    BadgeThemeAccentInfo("flock", "Flock", 0xA81F),
    BadgeThemeAccentInfo("wifi_attack", "WiFi Attack", 0x07FF),
    BadgeThemeAccentInfo("clear", "Clear", 0x2F65)
)

val BadgeThemePalettes = listOf("field", "night", "neon", "mono")
val BadgeThemeBackgrounds = listOf("dark", "dim", "scanline")

fun defaultBadgeThemeAccents(): Map<String, Int> = BadgeTheme.firmwareDefaults().accents

fun defaultBadgeTheme(): BadgeTheme = BadgeTheme.firmwareDefaults()

fun defaultBadgeDisplayPolicyClasses(): Map<String, BadgeDisplayRule> =
    BadgeDisplayPolicy.firmwareDefaults().classes

fun defaultBadgeDisplayPolicy(): BadgeDisplayPolicy = BadgeDisplayPolicy.firmwareDefaults()

private class FirmwareFnv1a {
    private var hash = 0x811C9DC5u

    fun byte(value: Int) {
        hash = (hash xor (value and 0xFF).toUInt()) * 0x01000193u
    }

    fun asciiZ(value: String) {
        value.encodeToByteArray().forEach { byte(it.toInt()) }
        byte(0)
    }

    fun value(): Long = hash.toLong() and 0xFFFF_FFFFL
}

fun BadgeTheme.firmwareHash(): Long = FirmwareFnv1a().apply {
    byte(version)
    byte(intensity)
    asciiZ(palette)
    asciiZ(background)
    BadgeTheme.accentOrder.forEach { key ->
        val color = accents.getValue(key)
        byte(color ushr 8)
        byte(color)
    }
}.value()

fun BadgeDisplayPolicy.firmwareHash(): Long = FirmwareFnv1a().apply {
    byte(version)
    BadgeDisplayPolicy.classOrder.forEach { key ->
        val row = classes.getValue(key)
        byte(if (row.enabled) 1 else 0)
        byte(row.lane.firmwareByte)
        byte(row.minProximity.firmwareByte)
        byte(row.priority)
    }
}.value()

fun badgeThemeCommandJson(theme: BadgeTheme): JsonObject = JsonObject().apply {
    BadgeTheme.validate(theme).getOrThrow()
    addProperty("cmd", "badge_theme")
    addProperty("persist", true)
    add("theme", theme.toJsonObject())
}

fun badgeDisplayNavCommandJson(action: BadgeDisplayAction): JsonObject = JsonObject().apply {
    addProperty("cmd", "display_nav")
    addProperty("action", action.wireValue)
}

fun badgeDisplayPolicyCommandJson(policy: BadgeDisplayPolicy): JsonObject = JsonObject().apply {
    BadgeDisplayPolicy.validate(policy).getOrThrow()
    addProperty("cmd", "badge_display_policy")
    addProperty("persist", true)
    add("policy", policy.toJsonObject())
}

fun badgeNetworkModeCommandJson(mode: BadgeNetworkMode): JsonObject = JsonObject().apply {
    addProperty("cmd", "set_mode")
    addProperty("mode", mode.wireValue)
    addProperty("persist", true)
}

fun badgeRebootCommandJson(): JsonObject = JsonObject().apply {
    addProperty("cmd", "reboot")
}

fun badgeBootloaderCommandJson(): JsonObject = JsonObject().apply {
    addProperty("cmd", "bootloader")
}

fun parseBadgeRecoveryAcknowledgement(
    line: String,
    pendingCommand: BadgeRecoveryCommand?
): BadgeRecoveryAcknowledgement? {
    return when (pendingCommand) {
        BadgeRecoveryCommand.REBOOT -> if (line == "FOF_REBOOT:OK") {
            BadgeRecoveryAcknowledgement.REBOOT_OK
        } else {
            null
        }
        BadgeRecoveryCommand.BOOTLOADER -> if (line == "FOF_BOOTLOADER:OK") {
            BadgeRecoveryAcknowledgement.BOOTLOADER_OK
        } else {
            null
        }
        null -> null
    }
}

internal class BadgeRecoveryTracker {
    @Volatile
    var pendingCommand: BadgeRecoveryCommand? = null
        private set

    @Synchronized
    fun begin(command: BadgeRecoveryCommand): Boolean {
        if (pendingCommand != null) return false
        pendingCommand = command
        return true
    }

    @Synchronized
    fun accept(line: String): BadgeRecoveryAcknowledgement? {
        val acknowledgement = parseBadgeRecoveryAcknowledgement(line, pendingCommand)
            ?: return null
        pendingCommand = null
        return acknowledgement
    }

    @Synchronized
    fun cancel(command: BadgeRecoveryCommand) {
        if (pendingCommand == command) pendingCommand = null
    }

    @Synchronized
    fun clear() {
        pendingCommand = null
    }
}

internal fun isDirectUsbRecoverySupported(status: BadgeUsbStatus): Boolean =
    status == BadgeUsbStatus.CONNECTED
