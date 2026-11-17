import Foundation

enum SignalCategory: String, Codable, CaseIterable {
    case recorder = "Personal recorders"
    case camera = "Cameras"
    case glasses = "Smart glasses"
    case tracker = "Trackers"
    case drone = "Drone Remote ID"
    case everyday = "Everyday devices"
    case beacon = "iBeacons"

    var symbol: String {
        switch self {
        case .recorder: "waveform"
        case .camera: "video"
        case .glasses: "eyeglasses"
        case .tracker: "tag"
        case .drone: "antenna.radiowaves.left.and.right"
        case .everyday: "headphones"
        case .beacon: "dot.radiowaves.left.and.right"
        }
    }
}

struct BroadcastMatch: Codable, Equatable {
    let category: SignalCategory
    let family: String
    let evidence: String
    let strength: Int

    var title: String {
        category == .recorder ? "\(family) recorder" : family
    }
}

enum PhoneSignatures {
    struct Recorder {
        let brand: String
        let prefixes: [String]
        let service: String?
        init(_ brand: String, _ prefixes: [String], _ service: String? = nil) {
            self.brand = brand
            self.prefixes = prefixes
            self.service = service
        }
    }

    // Keep these tokens and matching boundaries aligned with Android's PersonalRecorderSignatures.
    static let recorders = [
        Recorder("Plaud", ["plaud", "notepin"]),
        Recorder("Omi", ["omi"], "19b10000-e8f2-537e-4f6c-d104768a1214"),
        Recorder("Limitless", ["limitless"], "632de001-604c-446b-a80f-7963e950f3fb"),
        Recorder("Bee", ["bee"], "03d5d5c4-a86c-11ee-9d89-8f2089a49e7e"),
        Recorder("Friend", ["friend_"], "1a3fd0e7-b1f3-ac9e-2e49-b647b2c4f8da"),
        Recorder("Fieldy", ["fieldy"]),
        Recorder("viaim", ["viaim recdot", "viaim opennote"]),
        Recorder("soundcore", ["soundcore work"]),
        Recorder("Notta", ["notta memo"]),
        Recorder("Instamic", ["instamic"]),
        Recorder("DJI", ["dji-mic2", "dji-mic3", "dji mic 2", "dji mic 3"])
    ]

    private struct Family {
        let category: SignalCategory
        let name: String
        let services: [String]
        let prefixes: [String]
        init(_ category: SignalCategory, _ name: String, _ services: [String], _ prefixes: [String]) {
            self.category = category
            self.name = name
            self.services = services
            self.prefixes = prefixes
        }
    }

    // Product or protocol evidence from Android's camera and accessory rules. Vendor names
    // alone and generic shared services do not identify camera or recorder hardware.
    private static let families = [
        Family(.camera, "Ricoh THETA camera", ["f74d1ba5-bc52-4f10-99c0-85e785ebe43f"], []),
        Family(.camera, "GoPro camera", [], ["gopro hero", "gopro max"]),
        Family(.camera, "Insta360 camera", [], ["insta360"]),
        Family(.camera, "DJI camera", [], ["osmo action", "dji action", "osmo pocket"]),
        Family(.camera, "Nest camera", [], ["nest cam", "nest hello", "nest doorbell"]),
        Family(.camera, "Wyze camera", [], ["wyze cam"]),
        Family(.camera, "Eufy camera", [], ["eufy indoor", "eufy doorbell", "eufy floodlight"]),
        Family(.camera, "Axon camera", [], ["axon body", "axon fleet"]),
        Family(.glasses, "Meta glasses", [], ["rb meta", "ray-ban meta", "ray-ban stories", "oakley meta"]),
        Family(.glasses, "Snap Spectacles", [], ["spectacles"]),
        Family(.glasses, "Bose audio glasses", [], ["bose frames"]),
        Family(.glasses, "Echo Frames", [], ["echo frames"]),
        Family(.glasses, "Solos glasses", [], ["solos airgo"]),
        Family(.tracker, "Find My accessory", [], ["airtag"]),
        Family(.tracker, "Samsung SmartTag", [], ["smarttag", "galaxy smarttag"]),
        Family(.tracker, "Tile tracker", [], ["tile"]),
        Family(.tracker, "Chipolo tracker", [], ["chipolo"]),
        Family(.tracker, "Pebblebee tracker", [], ["pebblebee"]),
        Family(.tracker, "Eufy SmartTrack", [], ["eufy smarttrack", "smarttrack"]),
        Family(.tracker, "Moto Tag", [], ["moto tag"]),
        Family(.everyday, "AirPods", [], ["airpods"]),
        Family(.everyday, "Bluetooth input device", ["1812"], []),
        Family(.everyday, "Bluetooth audio device", ["184e", "1850"], [])
    ]

    private static func canonicalService(_ service: String) -> String {
        let lower = service.lowercased()
        if lower.count == 36, lower.hasPrefix("0000"), lower.hasSuffix("-0000-1000-8000-00805f9b34fb") {
            return String(lower.dropFirst(4).prefix(4))
        }
        return lower
    }

    private static func hasName(_ name: String, prefix: String) -> Bool {
        guard name.hasPrefix(prefix) else { return false }
        let next = name.dropFirst(prefix.count).first
        return next == nil || prefix.hasSuffix("_") || next == " " || next == "-" || next == "_"
    }

