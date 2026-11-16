import SwiftUI

struct StationView: View {
    @EnvironmentObject private var station: StationStore
    @State private var configuring = false
    @State private var query = ""
    @State private var filter: StationCategory?

    private var reports: [StationReport] { station.devices.reports + station.networks.reports }

    var body: some View {
        Group {
            if station.base == nil {
                ContentUnavailableView {
                    Label("Connect a scanner server", systemImage: "antenna.radiowaves.left.and.right")
                } description: {
                    Text("Read recorder sightings, Wi-Fi alerts and beacon reports from a Friend or Foe server you use.")
                } actions: {
                    Button("Add server") { configuring = true }
                        .buttonStyle(.borderedProminent).tint(Color("ActionFill"))
                    badgeDiagnostics
                }
            } else {
                reportsList
                    .searchable(text: $query, prompt: "Search station reports")
                    .refreshable { await station.refresh() }
            }
        }
        .navigationTitle("Station")
        .toolbar {
            if station.base != nil {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { configuring = true } label: { Image(systemName: "gearshape") }
                        .accessibilityLabel("Scanner server settings")
                }
            }
        }
        .sheet(isPresented: $configuring) {
            StationConfigurationView(station: station) { configuring = false }
                .presentationDragIndicator(.visible)
        }
    }

    private var badgeDiagnostics: some View {
        NavigationLink {
            BadgeDiagnosticsView()
        } label: {
            Label("Badge diagnostics", systemImage: "antenna.radiowaves.left.and.right")
        }
    }

    private var reportsList: some View {
        let matches = reports.filter { $0.matches(query) && (filter == nil || $0.category == filter) }
        return List {
            Section {
                LabeledContent("Server", value: station.base?.host ?? "")
                HStack {
                    if station.isRefreshing { ProgressView().controlSize(.small) }
                    Button(station.isRefreshing ? "Refreshing reports…" : "Refresh reports") {
                        Task { await station.refresh() }
                    }
                    .disabled(station.isRefreshing)
                }
                status("Devices", station.devices)
                status("Wi-Fi alerts", station.networks)
            }
            if !reports.isEmpty || filter != nil {
                Picker("Show", selection: $filter) {
                    Text("All reports").tag(nil as StationCategory?)
                    ForEach(StationCategory.allCases) { category in
                        Text(category.rawValue).tag(Optional(category))
                    }
                }
            }
            if matches.isEmpty, !station.isRefreshing {
                if !query.isEmpty || filter != nil {
                    Text("No matching reports.").foregroundStyle(.secondary)
                } else if station.devices.received != nil || station.networks.received != nil {
                    Text("No relevant reports in the last received feeds.").foregroundStyle(.secondary)
                } else if station.devices.error == nil && station.networks.error == nil {
                    Text("Refresh to read this server’s reports.").foregroundStyle(.secondary)
                }
            }
            ForEach(StationCategory.allCases) { category in
                let items = matches.filter { $0.category == category }
                if !items.isEmpty {
                    Section {
                        if category == .everyday {
                            DisclosureGroup("Everyday devices (\(items.count))") {
                                ForEach(items) { StationReportLink(report: $0) }
                            }
                        } else if category == .beacon {
                            let networks = Dictionary(grouping: items) { $0.beaconNetwork ?? "Network ID unavailable" }
                            ForEach(networks.keys.sorted(), id: \.self) { id in
                                let group = networks[id] ?? []
                                DisclosureGroup("\(id) (\(group.count))") {
                                    ForEach(group) { StationReportLink(report: $0) }
                                }
                            }
                        } else {
                            ForEach(items) { StationReportLink(report: $0) }
                        }
                    } header: {
                        if category != .everyday { Text(category.rawValue) }
                    }
                }
            }
            Section { badgeDiagnostics }
        }
    }

    @ViewBuilder
    private func status(_ label: String, _ snapshot: StationSnapshot) -> some View {
        if let error = snapshot.error {
            VStack(alignment: .leading, spacing: 4) {
                Label("\(label) unavailable", systemImage: "exclamationmark.circle")
                    .font(.subheadline.bold())
                Text(error).font(.footnote)
                if let seen = snapshot.received {
                    Text("Showing reports from \(seen.formatted(date: .abbreviated, time: .shortened)).")
                        .font(.footnote)
                }
            }
        } else if let seen = snapshot.received {
            LabeledContent(label, value: seen.formatted(date: .abbreviated, time: .shortened))
                .font(.footnote).foregroundStyle(.secondary)
        }
    }
}

/// Embeds fetched server sightings alongside the phone scanner. Reading this section never starts a request.
struct StationRecorderSection: View {
    @EnvironmentObject private var station: StationStore
    let search: String
    var recorderFamily: String? = nil

