import Foundation
import CoreLocation

struct Position: Equatable, Sendable {
    let latitude: Double
    let longitude: Double
    let altitudeMeters: Double

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }

    var altitudeFeet: Int {
        Int(altitudeMeters * 3.281)
    }

    static let zero = Position(latitude: 0, longitude: 0, altitudeMeters: 0)
}