    // Legacy Bluetooth ASTM prefix + standalone 25-byte Basic ID. Verify only the
    // serial/registration text form. Never store or include an aircraft ID in the evidence.
    private static func isLegacyBasicID(_ data: Data) -> Bool {
        guard data.count == 27 else { return false }
        let bytes = Array(data)
        guard bytes[0] == 0x0d, bytes[2] <= 2, [1, 2].contains(bytes[3] >> 4),
              bytes[24...26].allSatisfy({ $0 == 0 }) else { return false }
        let field = bytes[4..<24]
        let identifier = field.prefix(while: { $0 != 0 })
        return identifier.contains(where: { $0 > 0x20 }) &&
            identifier.allSatisfy { (0x20...0x7e).contains($0) } &&
            field.dropFirst(identifier.count).allSatisfy { $0 == 0 }
    }

    static func match(name: String?, services: [String] = [], serviceData: [String: Data] = [:],
                      manufacturerData: Data? = nil) -> BroadcastMatch? {
        let ids = Set((services + Array(serviceData.keys)).map(canonicalService))
        if serviceData.contains(where: { canonicalService($0.key) == "fffa" && isLegacyBasicID($0.value) }) {
            return BroadcastMatch(category: .drone, family: "Drone Remote ID",
                                  evidence: "Bluetooth service FFFA carries a Basic ID broadcast.", strength: 3)
        }
        let displayName = String((name ?? "").trimmingCharacters(in: .whitespacesAndNewlines).prefix(100))
        let lower = displayName.lowercased()
        for recorder in recorders {
            if let service = recorder.service, ids.contains(service) {
                return BroadcastMatch(category: .recorder, family: recorder.brand,
                                      evidence: "Advertised recorder service \(service)", strength: 3)
            }
        }
        // Open GoPro documents "GoPro " + four serial digits as the default camera name.
        // Pair it with the advertised discovery service to avoid classifying accessories.
        if ids.contains("fea6"), lower.hasPrefix("gopro "), lower.utf8.count == 10,
           lower.utf8.suffix(4).allSatisfy({ (48...57).contains($0) }) {
            return BroadcastMatch(category: .camera, family: "GoPro camera",
                                  evidence: "Advertised name “\(displayName)” and GoPro service FEA6", strength: 3)
        }
        for family in families where family.category != .everyday {
            if let service = family.services.first(where: ids.contains) {
                return BroadcastMatch(category: family.category, family: family.name,
                                      evidence: "Advertised service \(service.uppercased())", strength: 3)
            }
        }
        let plaudSerial = lower.utf8.count == 9 && lower.hasPrefix("plaud") &&
            lower.utf8.dropFirst(5).allSatisfy { (97...122).contains($0) || (48...57).contains($0) }
        for recorder in recorders where (recorder.brand == "Plaud" && plaudSerial) ||
            recorder.prefixes.contains(where: { hasName(lower, prefix: $0) }) {
            return BroadcastMatch(category: .recorder, family: recorder.brand,
                                  evidence: "Advertised name “\(displayName)”", strength: 2)
        }
        for family in families where family.prefixes.contains(where: { hasName(lower, prefix: $0) }) {
            return BroadcastMatch(category: family.category, family: family.name,
                                  evidence: "Advertised name “\(displayName)”", strength: 2)
        }
        for family in families where family.category == .everyday {
            if let service = family.services.first(where: ids.contains) {
                return BroadcastMatch(category: family.category, family: family.name,
                                      evidence: "Advertised service \(service.uppercased())", strength: 1)
            }
        }
        if let manufacturerData {
            let bytes = Array(manufacturerData)
            if bytes.count >= 5, bytes[0] == 0x4c, bytes[1] == 0,
               bytes[3] > 0, bytes.count >= 4 + Int(bytes[3]) {
                switch bytes[2] {
                case 0x12 where bytes[3] == 0x19:
                    return BroadcastMatch(category: .tracker, family: "Find My accessory",
                                          evidence: "Apple Find My broadcast (type 12, length 25)", strength: 2)
                case 0x07:
                    return BroadcastMatch(category: .everyday, family: "AirPods or Beats accessory",
                                          evidence: "Apple proximity-pairing broadcast (type 07)", strength: 1)
                case 0x10, 0x0f:
                    return BroadcastMatch(category: .everyday, family: "Apple Continuity",
                                          evidence: "Apple Continuity broadcast", strength: 1)
                default: break
                }
            }
        }
        return nil
    }
}

struct PhoneSighting: Identifiable, Codable, Equatable {
    let id: String
    let firstSeen: Date
    var lastSeen: Date
    var count: Int
    var rssi: Int?
    var match: BroadcastMatch
    var beaconUUID: String?
    var title: String { match.title }

    func contains(_ query: String) -> Bool {
        query.isEmpty || [title, match.evidence, match.category.rawValue, beaconUUID ?? ""].contains {
            $0.localizedCaseInsensitiveContains(query)
        }
    }
}

struct PhoneSightings {
    static let maximum = 300
    private(set) var values: [String: PhoneSighting] = [:]

    mutating func record(id: String, match: BroadcastMatch, rssi: Int?, at time: Date, beaconUUID: String? = nil) {
        if var value = values[id] {
            // A weaker, unrelated frame must not refresh a previous device-family claim.
            if value.lastSeen.addingTimeInterval(60) > time && value.match.family != match.family &&
                value.match.strength > match.strength { return }
            if value.match.family != match.family {
                values.removeValue(forKey: id)
            } else {
                value.lastSeen = time
                value.count += 1
                value.rssi = rssi
                if match.strength >= value.match.strength { value.match = match }
                values[id] = value
                return
            }
        }
        if values.count >= Self.maximum, let oldest = values.min(by: { $0.value.lastSeen < $1.value.lastSeen })?.key {
            values.removeValue(forKey: oldest)
        }
        values[id] = PhoneSighting(id: id, firstSeen: time, lastSeen: time, count: 1, rssi: rssi,
                                   match: match, beaconUUID: beaconUUID)
    }

    mutating func remove(id: String) { values.removeValue(forKey: id) }
}
