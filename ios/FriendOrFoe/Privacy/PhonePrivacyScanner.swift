import CoreBluetooth
import CoreLocation
import Combine
import UIKit

private struct PrivacyPreferences: Codable {
    var knownBeacons: [UUID] = []
    var ignored: Set<String> = []
    var saved: [PhoneSighting] = []
}

final class PhonePrivacyScanner: NSObject, ObservableObject, CBCentralManagerDelegate, CLLocationManagerDelegate {
    @Published private(set) var scanning = false
    @Published private(set) var starting = false
    @Published private(set) var bluetoothStatus = "Ready to scan"
    @Published private(set) var needsBluetoothSettings = false
    @Published private(set) var beaconStatus = ""
    @Published private(set) var ranging = false
    @Published private(set) var needsLocationSettings = false
    @Published private(set) var rows: [PhoneSighting] = []
    @Published private var preferences = PrivacyPreferences()
    @Published var storageMessage: String?

    private var central: CBCentralManager?
    private var locator: CLLocationManager?
    private var wantsBluetooth = false
    private var wantsBeacons = false
    private var constraints: [CLBeaconIdentityConstraint] = []
    private var ledger = PhoneSightings()
    private var pendingUpdate = false
    private var flushTimer: Timer?

    var knownBeacons: [UUID] { preferences.knownBeacons }
    var saved: [PhoneSighting] { preferences.saved }
    var ignoredCount: Int { preferences.ignored.count }

    private let file: URL

    init(storageDirectory: URL = URL.applicationSupportDirectory.appending(path: "PhonePrivacy", directoryHint: .isDirectory)) {
        file = storageDirectory.appending(path: "preferences.json")
        super.init()
        do {
            preferences = try JSONDecoder().decode(PrivacyPreferences.self, from: Data(contentsOf: file))
        } catch let error as NSError where error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError {
            // A new installation has no saved observations or beacon identifiers.
        } catch {
            storageMessage = "Saved scanner settings could not be read."
        }
        NotificationCenter.default.addObserver(self, selector: #selector(stopForBackground),
                                               name: UIApplication.didEnterBackgroundNotification, object: nil)
    }

    private func persist(_ next: PrivacyPreferences) {
        do {
            var directory = file.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try directory.setResourceValues(values)
            try JSONEncoder().encode(next).write(to: file, options: [.atomic, .completeFileProtection])
            preferences = next
        } catch {
            storageMessage = "Could not save this change on the iPhone."
        }
    }

    func save(_ row: PhoneSighting) {
        var next = preferences
        next.saved.removeAll { $0.id == row.id }
        next.saved.insert(row, at: 0)
        persist(next)
    }

    func forgetSaved(_ id: String) {
        var next = preferences
        next.saved.removeAll { $0.id == id }
        persist(next)
    }

    func ignore(_ id: String) {
        var next = preferences
        next.ignored.insert(id)
        persist(next)
        if preferences.ignored.contains(id) {
            ledger.remove(id: id)
            publishNow()
        }
    }

    func resetIgnored() {
        var next = preferences
        next.ignored.removeAll()
        persist(next)
    }

    func clearHistory() {
        ledger = PhoneSightings()
        publishNow()
        var next = preferences
        next.saved.removeAll()
        persist(next)
    }

    func addBeacon(_ text: String) -> Bool {
        guard let uuid = UUID(uuidString: text.trimmingCharacters(in: .whitespacesAndNewlines)) else {
            beaconStatus = "Enter a complete beacon UUID."
            return false
        }
        guard !knownBeacons.contains(uuid) else {
            beaconStatus = "This UUID is already in your list."
            return false
        }
        guard knownBeacons.count < 20 else {
            beaconStatus = "Remove a UUID before adding another. You can keep 20."
            return false
        }
        var next = preferences
        next.knownBeacons.append(uuid)
        persist(next)
        if knownBeacons.contains(uuid) {
            if ranging { beginRangingIfAllowed() } else { beaconStatus = "" }
            return true
        }
        return false
    }

    func removeBeacon(_ uuid: UUID) {
        var next = preferences
        next.knownBeacons.removeAll { $0 == uuid }
        persist(next)
        if !knownBeacons.contains(uuid) {
            if knownBeacons.isEmpty { stopBeacons() } else if ranging { beginRangingIfAllowed() }
        }
    }

    func startBluetooth() {
        wantsBluetooth = true
        starting = true
        bluetoothStatus = "Checking Bluetooth…"
        if let central { centralManagerDidUpdateState(central) }
        else { central = CBCentralManager(delegate: self, queue: .main, options: [CBCentralManagerOptionShowPowerAlertKey: false]) }
    }

