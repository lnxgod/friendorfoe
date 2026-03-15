import Foundation

enum ObjectCategory: String, Sendable {
    case commercial = "COMMERCIAL"
    case generalAviation = "GENERAL_AVIATION"
    case military = "MILITARY"
    case drone = "DRONE"
    case unknown = "UNKNOWN"
}

protocol SkyObject: Identifiable, Sendable {
    var id: String { get }
    var position: Position { get }
    var source: DetectionSource { get }
    var category: ObjectCategory { get }
    var confidence: Float { get }
    var firstSeen: Date { get }
    var lastUpdated: Date { get }
    var displayLabel: String { get }
    var displayDetail: String { get }
}
