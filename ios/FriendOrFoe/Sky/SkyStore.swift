import Combine
import CoreLocation
import Foundation
import UIKit

enum SkyLoadState: Equatable {
    case off, locating, loading, ready, failed(String)
}

enum SkyFilter: String, CaseIterable, Identifiable {
    case all = "All aircraft", airborne = "Airborne", ground = "On the ground"
    var id: Self { self }
}

@MainActor
final class SkyStore: NSObject, ObservableObject, @preconcurrency CLLocationManagerDelegate {
    @Published private(set) var state: SkyLoadState = .off
    @Published private(set) var area: SkyArea?
    @Published private(set) var aircraft: [SkyAircraft] = []
    @Published private(set) var trails = SkyTrails()
    @Published private(set) var saved: [SkySavedFlight] = []
    @Published private(set) var received: Date?
    @Published private(set) var rangeMiles = 10
    @Published var filter = SkyFilter.all
    @Published var search = ""
    @Published var automaticRefresh = false
    @Published var storageMessage: String?

    private var hasConsent = false
    private var locationManager: CLLocationManager?
    private var waitingForLocation = false
    private var loadTask: Task<Void, Never>?
    private var locationTimeout: Task<Void, Never>?
    private var generation = 0
    private var nextAllowed = Date.distantPast
    private let archiveURL: URL
    private let session: URLSession

    init(archiveURL: URL? = nil) {
        self.archiveURL = archiveURL ?? URL.applicationSupportDirectory
            .appending(path: "SavedFlights", directoryHint: .isDirectory).appending(path: "flights.json")
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 15
        config.timeoutIntervalForResource = 20
        config.httpCookieStorage = nil
        config.urlCache = nil
        session = URLSession(configuration: config, delegate: SkyFeedTransport(), delegateQueue: nil)
        super.init()
        NotificationCenter.default.addObserver(self, selector: #selector(pauseInBackground),
                                               name: UIApplication.didEnterBackgroundNotification, object: nil)
        if let data = try? Data(contentsOf: self.archiveURL),
           let flights = try? JSONDecoder().decode([SkySavedFlight].self, from: data) {
            saved = Array(flights.prefix(100))
        }
    }

    var enabled: Bool { hasConsent }
    var busy: Bool { state == .locating || state == .loading }

    var filtered: [SkyAircraft] {
        let term = search.trimmingCharacters(in: .whitespacesAndNewlines)
        return aircraft.filter { plane in
            (filter == .all || (filter == .ground ? plane.onGround : !plane.onGround)) &&
            (term.isEmpty || [plane.callsign, plane.registration, plane.typeCode, plane.id].compactMap { $0 }
                .contains { $0.localizedCaseInsensitiveContains(term) })
        }
    }

    func start() {
        hasConsent = true
        locate()
    }

