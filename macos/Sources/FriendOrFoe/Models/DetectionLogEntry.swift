import Foundation

struct DetectionLogEntry: Identifiable, Sendable {
    let id: String
    let timestamp: Date
    let source: DetectionSource
    let label: String
    let detail: String
    let isOnMap: Bool

    init(timestamp: Date, source: DetectionSource, label: String, detail: String, isOnMap: Bool) {
        self.id = "\(source.rawValue)_\(label)"
        self.timestamp = timestamp
        self.source = source
        self.label = label
        self.detail = detail
        self.isOnMap = isOnMap
    }
}
