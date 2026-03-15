import Foundation

enum DetectionSource: String, Sendable {
    case adsB = "ADS-B"
    case remoteId = "BLE"
    case wifi = "WiFi"
}
