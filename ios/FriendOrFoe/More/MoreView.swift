import SwiftUI
import WebKit

struct MoreView: View {
    @AppStorage("appearance") private var appearance = Appearance.system
    var body: some View {
        List {
            Section {
                NavigationLink { ReferenceGuideView() } label: {
                    Label("Aircraft & drone guide", systemImage: "books.vertical")
                }
                NavigationLink { LensInspectionView() } label: {
                    Label("Lens inspection", systemImage: "camera")
                }
                NavigationLink { MagneticFieldView() } label: {
                    Label("Magnetic field", systemImage: "waveform.path")
                }
                NavigationLink { SoundCheckView() } label: {
                    Label("Sound check", systemImage: "waveform")
                }
            }
            Section("Settings") {
                Picker("Appearance", selection: $appearance) {
                    ForEach(Appearance.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }
                NavigationLink { PhoneCoverageView() } label: {
                    Label("What this iPhone can see", systemImage: "antenna.radiowaves.left.and.right")
                }
                NavigationLink { PrivacyPolicyView() } label: {
                    Label("Privacy & data", systemImage: "hand.raised")
                }
            }
            Section("About Friend or Foe") {
                PublisherCredit()
                Link("Source code on GitHub", destination: URL(string: "https://github.com/lnxgod/friendorfoe")!)
                LabeledContent("Version", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0")
            }
        }.navigationTitle("More")
    }
}

private struct PhoneCoverageView: View {
    var body: some View {
        List {
            Section("On this iPhone") {
                Text("A foreground Bluetooth scan can recognize some recorders and accessories by a name, service or other broadcast evidence. Devices can go silent or change names. Recording status stays unknown.")
                Text("For iBeacons, enter a known network identifier in Privacy and enable Precise Location for ranging. Unknown beacon UUIDs are only available through a connected station.")
            }
            Section("With a scanner station") {
                Text("A station can report nearby Wi-Fi networks, including the name and the reason a network was flagged. iOS does not give ordinary apps a list of surrounding Wi-Fi networks. Station results belong to that station’s location.")
            }
            Section("Aircraft and maps") {
                Text("Nearby uses an online aircraft feed. Your iPhone does not receive ADS-B radio directly. Data may be delayed or incomplete.")
            }
            Section("Coverage limits") {
                Text("A supported Bluetooth Remote ID Basic ID broadcast can indicate a nearby drone, but the app does not map its position. This edition scans while the app is active. It does not issue continuous background tracker alerts, read the Find My network, or control a badge over USB. Camera and sound observations require your interpretation.")
            }
        }.navigationTitle("iPhone coverage")
    }
}

private struct PrivacyPolicyView: View {
    var body: some View {
        List {
            Section("GameChangers AI") {
                NavigationLink("Read full privacy policy") {
                    if let file = Bundle.main.url(forResource: "iphone-privacy", withExtension: "html") {
                        OfflinePrivacyDocument(file: file).navigationTitle("Privacy policy")
                            .navigationBarTitleDisplayMode(.inline)
                    } else {
                        ContentUnavailableView("Privacy policy unavailable", systemImage: "doc.text")
                    }
                }
                Text("Friend or Foe for iPhone works without an account. It includes no advertising or analytics SDK. Permissions are requested when you use the corresponding feature.")
            }
            Section("Radio observations") {
                Text("Privacy analyzes nearby broadcasts on your iPhone. Bluetooth signals and beacon sightings are not uploaded to GameChangers AI. Use the history and ignore controls in Privacy to manage local observations.")
            }
            Section("Networks and maps") {
                Text("When you choose to look up nearby aircraft, the app sends a rounded search area to the aircraft provider. Maps use Apple Maps. Map and feed requests can reveal an area of interest and your IP address to the service you use.")
                Text("Station connects to the HTTPS server address you enter. That server receives the request and your IP address. Station reports are held in memory.")
            }
            Section("Camera, sound and magnetic field") {
                Text("The inspection tools process camera frames and microphone samples on this iPhone while you use them. The magnetic field tool reads the local sensor only during a sweep. None of these tools save or send their readings.")
            }
            Section("Your choices") {
                Text("You can stop a scan or inspection, remove saved flights, forget a station, or revoke access in iPhone Settings. Deleting the app removes its local files. Providers manage their own records.")
                Link("Contact GameChangers AI", destination: URL(string: "https://gamechangersai.org/about.html")!)
            }
        }.navigationTitle("Privacy & data")
    }
}

private struct OfflinePrivacyDocument: UIViewRepresentable {
    let file: URL

    func makeCoordinator() -> Links { Links(file: file) }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.defaultWebpagePreferences.allowsContentJavaScript = false
        let web = WKWebView(frame: .zero, configuration: config)
        web.navigationDelegate = context.coordinator
        web.loadFileURL(file, allowingReadAccessTo: file)
        return web
    }

    func updateUIView(_ web: WKWebView, context: Context) {}

    final class Links: NSObject, WKNavigationDelegate {
        let file: URL
        init(file: URL) { self.file = file }
        func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction) async -> WKNavigationActionPolicy {
            guard let url = action.request.url else { return .cancel }
            if url == file { return .allow }
            if action.navigationType == .linkActivated && url.scheme == "https" {
                await UIApplication.shared.open(url)
            }
            return .cancel
        }
    }
}
