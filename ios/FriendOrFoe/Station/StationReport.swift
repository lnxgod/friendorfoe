import Foundation

enum StationCategory: String, CaseIterable, Identifiable {
    case recorder = "Personal recorders"
    case wifi = "Wi-Fi alerts"
    case camera = "Cameras and glasses"
    case tracker = "Trackers"
    case beacon = "Beacons"
    case everyday = "Everyday devices"
    case other = "Other privacy signals"

    var id: Self { self }

    var symbol: String {
        switch self {
        case .recorder: return "waveform"
        case .wifi: return "wifi.exclamationmark"
        case .camera: return "camera"
        case .tracker: return "dot.radiowaves.left.and.right"
        case .beacon: return "antenna.radiowaves.left.and.right"
        case .everyday: return "headphones"
        case .other: return "sensor"
        }
    }
}

struct StationReport: Identifiable, Equatable {
    let id: String
    let category: StationCategory
    let recorderFamily: String?
    let title: String
    let detail: String
    let lastSeen: Date?
    let networkName: String?
    let beaconNetwork: String?
    let rssi: Int?
    let sensor: String?
    let evidence: [String]

    func matches(_ query: String) -> Bool {
        let term = query.trimmingCharacters(in: .whitespacesAndNewlines)
        return term.isEmpty || ([title, detail, category.rawValue, recorderFamily ?? "", networkName ?? "", beaconNetwork ?? ""] + evidence)
            .contains { $0.localizedCaseInsensitiveContains(term) } || sensor?.localizedCaseInsensitiveContains(term) == true
    }
}

enum StationFeed: String, CaseIterable {
    case devices = "Devices"
    case networks = "Wi-Fi alerts"

    func requestURL(base: URL) -> URL {
        switch self {
        case .devices: return base.appending(path: "detections/devices/live")
        case .networks:
            return base.appending(path: "detections/anomalies/recent")
                .appending(queryItems: [URLQueryItem(name: "limit", value: "200")])
        }
    }

    func decode(_ data: Data) throws -> [StationReport] {
        // The backend evidence array contains heterogeneous values, including nested recorder signatures.
        let key = self == .devices ? "devices" : "alerts"
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let records = envelope[key] as? [[String: Any]] else { throw StationError.incompatible }
        if self == .devices, !records.isEmpty, !records.contains(where: { $0["privacy_kind"] is String }) {
            throw StationError.incompatible
        }

        let reports = records.compactMap { self == .devices ? Self.device($0) : Self.alert($0) }
        let sorted = reports.sorted {
            if $0.lastSeen == $1.lastSeen { return $0.id < $1.id }
            return ($0.lastSeen ?? .distantPast) > ($1.lastSeen ?? .distantPast)
        }
        var ids = Set<String>()
        return sorted.filter { ids.insert($0.id).inserted }
    }

