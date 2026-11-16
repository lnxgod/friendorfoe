import MapKit
import SwiftUI

struct NearbyView: View {
    @EnvironmentObject private var sky: SkyStore

    var body: some View {
        NavigationStack {
            List {
                if sky.enabled {
                    SkyStatus()
                    Section {
                        SkyFilterControls()
                        ForEach(sky.filtered) { plane in
                            NavigationLink {
                                SkyDetail(plane: plane, savedFlight: nil)
                            } label: {
                                VStack(alignment: .leading, spacing: 4) {
                                    Label(plane.name, systemImage: "airplane")
                                        .font(.headline)
                                    Text([plane.typeCode, sky.area.map { area in
                                        String(format: "%.1f mi from search center", area.center.miles(to: plane.point))
                                    }].compactMap { $0 }.joined(separator: " · "))
                                        .font(.subheadline).foregroundStyle(.secondary)
                                }
                                .padding(.vertical, 3)
                            }
                        }
                        if sky.filtered.isEmpty, sky.state == .ready {
                            let noReports = sky.aircraft.isEmpty
                            ContentUnavailableView(noReports ? "No aircraft reported" : "No matches",
                                systemImage: noReports ? "airplane" : "magnifyingglass",
                                description: Text(noReports ? "Try a larger search area or refresh later." : "Try another name or aircraft filter."))
                        }
                    } header: {
                        Text("Aircraft · \(sky.rangeMiles) mi")
                    } footer: {
                        SkyAttribution()
                    }
                } else {
                    SkyConsent()
                }
                Section {
                    NavigationLink {
                        SkySavedView()
                    } label: {
                        Label("Saved flights", systemImage: "bookmark")
                    }
                }
            }
            .navigationTitle("Nearby")
            .modifier(SkySessionControls())
            .refreshable { if sky.enabled && !sky.busy { sky.refresh() } }
        }
    }
}

struct SkyMapView: View {
    @EnvironmentObject private var sky: SkyStore
    @State private var camera: MapCameraPosition = .automatic
    @State private var selected: String?
    @State private var detail: SkyAircraft?
    @State private var showTrails = true
    @State private var browseMap = false
    @State private var showSearchConsent = false

