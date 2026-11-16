import XCTest
@testable import FriendOrFoe

final class ToolsTests: XCTestCase {
    func testBandSeparatesLowAndNearUltrasonicTonesAndHonorsNyquist() throws {
        let analyzer = try XCTUnwrap(SoundAnalyzer())
        func tone(_ frequency: Double, rate: Double = 48_000) -> SoundReading {
            let samples = (0..<2048).map { Float(0.5 * sin(2 * .pi * Double($0) * frequency / rate)) }
            return samples.withUnsafeBufferPointer {
                analyzer.analyze(samples: $0.baseAddress!, count: $0.count, rate: rate)
            }
        }
        let low = tone(1500), high = tone(19_500)
        XCTAssertEqual(try XCTUnwrap(low.level), -9.03, accuracy: 0.2)
        XCTAssertEqual(try XCTUnwrap(high.band), -9.03, accuracy: 0.6)
        XCTAssertGreaterThan(try XCTUnwrap(high.band) - XCTUnwrap(low.band), 45)
        XCTAssertNil(tone(1000, rate: 16_000).band)
    }

    func testBundledGuideReusesApprovedPhotosAndMatchesTypeCodes() throws {
        let guide = ReferenceEntry.catalog
        XCTAssertGreaterThan(guide.filter { $0.kind == "Drone" }.count, 100)
        let boeing = try XCTUnwrap(guide.first { $0.matches("B738") })
        XCTAssertTrue(boeing.name.contains("737"))
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(boeing.photoURL).path))
        XCTAssertFalse(guide.contains { $0.photo == "aircraft/A345.jpg" })
        let resources = try XCTUnwrap(Bundle.main.resourceURL)
        var bundled = Set<String>()
        for folder in ["aircraft", "drones"] {
            let files = try FileManager.default.contentsOfDirectory(atPath: resources.appending(path: folder).path)
            bundled.formUnion(files.map { "\(folder)/\($0)" })
        }
        XCTAssertEqual(bundled, Set(guide.compactMap(\.photo)))
    }
}
