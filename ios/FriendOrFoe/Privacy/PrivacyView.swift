import SwiftUI

private enum PrivacyFilter: String, CaseIterable, Identifiable {
    case recorders = "Recorders", nearby = "Nearby", beacons = "Beacons", all = "All"
    var id: Self { self }
}

private enum RecorderSource: String, CaseIterable {
    case both = "All sources", phone = "This iPhone", station = "Scanner server"
}

struct PrivacyView: View {
    @StateObject private var scanner = PhonePrivacyScanner()
    @State private var filter: PrivacyFilter = .recorders
    @State private var search = ""
    @State private var source = RecorderSource.both
    @State private var recorderFamily: String?
    @State private var guide = false
    @State private var history = false
    @State private var beaconSetup = false
    @State private var selected: PhoneSighting?
    @State private var everydayExpanded = false
    @Environment(\.openURL) private var openURL

    private var includesStation: Bool { filter == .recorders || filter == .all }
    private var activeFamily: String? { filter == .recorders ? recorderFamily : nil }
    private var filtering: Bool { includesStation && (source != .both || activeFamily != nil) }

    private var visible: [PhoneSighting] {
        scanner.rows.filter { row in
            guard row.contains(search.trimmingCharacters(in: .whitespacesAndNewlines)) else { return false }
            if let family = activeFamily, row.match.family != family { return false }
            switch filter {
            case .recorders: return row.match.category == .recorder
            case .nearby: return row.match.category != .beacon
            case .beacons: return row.match.category == .beacon
            case .all: return true
            }
        }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    VStack(alignment: .leading, spacing: 10) {
                        Label(scanner.bluetoothStatus, systemImage: scanner.scanning ? "wave.3.right" : "antenna.radiowaves.left.and.right")
                            .font(.headline)
                        Text("Scan while the app is open to look for nearby recorder broadcasts. Recording status stays unknown.")
                            .font(.subheadline).foregroundStyle(.secondary)
                        if scanner.needsBluetoothSettings {
                            Button("Open Bluetooth permissions") {
                                if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                            }.buttonStyle(.bordered)
                        } else {
                            Button(scanner.scanning || scanner.starting ? "Stop Bluetooth scan" : "Start Bluetooth scan") {
                                if scanner.scanning || scanner.starting { scanner.stopBluetooth() } else { scanner.startBluetooth() }
                            }
                            .buttonStyle(.borderedProminent)
                            .accessibilityIdentifier("privacy.scan")
                        }
                    }.padding(.vertical, 4)
                    Button { guide = true } label: { Label("Supported recorders & name example", systemImage: "book") }
                        .accessibilityIdentifier("privacy.guide")
                }
                Section {
                    ScrollView(.horizontal) {
                        HStack(spacing: 6) {
                            ForEach(PrivacyFilter.allCases) { item in
                                Button {
                                    filter = item
                                } label: {
                                    Text(item.rawValue)
                                        .font(.subheadline.weight(.semibold))
                                        .fixedSize().padding(.horizontal, 12).padding(.vertical, 8)
                                        .foregroundStyle(filter == item ? Color.accentColor : Color.secondary)
                                        .background(filter == item ? Color.accentColor.opacity(0.13) : Color.clear, in: Capsule())
                                        .frame(minHeight: 44)
                                }
                                .buttonStyle(.plain)
                                .accessibilityAddTraits(filter == item ? .isSelected : [])
                            }
                        }
                    }
                    .scrollIndicators(.hidden)
                    .listRowInsets(EdgeInsets(top: 0, leading: 8, bottom: 0, trailing: 8))
                }
                if filter == .beacons {
                    Section {
                        Button { beaconSetup = true } label: {
                            Label(scanner.knownBeacons.isEmpty ? "Add a known beacon UUID" : "Manage known beacon UUIDs",
                                  systemImage: "dot.radiowaves.left.and.right")
                        }
                        if !scanner.knownBeacons.isEmpty {
                            Button(scanner.ranging ? "Stop beacon ranging" : "Range known beacons") {
                                if scanner.ranging { scanner.stopBeacons() } else { scanner.startBeacons() }
                            }
                        }
                        if !scanner.beaconStatus.isEmpty {
                            Text(scanner.beaconStatus).font(.subheadline).foregroundStyle(.secondary)
                        }
                        if scanner.needsLocationSettings {
                            Button("Open location permissions") {
                                if let url = URL(string: UIApplication.openSettingsURLString) { openURL(url) }
                            }
                        }
                    } footer: { Text("iPhone can range iBeacons whose network UUID you know. Location access is requested when you start ranging.") }
                }
                if filtering {
                    Section {
                        HStack {
                            Text([source.rawValue, activeFamily].compactMap { $0 }.joined(separator: " · "))
                                .font(.subheadline)
                            Spacer()
                            Button("Reset") { source = .both; recorderFamily = nil }
                        }
                    }
                }
                if !includesStation || source != .station { phoneSections }
                if includesStation && source != .phone {
                    StationRecorderSection(search: search, recorderFamily: activeFamily)
                    if source == .station {
                        Section { Text("Open Station to connect a scanner server or refresh its recorder reports.")
                            .font(.subheadline).foregroundStyle(.secondary) }
                    }
                }
            }
            .navigationTitle("Privacy")
            .searchable(text: $search, prompt: "Search observations")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    if includesStation {
                        Menu {
                            Picker("Source", selection: $source) {
                                ForEach(RecorderSource.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                            }
                            if filter == .recorders {
                                Picker("Recorder family", selection: $recorderFamily) {
                                    Text("All recorder families").tag(nil as String?)
                                    ForEach(PhoneSignatures.recorders, id: \.brand) { recorder in
                                        Text(recorder.brand).tag(Optional(recorder.brand))
                                    }
                                }
                            }
                        } label: {
                            Label("Filter observations", systemImage: filtering ? "line.3.horizontal.decrease.circle.fill" : "line.3.horizontal.decrease.circle")
                        }
                    }
                    Button { history = true } label: { Label("Saved observations", systemImage: "clock.arrow.circlepath") }
                        .accessibilityIdentifier("privacy.history")
                }
            }
            .sheet(isPresented: $guide) { RecorderGuide() }
            .sheet(isPresented: $history) { PhoneHistoryView(scanner: scanner) }
            .sheet(isPresented: $beaconSetup) { BeaconSetupView(scanner: scanner) }
            .sheet(item: $selected) { observation in
                NavigationStack { PhoneObservationDetail(scanner: scanner, row: observation) }
            }
            .alert("Scanner storage", isPresented: Binding(get: { scanner.storageMessage != nil }, set: { if !$0 { scanner.storageMessage = nil } })) {
                Button("OK") { scanner.storageMessage = nil }
            } message: { Text(scanner.storageMessage ?? "") }
        }
    }

    @ViewBuilder private var phoneSections: some View {
        let results = visible
        if results.isEmpty {
            Section("On this iPhone") {
                if !search.isEmpty {
                    Label("No matching iPhone observations", systemImage: "magnifyingglass")
                } else {
                    Text(filter == .recorders ? "No recorder broadcasts observed" :
                            filter == .beacons ? "No known iBeacons observed" : "No matching broadcasts observed")
                    .foregroundStyle(.secondary)
                }
            }
        } else {
            ForEach([SignalCategory.recorder, .camera, .glasses, .tracker, .drone], id: \.self) { category in
                let group = results.filter { $0.match.category == category }
                if !group.isEmpty {
                    Section("\(category.rawValue) · iPhone Bluetooth") {
                        ForEach(group) { row in sightingButton(row) }
                    }
                }
            }
            let everyday = results.filter { $0.match.category == .everyday }
            if !everyday.isEmpty {
                Section {
                    DisclosureGroup(isExpanded: $everydayExpanded) {
                        ForEach(everyday) { row in sightingButton(row) }
                    } label: { Label("Everyday devices (\(everyday.count))", systemImage: "headphones") }
                }
            }
            let beacons = Dictionary(grouping: results.filter { $0.beaconUUID != nil }, by: { $0.beaconUUID! })
            if !beacons.isEmpty {
                Section("iBeacon networks · iPhone") {
                    ForEach(beacons.keys.sorted(), id: \.self) { network in
                        DisclosureGroup {
                            ForEach(beacons[network] ?? []) { row in sightingButton(row) }
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("iBeacon network (\(beacons[network]?.count ?? 0))").font(.headline)
                                Text(network).font(.caption.monospaced()).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
        }
    }

    private func sightingButton(_ row: PhoneSighting) -> some View {
        Button { selected = row } label: { SightingLabel(row: row) }
            .buttonStyle(.plain)
    }
}

private struct SightingLabel: View {
    let row: PhoneSighting
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Label(row.title, systemImage: row.match.category.symbol).font(.headline)
            Text(row.match.evidence).font(.subheadline).foregroundStyle(.secondary).lineLimit(2)
            HStack {
                if let dbm = row.rssi { Text("\(dbm) dBm") }
                Text(row.lastSeen, style: .relative)
            }.font(.caption).foregroundStyle(.secondary)
        }.padding(.vertical, 3).accessibilityElement(children: .combine)
    }
}

private struct PhoneObservationDetail: View {
    @ObservedObject var scanner: PhonePrivacyScanner
    let row: PhoneSighting
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                Label(row.title, systemImage: row.match.category.symbol).font(.title3.bold())
                Text(row.match.evidence).textSelection(.enabled)
                if [.recorder, .camera, .glasses].contains(row.match.category) {
                    Text("Recording status unknown. The broadcast can be imitated.")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
            } header: { Text("Detection evidence") }
            Section("Observation") {
                LabeledContent("Source", value: row.beaconUUID == nil ? "iPhone Bluetooth" : "iPhone beacon ranging")
                LabeledContent("First seen") { Text(row.firstSeen.formatted()) }
                LabeledContent("Last seen") { Text(row.lastSeen.formatted()) }
                LabeledContent("Sightings", value: "\(row.count)")
                LabeledContent("Signal", value: row.rssi.map { "\($0) dBm" } ?? "Unavailable")
            }
            Section {
                if scanner.saved.contains(where: { $0.id == row.id }) {
                    Button("Remove saved observation", role: .destructive) { scanner.forgetSaved(row.id) }
                } else {
                    Button { scanner.save(row) } label: { Label("Save observation", systemImage: "bookmark") }
                }
                Button("Ignore this signal") {
                    scanner.ignore(row.id)
                    dismiss()
                }
            } footer: { Text("Signal strength varies with walls and distance. Ignoring applies to this identifier; a device can change it.") }
        }
        .navigationTitle("Observation")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
    }
}