    var body: some View {
        NavigationStack {
            Group {
                if sky.area != nil || browseMap {
                    VStack(spacing: 0) {
                        HStack {
                            Text(sky.enabled ? "Within \(sky.rangeMiles) mi" : "Explore the map").font(.headline)
                            Spacer()
                            Menu {
                                if let area = sky.area {
                                    SkyFilterPickers()
                                    if !sky.search.isEmpty {
                                        Button("Clear “\(sky.search)” search") { sky.search = "" }
                                    }
                                    Toggle("Recent trails", isOn: $showTrails)
                                    Button("Center search area") { center(on: area) }
                                } else if !sky.enabled {
                                    Button("Find nearby aircraft") { showSearchConsent = true }
                                }
                            } label: { Label("Map options", systemImage: "slider.horizontal.3").labelStyle(.iconOnly) }
                        }
                        .padding(.horizontal).padding(.vertical, 8)
                        Map(position: $camera, selection: $selected) {
                            ForEach(sky.filtered) { plane in
                                if showTrails, let history = sky.trails.points[plane.id], history.count > 1 {
                                    MapPolyline(coordinates: history.map { $0.point.coordinate })
                                        .stroke(.teal, lineWidth: 2)
                                }
                                Marker(plane.name, systemImage: "airplane", coordinate: plane.point.coordinate)
                                    .tint(.teal).tag(plane.id)
                            }
                        }
                        .mapControls { MapCompass(); MapScaleView() }
                        .safeAreaInset(edge: .bottom) {
                            VStack(alignment: .leading, spacing: 4) {
                                if sky.enabled { SkyStatus(); SkyAttribution() }
                                else {
                                    Button("Find nearby aircraft") { showSearchConsent = true }
                                        .buttonStyle(.borderedProminent)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding().background(.regularMaterial)
                        }
                        .onAppear { if let area = sky.area { center(on: area) } }
                        .onChange(of: sky.area) { _, next in
                            if let next { center(on: next) }
                        }
                        .onChange(of: sky.rangeMiles) { _, _ in if let area = sky.area { center(on: area) } }
                        .onChange(of: selected) { _, newValue in
                            if let newValue { detail = sky.filtered.first { $0.id == newValue } }
                        }
                    }
                } else {
                    List {
                        if sky.enabled { SkyStatus() } else { SkyConsent() }
                        Section {
                            Button("Browse Apple Maps without location") {
                                camera = .rect(.world)
                                browseMap = true
                            }
                        } footer: { Text("Apple receives the map area you view.") }
                    }
                }
            }
            .navigationTitle("Map")
            .navigationBarTitleDisplayMode(.inline)
            .modifier(SkySessionControls())
            .sheet(isPresented: $showSearchConsent) {
                NavigationStack {
                    List { SkyConsent() }.navigationTitle("Aircraft feed")
                        .toolbar { Button("Done") { showSearchConsent = false } }
                        .onChange(of: sky.enabled) { _, enabled in if enabled { showSearchConsent = false } }
                }
            }
            .sheet(item: $detail, onDismiss: { selected = nil }) { plane in
                NavigationStack {
                    SkyDetail(plane: plane, savedFlight: nil)
                        .toolbar { Button("Done") { detail = nil } }
                }
            }
        }
    }

    private func center(on area: SkyArea) {
        let diameter = Double(sky.rangeMiles) * 1609.344 * 2.3
        camera = .region(MKCoordinateRegion(center: area.center.coordinate, latitudinalMeters: diameter,
                                            longitudinalMeters: diameter))
    }
}

private struct SkyConsent: View {
    @EnvironmentObject var sky: SkyStore
    var body: some View {
        Section {
            Label("Nearby aircraft", systemImage: "airplane.circle.fill")
                .font(.title2.bold()).foregroundStyle(.teal)
            Text("Look up aircraft reported within your chosen range. The request shares your location rounded to 0.1° with adsb.lol. Opening the map shares the viewed area with Apple Maps.")
            if case .failed(let message) = sky.state {
                Text(message).font(.callout).foregroundStyle(.secondary)
                if let url = URL(string: UIApplication.openSettingsURLString) {
                    Link("Open Settings", destination: url)
                }
            }
            Button("Use my search area") { sky.start() }
                .accessibilityIdentifier("sky.start")
        } footer: { SkyAttribution() }
    }
}

private struct SkyAttribution: View {
    var body: some View {
        Link("Aircraft data from adsb.lol · ODbL", destination: URL(string: "https://www.adsb.lol/docs/open-data/api")!)
            .font(.caption)
    }
}

private struct SkyStatus: View {
    @EnvironmentObject var sky: SkyStore
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            switch sky.state {
            case .locating: Label("Finding your search area…", systemImage: "location")
            case .loading: Label("Updating aircraft…", systemImage: "arrow.triangle.2.circlepath")
            case .failed(let message):
                Label(message, systemImage: "exclamationmark.circle")
                Button("Try again") { sky.refresh() }
            case .ready:
                Label("\(sky.aircraft.count) aircraft reported", systemImage: "antenna.radiowaves.left.and.right")
            case .off: EmptyView()
            }
            if let date = sky.received {
                TimelineView(.periodic(from: date, by: 15)) { time in
                    let age = max(0, time.date.timeIntervalSince(date))
                    let cached = age >= 120 || (sky.state != .ready && sky.state != .loading)
                    let prefix = cached ? "Last successful update" : "Updated"
                    Text("\(prefix) \(date.formatted(.relative(presentation: .numeric)))")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .font(.subheadline)
    }
}

private struct SkySessionControls: ViewModifier {
    @EnvironmentObject var sky: SkyStore
    @Environment(\.scenePhase) var scenePhase
    func body(content: Content) -> some View {
        content.toolbar {
            if sky.enabled {
                Menu {
                    Button("Refresh aircraft") { sky.refresh() }.disabled(sky.busy)
                    Button("Update search location") { sky.locate() }
                    Toggle("Refresh every minute", isOn: $sky.automaticRefresh)
                    Button("Stop aircraft feed", role: .destructive) { sky.stop() }
                } label: {
                    Label("Aircraft feed controls", systemImage: "ellipsis.circle")
                }
            }
        }
        .task(id: sky.automaticRefresh && scenePhase == .active) {
            guard sky.automaticRefresh, scenePhase == .active else { return }
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(60)) } catch { return }
                if sky.enabled, !sky.busy, sky.area != nil { sky.refresh() }
            }
        }
    }
}

