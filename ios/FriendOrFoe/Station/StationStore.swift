import Foundation
import SwiftUI

enum StationError: LocalizedError {
    case invalidAddress, incompatible, tooLarge, redirected, rejected(Int), cannotSave

    var errorDescription: String? {
        switch self {
        case .invalidAddress: return "Enter an HTTPS server address. Leave out usernames, passwords, query parameters and fragments."
        case .incompatible: return "The server did not return a compatible Friend or Foe feed."
        case .tooLarge: return "The report is too large to open on this phone."
        case .redirected: return "The server redirected the request. Enter its final HTTPS address."
        case let .rejected(status): return "The server returned HTTP \(status)."
        case .cannotSave: return "The server address could not be saved on this phone."
        }
    }
}

enum StationAddress {
    static func parse(_ input: String) throws -> URL {
        let raw = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !raw.contains("\\"), raw.rangeOfCharacter(from: .whitespacesAndNewlines) == nil,
              let parts = URLComponents(string: raw, encodingInvalidCharacters: false),
              parts.scheme?.lowercased() == "https", let host = parts.host, !host.isEmpty,
              parts.user == nil, parts.password == nil, parts.query == nil, parts.fragment == nil,
              !parts.percentEncodedPath.contains("%"), !parts.path.split(separator: "/").contains(where: { $0 == "." || $0 == ".." }),
              parts.port.map({ (1...65535).contains($0) }) ?? true,
              let url = parts.url else { throw StationError.invalidAddress }
        return url
    }
}

private final class StationRedirectDelegate: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

struct StationSnapshot {
    var reports: [StationReport] = []
    var received: Date?
    var error: String?
}

@MainActor
final class StationStore: ObservableObject {
    @Published private(set) var base: URL?
    @Published private(set) var devices = StationSnapshot()
    @Published private(set) var networks = StationSnapshot()
    @Published private(set) var isRefreshing = false

    private let addressFile: URL
    private let read: (StationFeed, URL) async throws -> [StationReport]
    private var generation = UUID()
    private var task: Task<Void, Never>?

    init(addressFile: URL = URL.applicationSupportDirectory.appending(path: "Station/address"),
         read: @escaping (StationFeed, URL) async throws -> [StationReport] = StationClient.read) {
        self.addressFile = addressFile
        self.read = read
        if let saved = try? String(contentsOf: addressFile, encoding: .utf8) {
            base = try? StationAddress.parse(saved)
        }
    }

    func configure(_ input: String) throws {
        let value = try StationAddress.parse(input)
        guard value != base else { return }
        do {
            var directory = addressFile.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            var resources = URLResourceValues()
            resources.isExcludedFromBackup = true
            try directory.setResourceValues(resources)
            try Data(value.absoluteString.utf8).write(to: addressFile, options: [.atomic, .completeFileProtection])
        } catch {
            throw StationError.cannotSave
        }
        replaceAddress(with: value)
    }

    func forget() throws {
        // A locked or inaccessible file must not be mistaken for a missing file.
        do { try FileManager.default.removeItem(at: addressFile) }
        catch let error as NSError where error.domain == NSCocoaErrorDomain && error.code == NSFileNoSuchFileError {}
        catch { throw error }
        replaceAddress(with: nil)
    }

    private func replaceAddress(with base: URL?) {
        generation = UUID()
        task?.cancel()
        task = nil
        self.base = base
        devices = StationSnapshot()
        networks = StationSnapshot()
        isRefreshing = false
    }

    func refresh() async {
        if let task { await task.value; return }
        guard let base else { return }
        let current = generation
        isRefreshing = true
        let read = read
        let operation = Task {
            async let deviceResult = Self.fetch(.devices, from: base, read: read)
            async let networkResult = Self.fetch(.networks, from: base, read: read)
            let results = await (deviceResult, networkResult)
            guard !Task.isCancelled, current == self.generation else { return }
            self.devices = Self.applying(results.0, to: self.devices)
            self.networks = Self.applying(results.1, to: self.networks)
            self.isRefreshing = false
            self.task = nil
        }
        task = operation
        await operation.value
    }

    private static func fetch(_ feed: StationFeed, from base: URL,
                              read: (StationFeed, URL) async throws -> [StationReport]) async -> Result<[StationReport], Error> {
        do { return .success(try await read(feed, base)) }
        catch { return .failure(error) }
    }

    private static func applying(_ result: Result<[StationReport], Error>, to previous: StationSnapshot) -> StationSnapshot {
        switch result {
        case .success(let reports):
            return StationSnapshot(reports: reports, received: Date())
        case .failure(let error):
            var cached = previous
            cached.error = error.localizedDescription
            return cached
        }
    }
}

enum StationClient {
    private static let maxBytes = 8 * 1024 * 1024
    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 12
        config.timeoutIntervalForResource = 20
        config.httpCookieStorage = nil
        config.urlCredentialStorage = nil
        config.urlCache = nil
        config.httpShouldSetCookies = false
        return URLSession(configuration: config, delegate: StationRedirectDelegate(), delegateQueue: nil)
    }()

    static func read(_ feed: StationFeed, base: URL) async throws -> [StationReport] {
        var request = URLRequest(url: feed.requestURL(base: base))
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse else { throw StationError.incompatible }
        guard (200...299).contains(http.statusCode) else {
            bytes.task.cancel()
            if (300...399).contains(http.statusCode) { throw StationError.redirected }
            throw StationError.rejected(http.statusCode)
        }
        guard response.expectedContentLength <= maxBytes else {
            bytes.task.cancel()
            throw StationError.tooLarge
        }
        var data = Data()
        for try await byte in bytes {
            guard data.count < maxBytes else {
                bytes.task.cancel()
                throw StationError.tooLarge
            }
            data.append(byte)
        }
        return try feed.decode(data)
    }
}
