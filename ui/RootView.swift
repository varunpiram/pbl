import SwiftUI
import AppKit

struct RootView: View {
    @ObservedObject var model: BlockerModel
    @State var pane = Pane.quick
    @AppStorage("selectedSet") var selected = ""
    @State var minutes = 60.0
    @AppStorage("appearance") var appearance = "System"
    @State var systemDark = NSApp.effectiveAppearance
        .bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
    @State var resourceName = ""
    @State var resourceKind: ResourceKind?
    @State var selectedDay = 1
    @State var expanded: String?
    @State var scheduleToClear: String?
    @State var setToDelete: String?
    @State var now = Date()
    @Environment(\.colorScheme) var colorScheme
    let heartbeat = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    var accent: Color {
        colorScheme == .dark ? Color(red: 0.88, green: 0.86, blue: 0.81) : Color(
            red: 0.20,
            green: 0.19,
            blue: 0.17
        )
    }

    let days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
    var index: Int? {
        model.sets.firstIndex { $0.id == selected }
    }

    var surface: Color {
        AppPalette.surface(colorScheme)
    }

    var background: Color {
        AppPalette.background(colorScheme)
    }

    var body: some View {
        HStack(spacing: 0) {
            sidebar
            VStack(alignment: .leading, spacing: 0) {
                if pane != .quick && !model.sets.isEmpty {
                    header
                }
                Group {
                    switch pane {
                    case .quick: quickPane
                    case .sets:
                        if model.sets.isEmpty {
                            VStack(spacing: 24) {
                                emptyView("Create a set to get started", symbol: "square.stack")
                                Button("Create set") { addSet() }.buttonStyle(ActionButtonStyle())
                            }.frame(maxWidth: .infinity, maxHeight: .infinity)
                        } else {
                            setsPane
                        }
                    case .schedule: schedulePane
                    }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .background(background)
        }
        .background(background)
        .background(WindowTheme(dark: colorScheme == .dark).frame(width: 0, height: 0))
        .preferredColorScheme(appearance == "System" ? (systemDark ? .dark : .light) : appearance ==
            "Dark" ? .dark : .light)
        .onReceive(NSApp.publisher(for: \.effectiveAppearance)) { systemDark = $0.bestMatch(from: [
            .darkAqua,
            .aqua
        ]) == .darkAqua }
        .tint(accent)
        .onAppear { model.refresh()
            model.promptForBlockingAccess()
            expanded = selected
        }
        .onReceive(model.$sets) { _ in now = Date() }
        .onChange(of: model.sets.map(\.id)) { _, ids in
            if !ids.contains(selected) {
                selected = ids.first ?? ""
            }
        }
        .disabled(model.busy)
        .alert(
            "pbl",
            isPresented: Binding(get: { model.error != nil }, set: { if !$0 {
                model.error = nil
            } })
        ) {
            if model.error?.contains("Full Disk Access") == true {
                Button("Open Full Disk Access") { model.openBlockingAccess() }
                Button("Retry") { model.error = nil
                    model.refresh()
                }
            }
            Button("OK") { model.error = nil }
        } message: { Text(model.error?.contains("Full Disk Access") == true
                ? BlockerModel.blockingAccessInstructions : model.error ?? "") }
        .onReceive(NotificationCenter.default.publisher(for: .NSSystemTimeZoneDidChange)) { _ in
            now = Date()
            model.refresh()
        }
        .onReceive(NotificationCenter.default
            .publisher(for: NSApplication.didBecomeActiveNotification))
        { _ in now = Date()
            model.refresh()
        }
        .onReceive(heartbeat) { date in
            if Int(date.timeIntervalSince1970 / 60) != Int(now.timeIntervalSince1970 / 60) ||
                model.sets
                .contains(where: {
                    $0.intervals
                        .contains {
                            now.timeIntervalSince1970 < $0.end && $0.end <= date
                                .timeIntervalSince1970
                        }
                })
            {
                now = date
                model.refresh(reconcile: false)
            }
        }
        .confirmationDialog(
            "Clear weekly schedule?",
            isPresented: Binding(
                get: { scheduleToClear != nil },
                set: { if !$0 {
                    scheduleToClear = nil
                } }
            ),
            titleVisibility: .visible
        ) {
            Button("Clear schedule", role: .destructive) {
                if let index = model.sets.firstIndex(where: { $0.id == scheduleToClear }),
                   model.sets[index].activeEnd(at: Date()) == nil
                {
                    model.edit(index) { $0.windows.removeAll() }
                }
                scheduleToClear = nil
            }
            Button("Cancel", role: .cancel) { scheduleToClear = nil }
        } message: {
            Text(
                "Remove all weekly time ranges for \(model.sets.first(where: { $0.id == scheduleToClear })?.name ?? "this set")?"
            )
        }
        .confirmationDialog(
            "Delete set?",
            isPresented: Binding(
                get: { setToDelete != nil },
                set: { if !$0 {
                    setToDelete = nil
                } }
            ),
            titleVisibility: .visible
        ) {
            Button("Delete set", role: .destructive) {
                if let index = model.sets.firstIndex(where: { $0.id == setToDelete }) {
                    deleteSet(index)
                }
                setToDelete = nil
            }
            Button("Cancel", role: .cancel) { setToDelete = nil }
        } message: {
            Text(
                "Delete \(model.sets.first(where: { $0.id == setToDelete })?.name ?? "this set") and its weekly schedule?"
            )
        }
        .sheet(item: $resourceKind) { kind in
            if kind == .app {
                AppPicker { apps in
                    guard let index else {
                        return
                    }
                    model.edit(index) { set in
                        for app in apps
                            where !set.resources.contains(where: { $0.path == app.path })
                        {
                            set.resources.append(BlockResource(
                                name: app.name,
                                isApp: true,
                                path: app.path
                            ))
                        }
                    }
                }
            } else {
                resourceSheet
            }
        }
    }

    var sidebar: some View {
        VStack(alignment: .leading, spacing: 32) {
            Text("pbl")
                .font(.system(size: 20, weight: .medium))
                .foregroundStyle(accent)
                .padding(.leading, 10)
                .padding(.top, 20)
            VStack(spacing: 7) {
                ForEach(Pane.allCases, id: \.self) { item in
                    Button { pane = item } label: {
                        HStack(spacing: 11) {
                            Image(systemName: item.symbol).font(.system(size: 16)).frame(width: 20)
                            Text(item.rawValue)
                                .font(.system(
                                    size: 13,
                                    weight: pane == item ? .semibold : .regular
                                ))
                                .lineLimit(1)
                                .fixedSize()
                            Spacer()
                        }
                        .contentShape(Rectangle())
                        .foregroundStyle(pane == item ? accent : Color.primary.opacity(0.7))
                        .padding(.horizontal, 12)
                        .padding(.vertical, 12)
                        .background(
                            pane == item ? accent.opacity(0.07) : .clear,
                            in: RoundedRectangle(cornerRadius: 4)
                        )
                    }.buttonStyle(HoverButtonStyle())
                }
            }
            Spacer()
            let active = model.sets.filter { $0.activeEnd(at: now) != nil }
            if !active.isEmpty {
                ScrollView {
                    VStack(alignment: .leading, spacing: 9) {
                        ForEach(active) { set in
                            HStack {
                                Circle().fill(accent).frame(width: 6, height: 6)
                                Text(set.name).lineLimit(1)
                                Spacer()
                                Image(systemName: "lock.fill").font(.system(size: 10))
                            }
                            .font(.system(size: 12))
                            .foregroundStyle(.secondary)
                            .frame(height: 22)
                        }
                    }.padding(.horizontal, 10)
                }.frame(height: min(160, CGFloat(active.count * 31 - 9)))
            }
            HStack(spacing: 3) {
                ForEach(["Light", "Dark", "System"], id: \.self) { mode in
                    Button { appearance = mode } label: {
                        Image(
                            systemName: mode == "Light" ? "sun.max" : mode == "Dark" ? "moon" :
                                "desktopcomputer"
                        )
                        .font(.system(size: 13))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 7)
                        .background(
                            appearance == mode ? accent.opacity(0.1) : .clear,
                            in: RoundedRectangle(cornerRadius: 4)
                        )
                    }
                    .buttonStyle(HoverButtonStyle())
                    .foregroundStyle(appearance == mode ? .primary : .secondary)
                    .accessibilityLabel(mode)
                    .help(mode)
                    .accessibilityAddTraits(appearance == mode ? .isSelected : [])
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.bottom, 22)
        .frame(width: 180)
        .frame(maxHeight: .infinity)
        .background(background)
    }

    var header: some View {
        HStack {
            Spacer()
            if pane == .sets {
                Button { addSet() } label: { Label("New set", systemImage: "plus") }
                    .buttonStyle(ActionButtonStyle())
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 34)
        .padding(.bottom, 24)
    }

    var resourceSheet: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("Add website").font(.system(size: 16, weight: .medium))
            TextField("example.com", text: $resourceName)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .frame(width: 320)
            HStack {
                Spacer()
                Button("Cancel") { resourceKind = nil }
                    .buttonStyle(ActionButtonStyle())
                    .keyboardShortcut(.cancelAction)
                Button("Add") { addResource() }
                    .buttonStyle(ActionButtonStyle())
                    .keyboardShortcut(.defaultAction)
                    .disabled(resourceName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(28)
        .background(background)
    }

    func timeBinding(_ i: Int, _ id: String, _ end: Bool) -> Binding<Int> {
        Binding(get: {
            guard let window = model.sets[i].windows.first(where: { $0.id == id }) else {
                return 0
            }
            return end ? window.end : window.start
        }, set: { minutes in
            guard model.sets[i].activeEnd(at: Date()) == nil,
                  let position = model.sets[i].windows.firstIndex(where: { $0.id == id })
            else {
                return
            }
            model
                .edit(i) {
                    if end {
                        $0.windows[position].end = minutes
                    } else {
                        $0.windows[position].start = minutes
                    }
                }
        })
    }

    func addSet() {
        let set = FocusSet(name: "New set", resources: [])
        model.save(set)
    }

    func deleteSet(_ i: Int) {
        guard model.sets[i].activeEnd(at: Date()) == nil else {
            return
        }
        model.send(["command": "remove", "id": model.sets[i].id])
    }

    func addResource() {
        guard let index else {
            resourceKind = nil
            return
        }
        let name = resourceName.trimmingCharacters(in: .whitespacesAndNewlines)
        if !model.sets[index].resources.contains(where: { $0.name == name }) {
            model.edit(index) { $0.resources.append(BlockResource(name: name)) }
        }
        resourceKind = nil
    }

    func resourceSummary(_ set: FocusSet) -> String {
        let websites = set.resources.filter { !$0.isApp }.count
        let apps = set.resources.filter { $0.isApp }.count
        return "\(websites) \(websites == 1 ? "website" : "websites") · \(apps) \(apps == 1 ? "app" : "apps")"
    }

    func durationLabel(_ value: Double) -> String {
        let minutes = Int(value)
        if minutes < 60 {
            return "\(minutes) min"
        }
        if minutes % 60 == 0 {
            return "\(minutes / 60)h"
        }
        return "\(minutes / 60)h \(minutes % 60)m"
    }

    func countdown(_ end: Date, at now: Date) -> String {
        let seconds = max(0, Int(end.timeIntervalSince(now)))
        return String(format: "%02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60)
    }

    func emptyView(_ title: String, symbol: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: symbol).font(.system(size: 28, weight: .light)).frame(
                width: 44,
                height: 44
            )
            Text(title).font(.system(size: 13))
        }
        .multilineTextAlignment(.center)
        .frame(maxWidth: .infinity)
        .foregroundStyle(.secondary)
    }
}
