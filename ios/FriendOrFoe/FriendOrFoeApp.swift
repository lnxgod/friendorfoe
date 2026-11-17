import SwiftUI

@main
struct FriendOrFoeApp: App {
    @State private var completedWelcome = UserDefaults.standard.bool(forKey: "completedWelcome")
    @AppStorage("appearance") private var appearance = Appearance.system
    @StateObject private var sky = SkyStore()
    @StateObject private var station = StationStore()

    var body: some Scene {
        WindowGroup {
            Group {
                if completedWelcome {
                    TabView {
                        PrivacyView()
                            .tabItem { Label("Privacy", systemImage: "shield") }
                        NearbyView()
                            .tabItem { Label("Nearby", systemImage: "airplane") }
                        SkyMapView()
                            .tabItem { Label("Map", systemImage: "map") }
                        NavigationStack { StationView() }
                            .tabItem { Label("Station", systemImage: "antenna.radiowaves.left.and.right") }
                        NavigationStack { MoreView() }
                            .tabItem { Label("More", systemImage: "ellipsis.circle") }
                    }
                } else {
                    WelcomeView {
                        UserDefaults.standard.set(true, forKey: "completedWelcome")
                        completedWelcome = true
                    }
                }
            }
            .environmentObject(sky)
            .environmentObject(station)
            .preferredColorScheme(appearance.scheme)
        }
    }
}

enum Appearance: String, CaseIterable {
    case system = "System"
    case light = "Light"
    case dark = "Dark"

    var scheme: ColorScheme? {
        switch self {
        case .system: nil
        case .light: .light
        case .dark: .dark
        }
    }
}

struct PublisherCredit: View {
    var body: some View {
        Link(destination: URL(string: "https://gamechangersai.org/")!) {
            HStack(spacing: 10) {
                Image("BrandMark")
                    .resizable().scaledToFit().frame(width: 40, height: 40)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                VStack(alignment: .leading) {
                    Text("Published by").font(.caption).foregroundStyle(.secondary)
                    Text("GameChangers AI").font(.headline).foregroundStyle(.primary)
                }
                Image(systemName: "arrow.up.right").imageScale(.small)
            }
        }
        .accessibilityLabel("GameChangers AI, publisher website")
    }
}

struct WelcomeView: View {
    let getStarted: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                Image(systemName: "dot.radiowaves.left.and.right")
                    .font(.system(size: 42, weight: .medium))
                    .foregroundStyle(Color.accentColor).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 8) {
                    Text("Friend or Foe").font(.largeTitle.bold())
                    Text("Explore nearby devices and aircraft.").font(.title3).foregroundStyle(.secondary)
                }
                PublisherCredit()
                Divider()
                welcomeFeature("shield", "Privacy", "Look for signals from personal recorders, cameras and trackers.")
                welcomeFeature("airplane", "Nearby aircraft", "See aircraft positions on a map and save flights.")
                welcomeFeature("antenna.radiowaves.left.and.right", "Scanner station", "See Wi-Fi findings from a station you connect.")
            }
            .frame(maxWidth: 560, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading).padding(24)
        }
        .safeAreaInset(edge: .bottom) {
            VStack(spacing: 12) {
                Text("You choose when to scan and when to share a location.")
                    .font(.footnote).foregroundStyle(.secondary)
                Button(action: getStarted) {
                    Text("Open Privacy").font(.headline).frame(maxWidth: .infinity).padding(.vertical, 6)
                }.buttonStyle(.borderedProminent).tint(Color("ActionFill")).accessibilityIdentifier("openPrivacy")
            }.padding().background(.regularMaterial)
        }
    }

    private func welcomeFeature(_ symbol: String, _ title: String, _ detail: String) -> some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: symbol).foregroundStyle(Color.accentColor).font(.title2)
                .frame(width: 28).accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(detail).foregroundStyle(.secondary)
            }
        }
    }
}
