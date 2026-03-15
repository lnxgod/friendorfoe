import Foundation

struct Drone: SkyObject, Sendable {
    let id: String
    let position: Position
    let source: DetectionSource
    let category: ObjectCategory = .drone
    let confidence: Float
    let firstSeen: Date
    let lastUpdated: Date

    let droneId: String
    let manufacturer: String?
    let model: String?
    let ssid: String?
    let signalStrengthDbm: Int?
    let estimatedDistanceMeters: Double?

    var displayLabel: String {
        let name = manufacturer ?? "Drone"
        let conf = source == .wifi ? " ?" : ""
        return "\(name) \(position.altitudeFeet)ft\(conf)"
    }

    var displayDetail: String {
        let name: String
        if let mfg = manufacturer, let mdl = model {
            name = "\(mfg) \(mdl)"
        } else {
            name = manufacturer ?? "Unknown drone"
        }
        let sourceLabel = source == .remoteId ? "Remote ID" : "WiFi (low confidence)"
        return "\(name) | \(sourceLabel) | ID: \(String(droneId.prefix(12)))"
    }
}