    var body: some View {
        let rows = station.devices.reports.filter { report in
            guard report.category == .recorder, report.matches(search) else { return false }
            guard let recorderFamily else { return true }
            if let manufacturer = report.recorderFamily {
                return manufacturer.localizedCaseInsensitiveContains(recorderFamily)
            }
            return report.title.localizedCaseInsensitiveContains(recorderFamily)
        }
        if !rows.isEmpty {
            Section {
                ForEach(rows) { StationReportLink(report: $0) }
            } header: {
                Label("Scanner server", systemImage: "antenna.radiowaves.left.and.right")
            } footer: {
                if let fetched = station.devices.received {
                    let time = fetched.formatted(date: .abbreviated, time: .shortened)
                    Text(station.devices.error == nil ? "Last fetched \(time)." : "Server unavailable. Last fetched \(time).")
                }
            }
        }
    }
}

private struct StationReportLink: View {
    let report: StationReport

    var body: some View {
        NavigationLink {
            StationReportDetail(report: report)
        } label: {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: report.category.symbol).foregroundStyle(.tint)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text(report.title).font(.headline)
                    if report.category == .wifi, let alert = report.evidence.first {
                        Text(alert).font(.subheadline.weight(.semibold))
                    } else if let network = report.networkName {
                        Text("SSID · \(network)").font(.subheadline)
                    }
                    Text(report.detail).font(.subheadline).foregroundStyle(.secondary).lineLimit(3)
                    if report.category == .recorder && !report.detail.localizedCaseInsensitiveContains("recording status unknown") {
                        Text("Recording status unknown").font(.caption).foregroundStyle(.secondary)
                    }
                    if let seen = report.lastSeen {
                        Text("Seen \(seen.formatted(date: .abbreviated, time: .shortened))")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}

private struct StationReportDetail: View {
    let report: StationReport

    var body: some View {
        List {
            Section {
                Label(report.title, systemImage: report.category.symbol).font(.title3.bold())
                Text(report.detail)
                if report.category == .recorder {
                    Text("The scanner saw a recorder signal. Its recording status is unknown.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            Section("Observation") {
                LabeledContent("Source", value: "Scanner server")
                if report.category == .wifi || report.networkName != nil {
                    LabeledContent("SSID", value: report.networkName ?? "Unavailable")
                }
                if let uuid = report.beaconNetwork { LabeledContent("Beacon network", value: uuid) }
                if let rssi = report.rssi { LabeledContent("Signal at scanner", value: "\(rssi) dBm") }
                if let sensor = report.sensor, !sensor.isEmpty { LabeledContent("Reporting sensor", value: sensor) }
                LabeledContent("Last observed", value: report.lastSeen?.formatted() ?? "Unavailable")
            }
            if !report.evidence.isEmpty {
                Section("Scanner evidence") {
                    ForEach(report.evidence, id: \.self) { Text($0) }
                }
            }
        }
        .textSelection(.enabled)
        .navigationTitle("Report details")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct BadgeDiagnosticsView: View {
    // The badge firmware's factory access point serves this GET endpoint without credentials.
    private let statusURL = URL(string: "http://192.168.4.1/api/badge/status")!

    var body: some View {
        List {
            Section {
                Label("Join your badge Wi-Fi", systemImage: "wifi").font(.headline)
                Text("Open Wi-Fi in iPhone Settings and join the network from your Friend or Foe scanner badge. Return here once connected.")
                Link(destination: statusURL) {
                    Label("Open badge status in browser", systemImage: "safari")
                }
            } footer: {
                Text("Your browser opens the read-only diagnostic report at the badge’s factory address. The badge must be powered on and running firmware with the status endpoint.")
            }
        }
        .navigationTitle("Badge diagnostics")
        .navigationBarTitleDisplayMode(.inline)
    }
}

private struct StationConfigurationView: View {
    @ObservedObject var station: StationStore
    let close: () -> Void
    @State private var address: String
    @State private var error: String?
    @State private var confirmForget = false

    init(station: StationStore, close: @escaping () -> Void) {
        self.station = station
        self.close = close
        _address = State(initialValue: station.base?.absoluteString ?? "")
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("HTTPS server address", text: $address)
                        .keyboardType(.URL)
                        .textContentType(.URL)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .accessibilityLabel("HTTPS server address")
                } header: {
                    Text("Friend or Foe server")
                } footer: {
                    Text("Use the base address of your scanner server. When you connect or refresh, it receives a request from your IP address. Reports stay in memory.")
                }
                if let error {
                    Label(error, systemImage: "exclamationmark.circle").foregroundStyle(.red)
                }
                Button("Connect and fetch reports") {
                    do {
                        try station.configure(address)
                        close()
                        Task { await station.refresh() }
                    } catch { self.error = error.localizedDescription }
                }
                .disabled(address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                if station.base != nil {
                    Button("Forget server", role: .destructive) { confirmForget = true }
                }
            }
            .navigationTitle("Scanner server")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: close) }
            }
            .confirmationDialog("Forget this server and clear its reports?", isPresented: $confirmForget, titleVisibility: .visible) {
                Button("Forget server", role: .destructive) {
                    do {
                        try station.forget()
                        close()
                    } catch { self.error = "The server address could not be removed." }
                }
            }
        }
    }
}
