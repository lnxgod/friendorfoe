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
        val alreadyMappedAppleActivity = input.matchReason == "apple_activity" ||
            input.matchReason == "own_device:apple_activity"
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

    fun normalize(input: PrivacyFinding): PrivacyFinding {
        val categorySafe = if (
            input.category == PrivacyCategory.APPLE_CONTINUITY &&
            input.severity != FindingSeverity.INFO
        ) {
            input.copy(severity = FindingSeverity.INFO)
        } else {
            input
        }
        val apple = categorySafe.appleEvidence
            ?: inferredSameRowAppleEvidence(categorySafe)
            ?: return categorySafe
        if (!apple.appleFamilyEvidence || !apple.listeningOrientedCategoryOrWording) {
            return categorySafe
        }

        val safeOwnedTitle = categorySafe.title.takeIf {
            categorySafe.ownership == Ownership.OWNED &&
                it.isNotBlank() &&
                !containsListeningClaim(it)
        }
        val title = safeOwnedTitle ?: if (apple.airPodsAssociationEvidence) {
            "AirPods connection/activity nearby"
        } else {
            "Apple device activity nearby"
        }
        return categorySafe.copy(
            title = title,
            evidence = if (apple.airPodsAssociationEvidence) {
                "An Apple device reports connected AirPods and media, call, or video activity."
            } else {
                "An Apple device reports a nearby activity state; the specific activity is unavailable."
            },
            limitation = "Live Listen and microphone use cannot be determined from BLE.",
            category = PrivacyCategory.APPLE_CONTINUITY,
            severity = FindingSeverity.INFO,
        )
    }

    private fun inferredSameRowAppleEvidence(input: PrivacyFinding): PrivacyAppleListeningEvidence? {
        val sameRowText = listOfNotNull(
            input.title,
            input.evidence,
            input.category.name,
            input.category.label,
        ).joinToString(separator = " ")
        val appleFamilyEvidence = APPLE_FAMILY_TOKEN.containsMatchIn(sameRowText)
        val airPodsEvidence = AIRPODS_TOKEN.containsMatchIn(sameRowText)
        val listeningEvidence = input.category == PrivacyCategory.REMOTE_LISTENING ||
            listOf("listening", "eavesdrop", "live listen", "microphone").any {
                sameRowText.contains(it, ignoreCase = true)
            }
        return if (appleFamilyEvidence && listeningEvidence) {
            PrivacyAppleListeningEvidence(
                appleFamilyEvidence = true,
                airPodsAssociationEvidence = airPodsEvidence,
                listeningOrientedCategoryOrWording = true,
            )
        } else {
            null
        }
    }

    private fun containsListeningClaim(text: String): Boolean =
        listOf("listening", "eavesdrop", "live listen", "microphone").any {
            text.contains(it, ignoreCase = true)
        }

    private val APPLE_FAMILY_TOKEN = Regex(
        pattern = "(?i)(?<![\\p{L}\\p{N}])(?:apple|airpods|iphone|ipad)(?![\\p{L}\\p{N}])",
    )
    private val AIRPODS_TOKEN = Regex(
        pattern = "(?i)(?<![\\p{L}\\p{N}])airpods(?![\\p{L}\\p{N}])",
    )
}