    func stopBluetooth() {
        wantsBluetooth = false
        if central?.isScanning == true { central?.stopScan() }
        starting = false
        scanning = false
        bluetoothStatus = "Scan stopped"
        updateTimer()
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        needsBluetoothSettings = central.state == .unauthorized
        switch central.state {
        case .poweredOn:
            guard wantsBluetooth else {
                scanning = false
                starting = false
                bluetoothStatus = "Ready to scan"
                return
            }
            if !central.isScanning {
                central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            }
            scanning = true
            starting = false
            bluetoothStatus = "Scanning nearby broadcasts"
        case .unknown, .resetting:
            scanning = false
            starting = wantsBluetooth
            bluetoothStatus = "Waiting for Bluetooth"
        case .unsupported, .unauthorized, .poweredOff:
            wantsBluetooth = false
            scanning = false
            starting = false
            if central.state == .unsupported { bluetoothStatus = "Bluetooth scanning is unavailable on this device." }
            if central.state == .unauthorized { bluetoothStatus = "Bluetooth access is off. Allow it in Settings to scan." }
            if central.state == .poweredOff { bluetoothStatus = "Bluetooth is off. Turn it on in Settings to scan." }
        @unknown default:
            wantsBluetooth = false
            scanning = false
            starting = false
            bluetoothStatus = "Bluetooth is unavailable."
        }
        updateTimer()
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let id = peripheral.identifier.uuidString
        guard wantsBluetooth, scanning, !preferences.ignored.contains(id) else { return }
        // peripheral.name may be cached or user-assigned. Only classify the broadcast name.
        let services = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []) +
            (advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] ?? [])
        let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] ?? [:]
        guard let match = PhoneSignatures.match(name: advertisementData[CBAdvertisementDataLocalNameKey] as? String,
                                                services: services.map(\.uuidString),
                                                serviceData: Dictionary(uniqueKeysWithValues: serviceData.map { ($0.key.uuidString, $0.value) }),
                                                manufacturerData: advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data) else { return }
        let dbm = RSSI.intValue
        ledger.record(id: id, match: match, rssi: (-127 ... -1).contains(dbm) ? dbm : nil, at: Date())
        pendingUpdate = true
    }

    func startBeacons() {
        guard !knownBeacons.isEmpty else { return }
        wantsBeacons = true
        if locator == nil {
            locator = CLLocationManager()
            locator?.delegate = self
        }
        beginRangingIfAllowed()
    }

    private func beginRangingIfAllowed() {
        guard wantsBeacons, let locator else { return }
        needsLocationSettings = false
        switch locator.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            guard CLLocationManager.isRangingAvailable() else {
                stopBeacons()
                beaconStatus = "Beacon ranging is unavailable on this device."
                return
            }
            for constraint in constraints where !knownBeacons.contains(constraint.uuid) {
                locator.stopRangingBeacons(satisfying: constraint)
            }
            let active = Set(constraints.map(\.uuid))
            constraints = knownBeacons.map { CLBeaconIdentityConstraint(uuid: $0) }
            for constraint in constraints where !active.contains(constraint.uuid) {
                locator.startRangingBeacons(satisfying: constraint)
            }
            ranging = true
            beaconStatus = "Looking for the UUIDs in your list."
            updateTimer()
        case .notDetermined:
            beaconStatus = "Location access is needed to range known beacons."
            locator.requestWhenInUseAuthorization()
        default:
            stopBeacons()
            needsLocationSettings = true
            beaconStatus = "Location access is off. Allow it in Settings to range beacons."
        }
    }

    func stopBeacons() {
        for constraint in constraints { locator?.stopRangingBeacons(satisfying: constraint) }
        constraints = []
        wantsBeacons = false
        ranging = false
        beaconStatus = "Beacon ranging stopped."
        updateTimer()
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) { beginRangingIfAllowed() }

    func locationManager(_ manager: CLLocationManager, didRange beacons: [CLBeacon],
                         satisfying beaconConstraint: CLBeaconIdentityConstraint) {
        guard ranging, wantsBeacons, knownBeacons.contains(beaconConstraint.uuid) else { return }
        for beacon in beacons where beacon.rssi != 0 {
            let uuid = beacon.uuid.uuidString
            let id = "\(uuid)/\(beacon.major)/\(beacon.minor)"
            guard !preferences.ignored.contains(id) else { continue }
            let match = BroadcastMatch(category: .beacon, family: "Major \(beacon.major) · Minor \(beacon.minor)",
                                       evidence: "Known iBeacon UUID \(uuid)", strength: 3)
            ledger.record(id: id, match: match, rssi: beacon.rssi, at: Date(), beaconUUID: uuid)
            pendingUpdate = true
        }
    }

    func locationManager(_ manager: CLLocationManager, didFailRangingFor beaconConstraint: CLBeaconIdentityConstraint,
                         error: Error) {
        guard wantsBeacons, constraints.contains(where: { $0.uuid == beaconConstraint.uuid }) else { return }
        stopBeacons()
        beaconStatus = "Ranging failed. Check Bluetooth and location access, then try again."
    }

    @objc private func stopForBackground() {
        if wantsBluetooth { stopBluetooth() }
        if wantsBeacons { stopBeacons() }
    }

    private func publishNow() {
        rows = ledger.values.values.sorted {
            if $0.lastSeen == $1.lastSeen { return $0.id < $1.id }
            return $0.lastSeen > $1.lastSeen
        }
        pendingUpdate = false
    }

    private func updateTimer() {
        if scanning || ranging {
            if flushTimer == nil {
                flushTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                    if self?.pendingUpdate == true { self?.publishNow() }
                }
            }
        } else {
            flushTimer?.invalidate()
            flushTimer = nil
            if pendingUpdate { publishNow() }
        }
    }

    deinit {
        flushTimer?.invalidate()
        if central?.isScanning == true { central?.stopScan() }
        for constraint in constraints { locator?.stopRangingBeacons(satisfying: constraint) }
    }
}