    func locate() {
        guard hasConsent else { return }
        cancelRequests()
        // A different location must never inherit results or a route from the old search area.
        area = nil
        aircraft = []
        trails = SkyTrails()
        received = nil
        state = .locating
        waitingForLocation = true
        let manager = locationManager ?? CLLocationManager()
        locationManager = manager
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyKilometer
        switch manager.authorizationStatus {
        case .notDetermined: manager.requestWhenInUseAuthorization()
        default: handleAuthorization(manager)
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        guard hasConsent else { return }
        handleAuthorization(manager)
    }

    private func handleAuthorization(_ manager: CLLocationManager) {
        switch manager.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways:
            if waitingForLocation, locationTimeout == nil {
                manager.requestLocation()
                locationTimeout = Task { [weak self] in
                    do { try await Task.sleep(for: .seconds(20)) } catch { return }
                    guard let self, waitingForLocation else { return }
                    finishLocationRequest()
                    state = .failed("A recent location is unavailable. Check Location Services and try again.")
                }
            }
        case .restricted, .denied:
            stop()
            state = .failed("Location access is off. Allow location in Settings to search nearby aircraft.")
        default: break
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard hasConsent, waitingForLocation,
              let fix = locations.last(where: {
                  $0.horizontalAccuracy >= 0 && abs($0.timestamp.timeIntervalSinceNow) < 120
              }), let rounded = SkyArea(latitude: fix.coordinate.latitude, longitude: fix.coordinate.longitude)
        else { return }
        finishLocationRequest()
        area = rounded
        refresh()
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        guard hasConsent, waitingForLocation else { return }
        finishLocationRequest()
        state = .failed("The phone could not determine its location. Try again outdoors or check Location Services.")
    }

    func setRange(_ miles: Int) {
        let value = min(50, max(1, miles))
        guard value != rangeMiles else { return }
        rangeMiles = value
        guard area != nil else { return }
        // A cached set for a different radius cannot establish coverage for this search.
        cancelRequests()
        aircraft = []
        received = nil
        state = .ready
        refresh()
    }

    func refresh() {
        guard hasConsent else { return }
        guard let area else { locate(); return }
        guard state != .loading, let url = area.url(radiusMiles: rangeMiles) else { return }
        guard Date() >= nextAllowed else {
            state = .failed("Please wait until \(nextAllowed.formatted(date: .omitted, time: .shortened)) to refresh this feed.")
            return
        }
        let requestID = generation
        let miles = rangeMiles
        state = .loading
        loadTask = Task { [weak self, session] in
            do {
                var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData)
                request.setValue("application/json", forHTTPHeaderField: "Accept")
                let (data, response) = try await session.data(for: request)
                guard !Task.isCancelled, let self, generation == requestID else { return }
                guard let http = response as? HTTPURLResponse else { throw SkyFeedError.unavailable }
                if http.statusCode == 429 {
                    nextAllowed = Self.retryDate(http.value(forHTTPHeaderField: "Retry-After"), now: Date())
                    throw SkyFeedError.rateLimited
                }
                guard http.statusCode == 200 else { throw SkyFeedError.unavailable }
                let now = Date()
                let reports = try await SkyFeed.nearby(data, now: now, center: area.center, radiusMiles: miles)
                guard !Task.isCancelled, generation == requestID else { return }
                aircraft = reports
                trails.record(aircraft, now: now)
                received = now
                state = .ready
            } catch {
                guard !Task.isCancelled, let self, generation == requestID else { return }
                state = .failed((error as? SkyFeedError)?.errorDescription ?? "Couldn’t load aircraft. Check your connection and retry.")
            }
        }
    }

    static func retryDate(_ value: String?, now: Date) -> Date {
        if let value, let seconds = Double(value), seconds.isFinite, seconds >= 0 {
            return now.addingTimeInterval(min(seconds, 86_400))
        }
        if let value {
            let format = DateFormatter()
            format.locale = Locale(identifier: "en_US_POSIX")
            format.timeZone = TimeZone(secondsFromGMT: 0)
            format.dateFormat = "EEE',' dd MMM yyyy HH':'mm':'ss z"
            if let target = format.date(from: value), target > now { return target }
        }
        return now.addingTimeInterval(60)
    }

    func stop() {
        cancelRequests()
        hasConsent = false
        area = nil
        automaticRefresh = false
        aircraft = []
        trails = SkyTrails()
        received = nil
        state = .off
    }

    @objc private func pauseInBackground() {
        guard hasConsent else { return }
        automaticRefresh = false
        if busy {
            cancelRequests()
            state = received == nil ? .failed("Aircraft lookup paused. Refresh when you’re ready.") : .ready
        }
    }

    private func cancelRequests() {
        generation += 1
        loadTask?.cancel()
        loadTask = nil
        finishLocationRequest()
    }

    private func finishLocationRequest() {
        waitingForLocation = false
        locationTimeout?.cancel()
        locationTimeout = nil
        locationManager?.stopUpdatingLocation()
    }

    func save(_ plane: SkyAircraft) {
        guard !saved.contains(where: { $0.aircraft.id == plane.id && $0.aircraft.observedAt == plane.observedAt }) else { return }
        guard saved.count < 100 else {
            storageMessage = "Remove a saved flight before adding another. You can keep up to 100."
            return
        }
        let flight = SkySavedFlight(id: UUID(), aircraft: plane, trail: trails.points[plane.id] ?? [], savedAt: Date())
        writeSaved([flight] + saved)
    }

    func removeSaved(_ ids: Set<UUID>) {
        writeSaved(saved.filter { !ids.contains($0.id) })
    }

    private func writeSaved(_ next: [SkySavedFlight]) {
        do {
            var directory = archiveURL.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            var flags = URLResourceValues()
            flags.isExcludedFromBackup = true
            try directory.setResourceValues(flags)
            try JSONEncoder().encode(next).write(to: archiveURL, options: [.atomic, .completeFileProtection])
            saved = next
            storageMessage = nil
        } catch {
            storageMessage = "The saved flights could not be updated. Try again when the phone is unlocked."
        }
    }
}


private final class SkyFeedTransport: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        // The consent screen names the single recipient of the search area.
        completionHandler(nil)
    }
}
