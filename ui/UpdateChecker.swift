import AppKit
import CryptoKit

@MainActor enum UpdateChecker {
    struct Release: Decodable {
        let tag_name: String
        let assets: [Asset]
        var version: String {
            String(tag_name.dropFirst())
        }
    }

    struct Asset: Decodable {
        let name, digest: String
        let browser_download_url: URL
    }

    static func check() async {
        let endpoint = URL(string: "https://api.github.com/repos/varunpiram/pbl/releases/latest")!
        let request = URLRequest(url: endpoint, timeoutInterval: 10)
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200,
              let release = try? JSONDecoder().decode(Release.self, from: data),
              release.tag_name.range(of: #"^v\d+\.\d+\.\d+$"#, options: .regularExpression) != nil,
              let installed = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String,
              installed.compare(release.version, options: .numeric) == .orderedAscending,
              let asset = release.assets.first(where: { $0.name == "pbl-macos-arm64.pkg" }),
              asset.browser_download_url.scheme == "https",
              asset.browser_download_url.host == "github.com"
        else {
            return
        }
        let alert = NSAlert()
        alert.icon = NSImage(named: "AppIcon")
        alert.messageText = "pbl \(release.version) is available"
        alert.informativeText = "You're using \(installed). pbl will quit when the installer opens."
        alert.addButton(withTitle: "Download & Install")
        alert.addButton(withTitle: "Later")
        guard alert.runModal() == .alertFirstButtonReturn else {
            return
        }
        alert.messageText = "Downloading update…"
        alert.informativeText = "The macOS installer will open when the download is verified."
        alert.buttons.forEach { $0.isHidden = true }
        alert.window.makeKeyAndOrderFront(nil)
        defer { alert.window.orderOut(nil) }
        do {
            let (download, response) = try await URLSession.shared
                .download(from: asset.browser_download_url)
            defer { try? FileManager.default.removeItem(at: download) }
            let hash = SHA256.hash(data: try Data(contentsOf: download))
            let digest = hash.map { String(format: "%02x", $0) }.joined()
            guard (response as? HTTPURLResponse)?.statusCode == 200,
                  asset.digest == "sha256:" + digest
            else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let folder = FileManager.default
                .temporaryDirectory
                .appendingPathComponent("pbl-update-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
            let package = folder.appendingPathComponent(asset.name)
            try FileManager.default.moveItem(at: download, to: package)
            guard NSWorkspace.shared.open(package) else {
                throw CocoaError(.fileReadUnknown)
            }
            NSApp.terminate(nil)
        } catch {
            alert.window.orderOut(nil)
            NSApp.presentError(error)
        }
    }
}
