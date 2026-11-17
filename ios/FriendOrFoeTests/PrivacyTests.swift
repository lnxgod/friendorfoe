import CoreLocation
import XCTest
@testable import FriendOrFoe

final class PrivacyTests: XCTestCase {
    func testApproximateLocationOffersBeaconSettingsWithoutStartingRanging() {
        let folder = URL.temporaryDirectory.appending(path: UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: folder) }
        let scanner = PhonePrivacyScanner(storageDirectory: folder, makeLocationManager: { ApproximateBeaconLocation() })
        XCTAssertTrue(scanner.addBeacon(UUID().uuidString))
        scanner.startBeacons()
        XCTAssertFalse(scanner.ranging)
        XCTAssertTrue(scanner.needsLocationSettings)
        XCTAssertTrue(scanner.beaconStatus.contains("Precise Location"))
    }

    func testRecorderNameBoundariesAcrossAndroidFamilies() {
        let examples = [
            "PLAUDAB12": "Plaud", "NotePin_123": "Plaud", "PLAUD NOTE PRO": "Plaud",
            "omi": "Omi", " Limitless Pendant ": "Limitless", "Bee_AB12": "Bee", "friend_123": "Friend",
            "Fieldy": "Fieldy", "viaim RecDot": "viaim", "Viaim OpenNote": "viaim",
            "soundcore Work": "soundcore", "Notta Memo Pro": "Notta", "Instamic BLE": "Instamic",
            "DJI-MIC2-123456": "DJI", "DJI Mic 3": "DJI"
        ]
        for (name, brand) in examples {
            XCTAssertEqual(PhoneSignatures.match(name: name)?.family, brand, name)
            XCTAssertEqual(PhoneSignatures.match(name: name)?.category, .recorder, name)
        }
        for name in ["My PLAUD phone", "Plaudify", "PLAUDAB123", "Beech", "OMIRON", "Friend",
                     "Fieldyard", "viaim Air", "viaim RecDotty", "soundcore Workout", "Notta Memories",
                     "Instamicrophone", "DJI-MIC20", "DJI Mic Mini", "Looki L1", "F2", "AirPods"] {
            XCTAssertNotEqual(PhoneSignatures.match(name: name)?.category, .recorder, name)
        }
    }

    func testCustomServicesInEitherAdvertisementFieldOverrideConflictingName() {
        let uuids = [
            "19b10000-e8f2-537e-4f6c-d104768a1214": "Omi",
            "632de001-604c-446b-a80f-7963e950f3fb": "Limitless",
            "03d5d5c4-a86c-11ee-9d89-8f2089a49e7e": "Bee",
            "1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da": "Friend"
        ]
        for (uuid, brand) in uuids {
            XCTAssertEqual(PhoneSignatures.match(name: nil, services: [uuid.uppercased()])?.family, brand)
            XCTAssertEqual(PhoneSignatures.match(name: "PLAUD NOTE", serviceData: [uuid: Data()])?.family, brand)
        }
        XCTAssertNil(PhoneSignatures.match(name: nil, services: ["1910", "180f", "4fafc201-1fb5-459e-8fcc-c5c9c331914b"]))
        XCTAssertEqual(PhoneSignatures.match(name: "viaim RecDot", services: ["184E"])?.category, .recorder)
    }

    func testSpecificCameraAndAccessoryFieldsAndConservativeAppleParsing() {
        XCTAssertEqual(PhoneSignatures.match(name: nil, serviceData: ["F74D1BA5-BC52-4F10-99C0-85E785EBE43F": Data()])?.category, .camera)
        XCTAssertEqual(PhoneSignatures.match(name: "PLAUDAB12", services: ["F74D1BA5-BC52-4F10-99C0-85E785EBE43F"])?.category, .camera)
        for vendor in ["FEA6", "FD5F", "FE45", "FDD2", "FD44", "FD5A", "FEEC", "FE33", "0000FD5A-0000-1000-8000-00805F9B34FB"] {
            XCTAssertNil(PhoneSignatures.match(name: nil, services: [vendor]), vendor)
        }
        XCTAssertEqual(PhoneSignatures.match(name: "GoPro 1234", services: ["FEA6"])?.category, .camera)
        XCTAssertNil(PhoneSignatures.match(name: "GoPro 12XX", services: ["FEA6"]))
        XCTAssertEqual(PhoneSignatures.match(name: "Ray-Ban Meta")?.category, .glasses)
        XCTAssertNil(PhoneSignatures.match(name: "Ray-Ban Metaphor"))
        XCTAssertEqual(PhoneSignatures.match(name: "AirPods Pro")?.category, .everyday)
        XCTAssertNil(PhoneSignatures.match(name: nil, manufacturerData: Data([0x4c, 0, 0x12, 0x19, 0])))
        XCTAssertNil(PhoneSignatures.match(name: nil, manufacturerData: Data([0x4c, 0])))
        XCTAssertEqual(PhoneSignatures.match(name: nil, manufacturerData: Data([0x4c, 0, 0x12, 0x19] + Array(repeating: 0, count: 25)))?.category, .tracker)
    }

    func testLegacyRemoteIDRequiresACompleteSupportedBasicFrame() {
        var bytes = [UInt8](repeating: 0, count: 27)
        bytes[0] = 0x0d
        bytes[2] = 2
        bytes[3] = 0x12
        let example = Array("TEST-EXAMPLE".utf8)
        bytes.replaceSubrange(4..<(4 + example.count), with: example)
        let match = PhoneSignatures.match(name: nil, serviceData: ["FFFA": Data(bytes)])
        XCTAssertEqual(match?.category, .drone)
        XCTAssertFalse(match?.evidence.contains("TEST-EXAMPLE") == true)
        XCTAssertNil(PhoneSignatures.match(name: nil, services: ["FFFA"]))
        XCTAssertNil(PhoneSignatures.match(name: nil, serviceData: ["FFF0": Data(bytes)]))
        XCTAssertNil(PhoneSignatures.match(name: nil, serviceData: ["FFFA": Data(bytes.dropLast())]))
        for (index, value): (Int, UInt8) in [(0, 0), (2, 0x12), (2, 15), (3, 2), (4, 0), (4, 0xff), (26, 0x31)] {
            var invalid = bytes
            invalid[index] = value
            XCTAssertNil(PhoneSignatures.match(name: nil, serviceData: ["FFFA": Data(invalid)]))
        }
    }

    func testIndependentPeersAndWeakerUnrelatedFramesDoNotRefreshARecorder() throws {
        let recorder = try XCTUnwrap(PhoneSignatures.match(name: nil, services: ["19b10000-e8f2-537e-4f6c-d104768a1214"]))
        let ordinary = try XCTUnwrap(PhoneSignatures.match(name: "AirPods Pro"))
        var seen = PhoneSightings()
        let t = Date()
        seen.record(id: "one", match: recorder, rssi: -48, at: t)
        seen.record(id: "two", match: recorder, rssi: nil, at: t)
        seen.record(id: "one", match: ordinary, rssi: -80, at: t.addingTimeInterval(10))
        XCTAssertEqual(seen.values["one"]?.count, 1)
        XCTAssertEqual(seen.values["one"]?.lastSeen, t)
        seen.record(id: "one", match: recorder, rssi: -60, at: t.addingTimeInterval(20))
        XCTAssertEqual(seen.values["one"]?.firstSeen, t)
        XCTAssertEqual(seen.values["one"]?.count, 2)
        XCTAssertEqual(seen.values["one"]?.rssi, -60)
        XCTAssertTrue(seen.values["one"]?.contains("omi") == true)
        XCTAssertEqual(seen.values.count, 2)
        seen.remove(id: "one")
        XCTAssertNotNil(seen.values["two"])
    }

    @MainActor
    func testExplicitSavingIgnoringAndKnownBeaconPreferencesSurviveRelaunchAndClear() throws {
        let directory = FileManager.default.temporaryDirectory.appending(path: UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let first = PhonePrivacyScanner(storageDirectory: directory)
        XCTAssertFalse(first.scanning)
        XCTAssertFalse(first.ranging)
        let uuid = UUID()
        XCTAssertFalse(first.addBeacon("invalid"))
        XCTAssertTrue(first.addBeacon(uuid.uuidString))
        XCTAssertFalse(first.addBeacon(uuid.uuidString.lowercased()))
        let observation = PhoneSighting(id: "example", firstSeen: Date(), lastSeen: Date(),
                                       count: 1, rssi: -52, match: try XCTUnwrap(PhoneSignatures.match(name: "PLAUDAB12")))
        first.save(observation)
        first.ignore("another")
        let reopened = PhonePrivacyScanner(storageDirectory: directory)
        XCTAssertEqual(reopened.knownBeacons, [uuid])
        XCTAssertEqual(reopened.saved.first?.id, "example")
        XCTAssertEqual(reopened.ignoredCount, 1)
        XCTAssertTrue(reopened.rows.isEmpty)
        XCTAssertTrue(try directory.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup == true)
        reopened.clearHistory()
        reopened.resetIgnored()
        reopened.removeBeacon(uuid)
        let cleared = PhonePrivacyScanner(storageDirectory: directory)
        XCTAssertTrue(cleared.saved.isEmpty)
        XCTAssertTrue(cleared.knownBeacons.isEmpty)
        XCTAssertEqual(cleared.ignoredCount, 0)
    }

    func testSessionBufferEvictsOldestAndBeaconSearchIncludesUUID() {
        var seen = PhoneSightings()
        let match = BroadcastMatch(category: .beacon, family: "Major 1 · Minor 2", evidence: "Known beacon", strength: 3)
        for i in 0...PhoneSightings.maximum {
            seen.record(id: "\(i)", match: match, rssi: -60, at: Date(timeIntervalSince1970: Double(i)), beaconUUID: "AB12")
        }
        XCTAssertEqual(seen.values.count, PhoneSightings.maximum)
        XCTAssertNil(seen.values["0"])
        XCTAssertTrue(seen.values["1"]?.contains("ab12") == true)
    }
}

private final class ApproximateBeaconLocation: CLLocationManager {
    override var authorizationStatus: CLAuthorizationStatus { .authorizedWhenInUse }
    override var accuracyAuthorization: CLAccuracyAuthorization { .reducedAccuracy }
}
