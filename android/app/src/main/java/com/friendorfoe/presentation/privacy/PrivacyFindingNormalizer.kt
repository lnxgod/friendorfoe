package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.BleSignatures
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory

object PrivacyFindingNormalizer {
    fun normalize(input: GlassesDetection): GlassesDetection {
        val appleEvidence = input.bleCompanyId == 0x004C ||
            input.bleAppleType != null ||
            input.manufacturer.equals("Apple", ignoreCase = true) ||
            input.details.keys.any { it.startsWith("apple_", ignoreCase = true) }
        val badgeListeningEvidence = input.details.containsKey("apple_badge_evidence")
        val listeningClaim = badgeListeningEvidence ||
            input.category == PrivacyCategory.REMOTE_LISTENING ||
            listOf(input.deviceType, input.deviceName, input.matchReason)
                .filterNotNull().any { it.contains("listening", ignoreCase = true) }
        val alreadyMappedAppleActivity = input.matchReason == "apple_activity"
        if (!appleEvidence || (!listeningClaim && !alreadyMappedAppleActivity)) return input

        val airPodsEvidence = input.deviceType.contains("airpods", ignoreCase = true) ||
            input.details.values.any { it.contains("airpods", ignoreCase = true) } ||
            ((input.bleAppleFlags ?: 0) and BleSignatures.APPLE_FLAG_AIRPODS_IN != 0)
        val safeOwnedName = input.deviceName?.takeIf {
            input.isBonded && it.isNotBlank() && !it.contains("listening", ignoreCase = true)
        }
        val title = safeOwnedName ?: if (airPodsEvidence) "AirPods connection/activity nearby"
        else "Apple device activity nearby"
        val safeDetails = input.details.filterNot { (key, value) ->
            listOf(key, value).any { text ->
                text.contains("listening", ignoreCase = true) ||
                    text.contains("eavesdrop", ignoreCase = true)
            }
        }
        return input.copy(
            deviceType = title,
            deviceName = safeOwnedName,
            hasCamera = false,
            matchReason = "apple_activity",
            category = PrivacyCategory.APPLE_CONTINUITY,
            details = safeDetails + mapOf(
                "evidence" to if (airPodsEvidence)
                    "An Apple device reports connected AirPods and media, call, or video activity."
                else "An Apple device reports a nearby activity state; the specific activity is unavailable.",
                "limitation" to "Live Listen and microphone use cannot be determined from BLE.",
            ),
        )
    }
}
