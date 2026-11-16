import SwiftUI

struct ReferenceEntry: Decodable, Identifiable {
    let id: String
    let kind: String
    let name: String
    let manufacturer: String
    let group: String
    let codes: [String]?
    let photo: String?

    var photoURL: URL? {
        guard let photo else { return nil }
        return Bundle.main.resourceURL?.appendingPathComponent(photo)
    }

    static let catalog: [Self] = {
        guard let url = Bundle.main.url(forResource: "ReferenceIndex", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode([Self].self, from: data)) ?? []
    }()

    func matches(_ query: String) -> Bool {
        query.isEmpty || ([name, manufacturer, group] + (codes ?? [])).contains { $0.localizedCaseInsensitiveContains(query) }
    }
}

struct ReferenceGuideView: View {
    @State private var kind = "Aircraft"
    @State private var search = ""
    private var entries: [ReferenceEntry] {
        ReferenceEntry.catalog.filter { $0.kind == kind && $0.matches(search.trimmingCharacters(in: .whitespacesAndNewlines)) }
    }

    var body: some View {
        List {
            Picker("Collection", selection: $kind) {
                Text("Aircraft").tag("Aircraft")
                Text("Drones").tag("Drone")
            }.pickerStyle(.segmented)
            if entries.isEmpty {
                ContentUnavailableView.search(text: search)
            } else {
                Section("\(entries.count) models") {
                    ForEach(entries) { item in
                        NavigationLink {
                            List {
                                if let url = item.photoURL, let image = UIImage(contentsOfFile: url.path) {
                                    Section {
                                        Image(uiImage: image).resizable().scaledToFit().frame(maxWidth: .infinity)
                                    } footer: {
                                        Text(item.photo == "drones/shahed_136.jpg" ? "Reference illustration" : "Reference photo")
                                    }
                                }
                                LabeledContent("Manufacturer", value: item.manufacturer)
                                LabeledContent("Type", value: item.group)
                                if let codes = item.codes, !codes.isEmpty {
                                    LabeledContent("ICAO codes", value: codes.joined(separator: ", "))
                                }
                            }.navigationTitle(item.name)
                        } label: {
                            VStack(alignment: .leading) {
                                Text(item.name)
                                Text(item.group).font(.subheadline).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
        }.navigationTitle("Reference guide")
        .searchable(text: $search, prompt: "Model, manufacturer or type code")
    }
}
