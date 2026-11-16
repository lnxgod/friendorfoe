import XCTest
@testable import FriendOrFoe

final class MagneticFieldTests: XCTestCase {
    func testComparisonUsesTotalFieldAndWaitsForHighAccuracy() {
        var sweep = FieldSweepReading()
        sweep.accept(x: 3, y: 4, z: 12, accuracy: .medium)
        XCTAssertEqual(sweep.magnitude, 13)
        XCTAssertNil(sweep.baseline)
        sweep.accept(x: -12, y: 3, z: -4, accuracy: .high)
        XCTAssertEqual(sweep.baseline, 13)
        sweep.accept(x: 0, y: 0, z: -9, accuracy: .high)
        XCTAssertEqual(sweep.change, -4)
        XCTAssertEqual(sweep.peakChange, 4)
        sweep.accept(x: 16, y: 0, z: 0, accuracy: .high)
        XCTAssertEqual(sweep.change, 3)
        XCTAssertEqual(sweep.peakChange, 4)
        sweep.accept(x: 100, y: 0, z: 0, accuracy: .low)
        XCTAssertEqual(sweep.change, 87)
        XCTAssertEqual(sweep.peakChange, 4)
    }

    func testUncalibratedAndInvalidSamplesDoNotCreatePeaksOrKeepStaleReadings() {
        var sweep = FieldSweepReading()
        sweep.accept(x: 2, y: 0, z: 0, accuracy: .high)
        for (x, accuracy): (Double, FieldAccuracy) in [(8, .uncalibrated), (.nan, .high), (.infinity, .high)] {
            sweep.accept(x: x, y: 0, z: 0, accuracy: accuracy)
            XCTAssertNil(sweep.magnitude)
            XCTAssertNil(sweep.change)
            XCTAssertEqual(sweep.baseline, 2)
            XCTAssertEqual(sweep.peakChange, 0)
        }
        sweep.resetBaseline()
        sweep.accept(x: 3, y: 0, z: 0, accuracy: .low)
        XCTAssertNil(sweep.baseline)
        sweep.accept(x: 8, y: 0, z: 0, accuracy: .high)
        XCTAssertEqual(sweep.baseline, 8)
        XCTAssertEqual(sweep.change, 0)
        XCTAssertEqual(sweep.peakChange, 0)
    }
}
