import Foundation

struct Aircraft: SkyObject, Sendable {
    let id: String
    let position: Position
    let source: DetectionSource = .adsB
    let category: ObjectCategory
    let confidence: Float
    let firstSeen: Date
    let lastUpdated: Date

    let icaoHex: String
    let callsign: String?
    let registration: String?
    let aircraftType: String?
    let aircraftModel: String?
    let airline: String?
    let origin: String?
    let destination: String?
    let squawk: String?
    let isOnGround: Bool
    let distanceNm: Double?
    let headingDeg: Double?
    let speedKts: Double?

    var displayLabel: String {
        let name = callsign ?? icaoHex
        let type = aircraftType ?? ""
        return "\(name) \(type) \(position.altitudeFeet)ft".trimmingCharacters(in: .whitespaces)
    }

    var displayDetail: String {
        let name = callsign ?? icaoHex
        let model = aircraftModel ?? aircraftType ?? "Unknown type"
        let route: String
        if let o = origin, let d = destination {
            route = " \(o) → \(d)"
        } else {
            route = ""
        }
        return "\(name) | \(model)\(route)"
    }
}
