package com.friendorfoe.presentation.util

import com.google.gson.JsonParser
import java.io.File
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class ReferencePhotoCatalogTest {
    private fun repoFile(path: String): File = listOf(File(path), File("../$path"), File("../../$path"))
        .first { it.exists() }

    @Test fun catalogIdsAreUniqueSoReferenceListsCanRenderEveryEntry() {
        val aircraftIds = AircraftDatabase.allAircraft.map { it.id }
        val droneIds = DroneDatabase.allDrones.map { it.id }
        assertEquals(aircraftIds.size, aircraftIds.toSet().size)
        assertEquals(droneIds.size, droneIds.toSet().size)
        assertEquals("file:///android_asset/aircraft/CH53.jpg", getAircraftPhotoUrl("CH53"))
    }

    @Test fun everyVisibleCatalogPhotoExistsInTheApp() {
        val urls = AircraftDatabase.allAircraft.mapNotNull(::aircraftReferencePhotoUrl) +
            DroneDatabase.allDrones.mapNotNull(::droneReferencePhotoUrl)
        assertTrue(urls.isNotEmpty())
        urls.distinct().forEach { url ->
            val asset = url.removePrefix("file:///android_asset/")
            assertTrue("Missing $asset", repoFile("android/app/src/main/assets/$asset").isFile)
        }
    }

    @Test fun rejectedImagesCannotBeDisplayedThroughTheCatalog() {
        rejectedReferenceAssets.forEach { (asset, reason) ->
            assertTrue(reason.isNotBlank())
            assertNull(asset, bundledReferencePhotoUrl(asset))
        }
        assertNull(aircraftReferencePhotoUrl(AircraftDatabase.allAircraft.first { it.id == "y20" }))
        assertNull(droneReferencePhotoUrl(DroneDatabase.allDrones.first { it.id == "hoverair_x1_pro" }))
    }

    @Test fun exactAirframesNoLongerBorrowUnrelatedModelPhotos() {
        listOf("SU57", "SU25", "F4", "F5", "KFIR", "CH46", "NH90", "MI26", "MQ1", "RQ4", "PC24", "SF50")
            .forEach { code -> assertEquals("file:///android_asset/aircraft/$code.jpg", getAircraftPhotoUrl(code)) }
        assertEquals("file:///android_asset/aircraft/F18.jpg", getAircraftPhotoUrl("FA18"))
        assertNull(getAircraftPhotoUrl("FC31"))
        assertNull(getAircraftPhotoUrl("A320FAKE"))
    }

    @Test fun reviewedReplacementsKeepTheirVerifiedBytesAndCredits() {
        val records = JsonParser.parseString(repoFile("docs/design/interface-refresh/photo-replacements.json").readText()).asJsonArray
        records.forEach { element ->
            val record = element.asJsonObject
            val asset = record["asset"].asString
            val bytes = repoFile("android/app/src/main/assets/$asset").readBytes()
            val hash = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
            assertEquals("Re-review photo and source after changing $asset", record["sha256"].asString, hash)
            assertTrue(record["author"].asString.isNotBlank())
            assertTrue(record["license"].asString.isNotBlank())
            val credits = repoFile("android/app/src/main/assets/${asset.substringBefore('/')}/CREDITS.md").readText()
            assertTrue("Missing credit for $asset", credits.contains(record["source"].asString))
        }
    }
}