private struct SkyFilterControls: View {
    @EnvironmentObject var sky: SkyStore
    var body: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
            TextField("Search flights or aircraft", text: $sky.search)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
            Menu { SkyFilterPickers() } label: {
                Label("Aircraft filters", systemImage: "line.3.horizontal.decrease.circle").labelStyle(.iconOnly)
            }
        }
        if sky.filter != .all { Text(sky.filter.rawValue).font(.caption).foregroundStyle(.secondary) }
    }
}

private struct SkyFilterPickers: View {
    @EnvironmentObject var sky: SkyStore
    var body: some View {
        Picker("Range", selection: Binding(get: { sky.rangeMiles }, set: { sky.setRange($0) })) {
            ForEach([1, 5, 10, 25, 50], id: \.self) { miles in Text("\(miles) mi").tag(miles) }
        }
        Picker("Show", selection: $sky.filter) {
            ForEach(SkyFilter.allCases) { filter in Text(filter.rawValue).tag(filter) }
        }
    }
}

private struct SkyDetail: View {
    @EnvironmentObject var sky: SkyStore
    let plane: SkyAircraft
    let savedFlight: SkySavedFlight?
    @State private var showRoute = false
    var body: some View {
        let history = savedFlight?.trail ?? sky.trails.points[plane.id] ?? []
        List {
            Section("Aircraft report") {
                if let value = plane.typeCode { LabeledContent("Type", value: value) }
                LabeledContent("ICAO address", value: plane.id.uppercased())
                if let value = plane.registration { LabeledContent("Registration", value: value) }
                if plane.onGround { LabeledContent("Position", value: "On the ground") }
                else if let value = plane.altitudeFeet { LabeledContent("Altitude", value: "\(value.formatted()) ft") }
                if let value = plane.speedKnots { LabeledContent("Ground speed", value: "\(Int(value.rounded())) kt") }
                LabeledContent("Observed", value: plane.observedAt.formatted(date: .abbreviated, time: .standard))
                SkyAttribution()
            }
            if !history.isEmpty {
                Section("Observed trail") {
                    LabeledContent("Positions", value: String(history.count))
                    if history.count > 1 {
                        DisclosureGroup("Show trail on Apple Maps", isExpanded: $showRoute) {
                            if showRoute {
                                Map {
                                    MapPolyline(coordinates: history.map { $0.point.coordinate })
                                        .stroke(.teal, lineWidth: 3)
                                    Marker(plane.name, coordinate: plane.point.coordinate)
                                }.frame(height: 200)
                            }
                        }
                    }
                }
            }
            Section {
                if let savedFlight {
                    LabeledContent("Saved on this phone", value: savedFlight.savedAt.formatted())
                } else {
                    let alreadySaved = sky.saved.contains { $0.aircraft.id == plane.id && $0.aircraft.observedAt == plane.observedAt }
                    Button(alreadySaved ? "Flight saved" : "Save flight on this phone") { sky.save(plane) }
                        .disabled(alreadySaved)
                }
                if let message = sky.storageMessage { Text(message).font(.footnote) }
            }
        }
        .navigationTitle(plane.name)
    }
}

private struct SkySavedView: View {
    @EnvironmentObject var sky: SkyStore
    var body: some View {
        List {
            if sky.saved.isEmpty {
                ContentUnavailableView("No saved flights", systemImage: "bookmark",
                    description: Text("Open an aircraft report and choose Save flight on this phone."))
            }
            ForEach(sky.saved) { flight in
                NavigationLink {
                    SkyDetail(plane: flight.aircraft, savedFlight: flight)
                } label: {
                    VStack(alignment: .leading) {
                        Text(flight.aircraft.name).font(.headline)
                        Text(flight.savedAt.formatted()).font(.subheadline).foregroundStyle(.secondary)
                    }
                }
            }
            .onDelete { offsets in sky.removeSaved(Set(offsets.map { sky.saved[$0].id })) }
            if let message = sky.storageMessage { Text(message).font(.footnote) }
        }
        .navigationTitle("Saved flights")
    }
}
