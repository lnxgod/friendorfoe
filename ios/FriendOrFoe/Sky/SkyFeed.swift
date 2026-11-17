import CoreFoundation
import CoreLocation
import Foundation

struct SkyPoint: Codable, Hashable {
    let latitude: Double
    let longitude: Double

    init?(latitude: Double, longitude: Double) {
        guard latitude.isFinite, longitude.isFinite,
              (-90...90).contains(latitude), (-180...180).contains(longitude) else { return nil }
        self.latitude = latitude
        self.longitude = longitude
    }

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }

    func miles(to other: SkyPoint) -> Double {
        let start = CLLocation(latitude: latitude, longitude: longitude)
        let end = CLLocation(latitude: other.latitude, longitude: other.longitude)
        return start.distance(from: end) / 1609.344
    }
}

struct SkyArea: Equatable {
    let center: SkyPoint

    init?(latitude: Double, longitude: Double) {
        guard let position = SkyPoint(latitude: latitude, longitude: longitude),
              let rounded = SkyPoint(latitude: (position.latitude * 10).rounded() / 10,
                                     longitude: (position.longitude * 10).rounded() / 10) else { return nil }
        self.center = rounded
    }

    func url(radiusMiles: Int) -> URL? {
        let lat = String(format: "%.1f", locale: Locale(identifier: "en_US_POSIX"), center.latitude)
        let lon = String(format: "%.1f", locale: Locale(identifier: "en_US_POSIX"), center.longitude)
        let nauticalMiles = Int(ceil(Double(min(50, max(1, radiusMiles))) * 1609.344 / 1852))
        return URL(string: "https://api.adsb.lol/v2/point/\(lat)/\(lon)/\(nauticalMiles)")
    }
}

struct SkyAircraft: Codable, Identifiable, Hashable {
    let id: String
    let callsign: String?
    let typeCode: String?
    let registration: String?
    let point: SkyPoint
    let altitudeFeet: Int?
    let speedKnots: Double?
    let onGround: Bool
    let observedAt: Date

    var name: String { callsign ?? registration ?? id.uppercased() }
}

struct SkyTrailPoint: Codable, Hashable {
    let point: SkyPoint
    let time: Date
}

struct SkyTrails {
    private(set) var points: [String: [SkyTrailPoint]] = [:]
    private static let lifetime: TimeInterval = 30 * 60

    mutating func record(_ aircraft: [SkyAircraft], now: Date) {
        let oldest = now.addingTimeInterval(-Self.lifetime)
        points = points.compactMapValues { history in
            let recent = history.filter { $0.time >= oldest }
            return recent.isEmpty ? nil : recent
        }
        for plane in aircraft where plane.observedAt >= oldest {
            var history = points[plane.id, default: []]
            if let last = history.last {
                guard plane.observedAt > last.time else { continue }
                if last.point == plane.point { history.removeLast() }
            }
            history.append(SkyTrailPoint(point: plane.point, time: plane.observedAt))
            points[plane.id] = Array(history.suffix(90))
        }
    }
}

struct SkySavedFlight: Codable, Identifiable {
    let id: UUID
    let aircraft: SkyAircraft
    let trail: [SkyTrailPoint]
    let savedAt: Date
}

enum SkyFeedError: LocalizedError {
    case invalidResponse, staleResponse, unavailable, rateLimited, oversized
    var errorDescription: String? {
        switch self {
        case .invalidResponse: return "The service returned unreadable aircraft data."
        case .staleResponse: return "The service returned an out-of-date aircraft report."
        case .unavailable: return "The aircraft service is unavailable."
        case .rateLimited: return "The aircraft service has reached its request limit. Try again later."
        case .oversized: return "The aircraft response was too large."
        }
    }
}

enum SkyFeed {
    static func nearby(_ data: Data, now: Date, center: SkyPoint, radiusMiles: Int) async throws -> [SkyAircraft] {
        let reports = try decode(data, now: now)
        let distances = reports.map { plane in
            (plane: plane, miles: center.miles(to: plane.point))
        }
        let inRange = distances.filter { $0.miles <= Double(radiusMiles) }
        return inRange.sorted { left, right in
            if left.miles == right.miles { return left.plane.id < right.plane.id }
            return left.miles < right.miles
        }.map(\.plane)
    }

    static func decode(_ data: Data, now: Date) throws -> [SkyAircraft] {
        guard data.count <= 8_000_000 else { throw SkyFeedError.oversized }
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let rows = json["ac"] as? [Any], rows.count <= 15_000,
              let time = number(json["now"]), time > 0 else { throw SkyFeedError.invalidResponse }
        guard rows.isEmpty || rows.contains(where: { ($0 as? [String: Any])?["hex"] is String }) else {
            throw SkyFeedError.invalidResponse
        }
        let timeSent = Date(timeIntervalSince1970: time > 1e11 ? time / 1000 : time)
        guard (-60...300).contains(now.timeIntervalSince(timeSent)) else { throw SkyFeedError.staleResponse }
        var distinct: [String: SkyAircraft] = [:]
        for case let row as [String: Any] in rows {
            guard let id = string(row["hex"])?.lowercased(),
                  id.range(of: "^~?[0-9a-f]{6}$", options: .regularExpression) != nil,
                  let lat = number(row["lat"]), let lon = number(row["lon"]),
                  let point = SkyPoint(latitude: lat, longitude: lon),
                  let age = number(row["seen_pos"]), (0...300).contains(age) else { continue }
            let observedAt = timeSent.addingTimeInterval(-age)
            guard now.timeIntervalSince(observedAt) <= 300,
                  distinct[id].map({ $0.observedAt < observedAt }) ?? true else { continue }
            let onGround = string(row["alt_baro"])?.lowercased() == "ground"
            let altitude = number(row["alt_baro"]) ?? number(row["alt_geom"])
            let validAltitude = altitude.flatMap { (-2000...150000).contains($0) ? Int($0.rounded()) : nil }
            let speed = number(row["gs"]).flatMap { (0...3000).contains($0) ? $0 : nil }
            distinct[id] = SkyAircraft(id: id, callsign: string(row["flight"]), typeCode: string(row["t"]),
                                       registration: string(row["r"]), point: point, altitudeFeet: validAltitude,
                                       speedKnots: speed, onGround: onGround, observedAt: observedAt)
        }
        return Array(distinct.values)
    }

    private static func number(_ value: Any?) -> Double? {
        guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              number.doubleValue.isFinite else { return nil }
        return number.doubleValue
    }

    private static func string(_ value: Any?) -> String? {
        guard let raw = value as? String else { return nil }
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty || trimmed.count > 32 || trimmed.rangeOfCharacter(from: .controlCharacters) != nil
            ? nil : trimmed
    }
}
