import Foundation
import XCTest
@testable import FriendOrFoe

final class StationTests: XCTestCase {
    func testAcceptsHTTPSOriginAndPathWithoutCredentialsOrAlternateDestinations() throws {
        let base = try StationAddress.parse(" https://sensor.example.test:8443/fof/ ")
        XCTAssertEqual(StationFeed.devices.requestURL(base: base).absoluteString,
                       "https://sensor.example.test:8443/fof/detections/devices/live")
        XCTAssertEqual(StationFeed.networks.requestURL(base: base).absoluteString,
                       "https://sensor.example.test:8443/fof/detections/anomalies/recent?limit=200")
        for text in ["", "sensor.example.test", "http://sensor.example.test", "file:///tmp/test",
                     "https://user:token@sensor.example.test", "https://sensor.example.test?token=x",
                     "https://sensor.example.test/#section", "https://sensor.example.test/p/../other",
                     "https://sensor.example.test/%2fother", "https://sensor.example.test:65536"] {
            XCTAssertThrowsError(try StationAddress.parse(text), text)
        }
    }

    func testDecodesRecorderSignatureAndBeaconNetworkWithActualObservationTime() throws {
        let rows = try StationFeed.devices.decode(Data(#"""
        {"devices":[
          {"fingerprint":"FP:ABC","privacy_kind":"VOICE_RECORDER","current_rssi":-55,
           "last_seen":1700000000,"seen_by":["demo-node"],"display_detail":"Plaud; recording status unknown",
           "privacy_evidence":[{"field":"personal_recorder_signature","value":{"manufacturer":"Plaud","reason":"recorder:name:plaud_serial"}}]},
          {"fingerprint":"FP:ABC","privacy_kind":"VOICE_RECORDER","ble_name":"older","last_seen":1699999000},
          {"fingerprint":"FP:BEACON","privacy_kind":"VENUE_BEACON","last_seen":1700000001,
           "privacy_evidence":[{"field":"ibeacon_uuid","value":"00000000-0000-0000-0000-000000000001"}]},
          {"fingerprint":"FP:OTHER","privacy_kind":"PRIVACY_SIGNAL","current_rssi":-40}
        ],"summary":{}}
        """#.utf8))
        XCTAssertEqual(rows.count, 2)
        let recorder = try XCTUnwrap(rows.first { $0.category == .recorder })
        XCTAssertEqual(recorder.title, "Plaud recorder")
        XCTAssertEqual(recorder.recorderFamily, "Plaud")
        XCTAssertTrue(recorder.matches("plaud_serial"))
        XCTAssertEqual(recorder.rssi, -55)
        XCTAssertEqual(recorder.lastSeen, Date(timeIntervalSince1970: 1_700_000_000))
        XCTAssertEqual(rows.first?.beaconNetwork, "00000000-0000-0000-0000-000000000001")
        XCTAssertThrowsError(try StationFeed.devices.decode(Data(#"{"summary":{}}"#.utf8)))
        XCTAssertThrowsError(try StationFeed.devices.decode(Data(#"{"devices":[{"device_type":"Unknown"}]}"#.utf8)))
    }

    func testWiFiIncludesReportedSSIDAndReasonWithoutFillingMissingNames() throws {
        let rows = try StationFeed.networks.decode(Data(#"""
        {"alerts":[
          {"anomaly_type":"ssid_spoofing","detected_at":1700000001,"source":"wifi_ap","title":"SSID spoofing suspected",
           "message":"Demo guest mapped to multiple access points.","ssid":"Demo guest","device_id":"demo-sensor","entity_key":"ssid:demo"},
          {"anomaly_type":"bssid_churn","detected_at":1700000000,"source":"wifi_ap","title":"Network changed",
           "message":"A sensor observed changing access point identifiers.","ssid":"<unknown ssid>"},
          {"anomaly_type":"new_device","detected_at":1700000000,"source":"ble","title":"New device","message":"Nearby"},
          {"anomaly_type":"ssid_spoofing","detected_at":1700000000,"source":"wifi_probe","title":"Probe","message":"Excluded"}
        ]}
        """#.utf8))
        XCTAssertEqual(rows.count, 2)
        XCTAssertEqual(rows.first?.title, "Demo guest")
        XCTAssertEqual(rows.first?.detail, "Demo guest mapped to multiple access points.")
        XCTAssertEqual(rows.first?.evidence.first, "SSID spoofing suspected")
        XCTAssertNil(rows.last?.networkName)
        XCTAssertEqual(rows.last?.title, "SSID unavailable")
    }

    @MainActor
    func testPartialFailureRetainsPreviousTimestampAndChangingServerClearsReports() async throws {
        let root = URL.temporaryDirectory.appending(path: UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appending(path: "config/address")
        let reply = StationTestFeed()
        let station = StationStore(addressFile: file, read: { feed, _ in try await reply.read(feed) })
        XCTAssertNil(station.base)
        try station.configure("https://one.example.test")
        XCTAssertNil(station.devices.received)
        await station.refresh()
        let first = try XCTUnwrap(station.devices.received)
        XCTAssertEqual(station.devices.reports.count, 1)
        await reply.failDeviceRequests()
        await station.refresh()
        XCTAssertNotNil(station.devices.error)
        XCTAssertEqual(station.devices.received, first)
        XCTAssertEqual(station.devices.reports.count, 1)
        XCTAssertNil(station.networks.error)
        try station.configure("https://two.example.test")
        XCTAssertTrue(station.devices.reports.isEmpty)
        XCTAssertNil(station.devices.error)
        XCTAssertNil(station.networks.received)
        let restored = StationStore(addressFile: file)
        XCTAssertEqual(restored.base?.host, "two.example.test")
        XCTAssertNil(restored.devices.received)
        try station.forget()
        XCTAssertNil(station.base)
        XCTAssertNil(StationStore(addressFile: file).base)
    }
}

private actor StationTestFeed {
    private var failDevices = false

    func failDeviceRequests() { failDevices = true }

    func read(_ feed: StationFeed) throws -> [StationReport] {
        switch feed {
        case .devices:
            if failDevices { throw StationError.rejected(503) }
            return try feed.decode(Data(#"{"devices":[{"fingerprint":"FP:fixture","privacy_kind":"VOICE_RECORDER","ble_name":"PLAUD"}]}"#.utf8))
        case .networks: return try feed.decode(Data(#"{"alerts":[]}"#.utf8))
        }
    }
}
