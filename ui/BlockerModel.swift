import SwiftUI
import AppKit

struct BlockResource: Identifiable, Codable, Equatable {
    var id = UUID().uuidString
    var name: String
    var isApp = false
    var path: String?
    var symbol: String {
        isApp ? "app" : "globe"
    }
}

struct WeeklyWindow: Identifiable, Codable {
    var id = UUID().uuidString
    var day: Int
    var start: Int
    var end: Int
}

struct BlockInterval: Codable { var start: Double
    var end: Double
}

struct FocusSet: Identifiable, Codable {
    var id = UUID().uuidString
    var name = "New set"
    var resources: [BlockResource] = []
    var windows: [WeeklyWindow] = []
    var intervals: [BlockInterval] = []
    var quickIntervals: [BlockInterval] = []
    func activeInterval(at now: Date, quickOnly: Bool = false) -> DateInterval? {
        (quickOnly ? quickIntervals : intervals)
            .first { $0.start <= now.timeIntervalSince1970 && now.timeIntervalSince1970 < $0.end }
            .map { DateInterval(
                start: Date(timeIntervalSince1970: $0.start),
                end: Date(timeIntervalSince1970: $0.end)
            ) }
    }

    func activeEnd(at now: Date) -> Date? {
        activeInterval(at: now)?.end
    }
}

@MainActor final class BlockerModel: ObservableObject {
    @Published var sets: [FocusSet] = [] {
        didSet {
            UserDefaults.standard.set(sets.map(\.id), forKey: "setOrder")
            for set in sets {
                UserDefaults.standard.set(set.name, forKey: "setName." + set.id)
            }
        }
    }

    @Published var error: String?
    @Published var busy = false
    private var client: BackendClient?
    static let blockingAccessInstructions = "Allow pbl Blocking Service in System Settings → Privacy & Security → Full Disk Access. Drag the selected service from Finder into the list and enable it. macOS also grants access to protected data such as Mail and Messages. Return to pbl and retry. You may need to approve access again after an update."
    func promptForBlockingAccess() {
        guard let build = try? String(contentsOfFile: PBL_HELPER_REQUIREMENT_FILE, encoding: .utf8),
              UserDefaults.standard.string(forKey: "permissionInstructionsShownFor") != build
        else {
            return
        }
        UserDefaults.standard.set(build, forKey: "permissionInstructionsShownFor")
        error = Self.blockingAccessInstructions
    }

    func openBlockingAccess() {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: PBL_BACKEND_BUNDLE)])
        NSWorkspace.shared.open(URL(string:
            "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles")!)
    }

    func refresh(reconcile: Bool = true) {
        send(["command": reconcile ? "renew" : "read"])
    }

    func edit(_ index: Int, change: (inout FocusSet) -> Void) {
        var set = sets[index]
        change(&set)
        save(set)
    }

    func save(_ set: FocusSet) {
        do {
            var input = try JSONSerialization
                .jsonObject(with: JSONEncoder().encode(set)) as! [String: Any]
            input["command"] = "save"
            send(input)
        } catch { self.error = error.localizedDescription }
    }

    func send(_ input: [String: Any]) {
        guard !busy else {
            return
        }
        do {
            if client == nil {
                let requirement = try String(
                    contentsOfFile: PBL_HELPER_REQUIREMENT_FILE,
                    encoding: .utf8
                ).trimmingCharacters(in: .whitespacesAndNewlines)
                client = BackendClient(helperRequirement: requirement)
            }
            let data = try JSONSerialization.data(withJSONObject: input)
            busy = true
            client!.request(data) { result in
                defer { self.busy = false }
                do {
                    var updated = try JSONDecoder().decode([FocusSet].self, from: result.get())
                    let order = UserDefaults.standard.stringArray(forKey: "setOrder") ?? []
                    for i in updated.indices {
                        updated[i].name = UserDefaults.standard
                            .string(forKey: "setName." + updated[i].id) ?? "New set"
                        for j in updated[i].resources.indices where updated[i].resources[j].isApp {
                            updated[i].resources[j]
                                .name = URL(fileURLWithPath: updated[i].resources[j].path!)
                                .deletingPathExtension()
                                .lastPathComponent
                        }
                    }
                    self.sets = updated
                        .sorted {
                            (order.firstIndex(of: $0.id) ?? Int.max) <
                                (order.firstIndex(of: $1.id) ?? Int.max)
                        }
                } catch { self.error = error.localizedDescription
                    self.client = nil
                }
            }
        } catch {
            self.error = "Unable to connect. Reinstall pbl, then reopen it.\n" + error
                .localizedDescription
        }
    }
}
