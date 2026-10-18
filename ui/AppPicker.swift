import SwiftUI

struct InstalledApp: Identifiable, Sendable {
    var name: String
    var path: String
    var id: String {
        path
    }

    static func discover() -> [Self] {
        let roots = ["/Applications", NSHomeDirectory() + "/Applications"]
        var apps = [Self]()
        for root in roots {
            guard let files = FileManager.default.enumerator(
                at: URL(fileURLWithPath: root),
                includingPropertiesForKeys: nil,
                options: [.skipsHiddenFiles]
            ) else {
                continue
            }
            for case let url as URL in files where url.pathExtension == "app" {
                files.skipDescendants()
                apps.append(Self(
                    name: FileManager.default.displayName(atPath: url.path).replacingOccurrences(
                        of: ".app",
                        with: ""
                    ),
                    path: url.path
                ))
            }
        }
        return apps.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
}

struct AppPicker: View {
    @Environment(\.colorScheme) private var scheme
    var add: ([InstalledApp]) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var apps = [InstalledApp]()
    @State private var search = ""
    @State private var selected = Set<String>()
    @State private var loading = true
    var body: some View {
        VStack(spacing: 16) {
            TextField("Search apps", text: $search).textFieldStyle(.plain).font(.system(size: 13))
            if loading {
                ProgressView().frame(maxHeight: .infinity)
            } else {
                List(apps
                    .filter { search.isEmpty || $0.name.localizedCaseInsensitiveContains(search)
                    }) { app in
                        Button {
                            if !selected.insert(app.id).inserted {
                                selected.remove(app.id)
                            }
                        } label: {
                            HStack {
                                Text(app.name)
                                Spacer()
                                Image(systemName: selected
                                    .contains(app.id) ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(.secondary)
                            }.padding(.vertical, 6)
                        }
                        .buttonStyle(HoverButtonStyle())
                        .help(app.path)
                    }
                    .listStyle(.plain)
                    .scrollContentBackground(.hidden)
            }
            HStack {
                Button("Cancel") { dismiss() }
                    .buttonStyle(ActionButtonStyle())
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Add") {
                    add(apps.filter { selected.contains($0.id) })
                    dismiss()
                }
                .buttonStyle(ActionButtonStyle())
                .disabled(selected.isEmpty)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 380, height: 440)
        .background(AppPalette.background(scheme))
        .task {
            apps = await Task.detached(priority: .userInitiated) { InstalledApp.discover() }
                .value
            loading = false
        }
    }
}