private struct PhoneHistoryView: View {
    @ObservedObject var scanner: PhonePrivacyScanner
    @Environment(\.dismiss) private var dismiss
    @State private var confirmClear = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if scanner.saved.isEmpty {
                        Text("Open a finding and choose Save observation to keep it here.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(scanner.saved) { row in
                        NavigationLink {
                            PhoneObservationDetail(scanner: scanner, row: row)
                        } label: { SightingLabel(row: row) }
                    }
                } header: { Text("Saved on this iPhone") }
                Section {
                    if scanner.ignoredCount > 0 {
                        Button("Show ignored signals again (\(scanner.ignoredCount))") { scanner.resetIgnored() }
                    }
                    Button("Clear sightings and saved observations", role: .destructive) { confirmClear = true }
                        .disabled(scanner.rows.isEmpty && scanner.saved.isEmpty)
                } footer: { Text("New sightings stay in memory until you close the app. Only observations you save are kept between launches.") }
            }
            .navigationTitle("Scanner history")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .confirmationDialog("Clear current sightings and all saved observations?", isPresented: $confirmClear, titleVisibility: .visible) {
                Button("Clear observations", role: .destructive) { scanner.clearHistory() }
            }
        }
    }
}

private struct RecorderGuide: View {
    @State private var name = "PLAUDAB12"
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            List {
                Section {
                    TextField("Bluetooth name", text: $name)
                        .autocorrectionDisabled().textInputAutocapitalization(.never)
                        .accessibilityIdentifier("privacy.example.name")
                    if let match = PhoneSignatures.match(name: name), match.category == .recorder {
                        Label("Name matches the \(match.family) recorder family.", systemImage: "checkmark.circle").foregroundStyle(.tint)
                    } else {
                        Text("No supported recorder name matches.").foregroundStyle(.secondary)
                    }
                } header: { Text("Try a Bluetooth name") } footer: {
                    Text("This offline example uses the scanner’s name rules. It creates no observation and does not start a scan.")
                }
                Section {
                    ForEach(PhoneSignatures.recorders, id: \.brand) { recorder in
                        VStack(alignment: .leading, spacing: 3) {
                            Text(recorder.brand).font(.headline)
                            Text(recorder.service == nil ? "Advertised name" : "Advertised name or distinctive service")
                                .font(.subheadline).foregroundStyle(.secondary)
                        }
                    }
                } header: { Text("Recorder families") } footer: {
                    Text("Visibility depends on what the recorder broadcasts. An idle, renamed or already connected recorder can be missed.")
                }
            }
            .navigationTitle("Recorder guide")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}

private struct BeaconSetupView: View {
    @ObservedObject var scanner: PhonePrivacyScanner
    @State private var input = ""
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            List {
                Section {
                    TextField("Proximity UUID", text: $input, axis: .vertical)
                        .font(.body.monospaced()).autocorrectionDisabled().textInputAutocapitalization(.never)
                    Button("Add UUID") { if scanner.addBeacon(input) { input = "" } }
                        .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    if !scanner.beaconStatus.isEmpty {
                        Text(scanner.beaconStatus).font(.subheadline).foregroundStyle(.secondary)
                    }
                } footer: { Text("Use the proximity UUID from the beacon configuration or a trusted scanner report.") }
                Section("Known networks") {
                    ForEach(scanner.knownBeacons, id: \.self) { uuid in
                        Text(uuid.uuidString).font(.subheadline.monospaced())
                            .swipeActions { Button("Remove", role: .destructive) { scanner.removeBeacon(uuid) } }
                    }
                }
            }
            .navigationTitle("Known iBeacons")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}
