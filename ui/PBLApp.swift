import SwiftUI
import AppKit

enum Pane: String, CaseIterable {
    case quick = "Quick Block", sets = "Sets", schedule = "Schedule"
    var symbol: String {
        switch self { case .quick: return "timer"
        case .sets: return "square.stack"
        case .schedule: return "calendar" }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldHandleReopen(
        _ sender: NSApplication,
        hasVisibleWindows flag: Bool
    ) -> Bool {
        for window in sender.windows where window.canBecomeMain {
            window.deminiaturize(nil)
            window.makeKeyAndOrderFront(nil)
        }
        sender.activate(ignoringOtherApps: true)
        return true
    }
}

@main struct PBLApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = BlockerModel()
    var body: some Scene {
        Window("pbl", id: "main") {
            RootView(model: model)
                .frame(minWidth: 920, minHeight: 660)
        }
        .defaultSize(width: 1020, height: 720)
        .windowStyle(.titleBar)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