    private static func device(_ row: [String: Any]) -> StationReport? {
        guard let kind = string(row["privacy_kind"]),
              let fingerprint = string(row["fingerprint"]) else { return nil }
        let category: StationCategory
        let genericTitle: String
        switch kind {
        case "VOICE_RECORDER": (category, genericTitle) = (.recorder, "Personal recorder")
        case "META_GLASSES": (category, genericTitle) = (.camera, "Smart glasses")
        case "CAMERA_NEAR", "FLOCK_ALPR": (category, genericTitle) = (.camera, "Camera signal")
        case "TRACKER_NEAR": (category, genericTitle) = (.tracker, "Tracker signal")
        case "VENUE_BEACON": (category, genericTitle) = (.beacon, "Venue beacon")
        case "APPLE_CONTINUITY": (category, genericTitle) = (.everyday, "Apple device activity")
        case "AURACAST": (category, genericTitle) = (.everyday, "Broadcast audio")
        case "BLE_HID": (category, genericTitle) = (.everyday, "Bluetooth input device")
        case "MOBILE_KEY_LOCK": (category, genericTitle) = (.everyday, "Mobile key or lock")
        case "EVENT_BADGE": (category, genericTitle) = (.other, "Event badge")
        case "REMOTE_LISTENING": (category, genericTitle) = (.other, "Possible listening device")
        case "SECURITY_INFRASTRUCTURE": (category, genericTitle) = (.other, "Security infrastructure")
        default: return nil
        }
        var fields = [String: Any]()
        for item in (row["privacy_evidence"] ?? row["evidence"]) as? [[String: Any]] ?? [] {
            if let key = item["field"] as? String { fields[key] = item["value"] }
        }
        let signature = fields["personal_recorder_signature"] as? [String: Any]
        let name = string(row["ble_name"] ?? fields["ble_name"])
        let manufacturer = string(signature?["manufacturer"])
        let title = name ?? manufacturer.map { "\($0) recorder" } ?? genericTitle
        let serviceIDs = string(row["ble_svc_uuids"] ?? fields["ble_svc_uuids"])
        var evidence = [String]()
        if let name { evidence.append("Broadcast name · \(name)") }
        if let serviceIDs { evidence.append("Advertised services · \(serviceIDs)") }
        if let source = string(row["source"] ?? fields["source"]) { evidence.append("Scanner source · \(source)") }
        if let reason = string(signature?["reason"] ?? row["class_reason"] ?? fields["class_reason"]) {
            evidence.append("Scanner match · \(reason)")
        }
        let network = networkName(row["ssid"] ?? fields["ssid"])
        let uuid = string(row["ibeacon_uuid"] ?? fields["ibeacon_uuid"] ?? row["beacon_uuid"] ?? fields["beacon_uuid"])
            .flatMap(UUID.init(uuidString:))?.uuidString
        let detail = string(row["display_detail"]) ?? genericTitle
        let rssi = row["current_rssi"] as? Int
        return StationReport(
            id: "device/\(fingerprint)", category: category, recorderFamily: manufacturer, title: title, detail: detail,
            lastSeen: date(row["last_seen"]), networkName: network, beaconNetwork: uuid,
            rssi: rssi.flatMap { (-127 ... -1).contains($0) ? $0 : nil },
            sensor: (row["seen_by"] as? [String])?.compactMap { string($0) }.sorted().joined(separator: ", "),
            evidence: evidence
        )
    }

    private static func alert(_ row: [String: Any]) -> StationReport? {
        guard let kind = string(row["anomaly_type"]), let timestamp = date(row["detected_at"]),
              let source = string(row["source"]), !source.lowercased().contains("probe"),
              source.lowercased().contains("wifi") || ["ssid_spoofing", "bssid_churn"].contains(kind),
              let title = string(row["title"]), let detail = string(row["message"]) else { return nil }
        let network = networkName(row["ssid"])
        let sensor = string(row["device_id"])
        let identity = string(row["entity_key"]) ?? network ?? title
        let id = [kind, identity, sensor ?? "", String(timestamp.timeIntervalSince1970)].joined(separator: "/")
        return StationReport(
            id: "network/\(id)", category: .wifi, recorderFamily: nil, title: network ?? "SSID unavailable", detail: detail,
            lastSeen: timestamp, networkName: network, beaconNetwork: nil, rssi: nil, sensor: sensor,
            evidence: [title, "Scanner source · \(source)"]
        )
    }

    private static func networkName(_ value: Any?) -> String? {
        guard let text = string(value), text.lowercased() != "<unknown ssid>" else { return nil }
        return text
    }

    private static func string(_ value: Any?) -> String? {
        guard let raw = value as? String else { return nil }
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text
    }

    private static func date(_ value: Any?) -> Date? {
        guard let seconds = value as? Double, seconds.isFinite, seconds > 0 else { return nil }
        return Date(timeIntervalSince1970: seconds)
    }
}
