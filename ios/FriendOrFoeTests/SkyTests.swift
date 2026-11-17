import XCTest
@testable import FriendOrFoe

final class SkyTests: XCTestCase {
    let instant = Date(timeIntervalSince1970: 1_730_000_000)

    func testOnlyRoundedCoordinatesReachProviderAndRadiusUsesNauticalMiles() throws {
        let area = try XCTUnwrap(SkyArea(latitude: -26.234567, longitude: 53.987654))
        XCTAssertEqual(area.center.latitude, -26.2)
        XCTAssertEqual(area.center.longitude, 54)
        XCTAssertEqual(area.url(radiusMiles: 10)?.absoluteString, "https://api.adsb.lol/v2/point/-26.2/54.0/9")
        XCTAssertEqual(area.url(radiusMiles: 100)?.lastPathComponent, "44")
        XCTAssertNil(SkyArea(latitude: .nan, longitude: 10))
        XCTAssertNil(SkyArea(latitude: 91, longitude: 10))
        XCTAssertNil(SkyArea(latitude: 10, longitude: 181))
    }

    func testParserKeepsFreshestValidPositionsAndGroundReports() throws {
        let rows: [[String: Any]] = [
            ["hex":"aabbcc","flight":"  OLD  ","lat":0,"lon":0,"seen_pos":10],
            ["hex":"AABBCC","flight":"  TEST  ","lat":0,"lon":1,"alt_baro":"ground","seen_pos":4],
            ["hex":"~123abc","lat":0,"lon":0,"seen_pos":2,"alt_baro":30000,"gs":170],
            ["hex":"12","lat":0,"lon":0,"seen_pos":1],
            ["hex":"123456","lat":true,"lon":1,"seen_pos":1],
            ["hex":"123457","lat":94,"lon":1,"seen_pos":1],
            ["hex":"123458","lat":0,"lon":1,"seen_pos":301]
        ]
        let data = try JSONSerialization.data(withJSONObject: ["now":instant.timeIntervalSince1970 * 1000, "ac":rows])
        let decoded = try SkyFeed.decode(data, now: instant)
        XCTAssertEqual(decoded.count, 2)
        let ground = try XCTUnwrap(decoded.first { $0.id == "aabbcc" })
        XCTAssertEqual(ground.name, "TEST")
        XCTAssertTrue(ground.onGround)
        XCTAssertNil(ground.altitudeFeet)
        XCTAssertEqual(ground.observedAt, instant.addingTimeInterval(-4))
        let air = try XCTUnwrap(decoded.first { $0.id == "~123abc" })
        XCTAssertEqual(air.altitudeFeet, 30_000)
        XCTAssertEqual(air.speedKnots, 170)
    }

    func testEmptyFeedIsValidWhileMissingOrStaleFeedIsAnError() throws {
        func packet(_ json: [String: Any]) throws -> [SkyAircraft] {
            try SkyFeed.decode(JSONSerialization.data(withJSONObject: json), now: instant)
        }
        XCTAssertTrue(try packet(["now":instant.timeIntervalSince1970, "ac":[]]).isEmpty)
        XCTAssertThrowsError(try packet(["now":instant.timeIntervalSince1970]))
        XCTAssertThrowsError(try packet(["now":instant.timeIntervalSince1970, "ac":["unexpected"]]))
        XCTAssertThrowsError(try packet(["now":instant.timeIntervalSince1970 - 600, "ac":[]]))
        XCTAssertThrowsError(try packet(["now":instant.timeIntervalSince1970 + 600, "ac":[]]))
    }

    func testNearbyFiltersAndSortsFromTheRoundedAreaAcrossDateLine() async throws {
        let rows: [[String: Any]] = [
            ["hex":"aaaaaa", "lat":0, "lon":-179.98, "seen_pos":1],
            ["hex":"bbbbbb", "lat":0, "lon":179.99, "seen_pos":1],
            ["hex":"cccccc", "lat":0, "lon":170, "seen_pos":1]
        ]
        let data = try JSONSerialization.data(withJSONObject: ["now":instant.timeIntervalSince1970, "ac":rows])
        let center = try XCTUnwrap(SkyArea(latitude: 0, longitude: 179.983)).center
        let near = try await SkyFeed.nearby(data, now: instant, center: center, radiusMiles: 5)
        XCTAssertEqual(near.map(\.id), ["bbbbbb", "aaaaaa"])
    }

    func testSessionTrailDeduplicatesPositionsAndExpiresAcrossAircraft() throws {
        let zero = try XCTUnwrap(SkyPoint(latitude: 0, longitude: 0))
        let moved = try XCTUnwrap(SkyPoint(latitude: 0, longitude: 0.1))
        func plane(_ point: SkyPoint, _ seconds: Double) -> SkyAircraft {
            SkyAircraft(id: "abcdef", callsign: nil, typeCode: nil, registration: nil, point: point,
                        altitudeFeet: 5000, speedKnots: nil, onGround: false, observedAt: instant.addingTimeInterval(seconds))
        }
        var trails = SkyTrails()
        for (point, time) in [(zero,0.0), (zero,5.0), (moved,8.0), (zero,1.0)] {
            trails.record([plane(point, time)], now: instant.addingTimeInterval(10))
        }
        XCTAssertEqual(trails.points["abcdef"]?.map(\.time), [5.0,8.0].map { instant.addingTimeInterval($0) })
        trails.record([], now: instant.addingTimeInterval(2000))
        XCTAssertTrue(trails.points.isEmpty)
    }

    @MainActor
    func testSavedFlightsSurviveRelaunchAndCanBeDeletedOffline() throws {
        let folder = FileManager.default.temporaryDirectory.appending(path: UUID().uuidString)
        let url = folder.appending(path: "flights.json")
        defer { try? FileManager.default.removeItem(at: folder) }
        let store = SkyStore(archiveURL: url)
        XCTAssertEqual(store.state, .off)
        XCTAssertFalse(store.enabled)
        let plane = SkyAircraft(id: "abcdef", callsign: "Example", typeCode: nil, registration: nil,
                                point: try XCTUnwrap(SkyPoint(latitude: 0, longitude: 0)), altitudeFeet: 5000,
                                speedKnots: nil, onGround: false, observedAt: instant)
        store.save(plane)
        store.save(plane)
        let restored = SkyStore(archiveURL: url)
        XCTAssertEqual(restored.saved.count, 1)
        XCTAssertEqual(restored.saved.first?.aircraft, plane)
        XCTAssertNil(restored.area)
        restored.removeSaved(Set(restored.saved.map(\.id)))
        XCTAssertTrue(SkyStore(archiveURL: url).saved.isEmpty)
    }
}
