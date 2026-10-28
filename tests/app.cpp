#include "macos_app.hpp"
#include "macos_environment.hpp"
#include "process.hpp"
#include "storage.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace blocker;

struct Fixture {
    fs::path root, app;
    unsigned user;

    Fixture() : user(std::stoul(std::getenv("SUDO_UID"))) {
        char name[] = "/tmp/pbl-app-test-XXXXXX";
        require(mkdtemp(name), "Create fixture directory");
        root = name;
        chmod(root.c_str(), 0755);
        app = root / "Fixture.app";
    }

    ~Fixture() {
        for (const auto& entry : fs::directory_iterator(root))
            chflags(entry.path().c_str(), 0);
        fs::remove_all(root);
    }

    void create(const std::string& identifier = "local.pbl.fixture", mode_t mode = 0755) {
        fs::create_directories(app / "Contents/MacOS");
        atomicWrite(root / "fixture.cpp", "int main() { return 0; }\n");
        checked({"/usr/bin/xcrun", "clang++", (root / "fixture.cpp").string(), "-o",
                 (app / "Contents/MacOS/fixture").string()});
        atomicWrite(app / "Contents/Info.plist",
                    "<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>" +
                        identifier +
                        "</string><key>CFBundleExecutable</key><string>fixture</string>"
                        "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>",
                    0644);
        checked({"/usr/bin/codesign", "--force", "--sign", "-", "--identifier", identifier,
                 "--timestamp=none", app.string()});
        require(chown(app.c_str(), user, 20) == 0 && chmod(app.c_str(), mode) == 0,
                "Set fixture metadata");
    }

    ResourceDefinition definition() {
        return validateResource({"fixture", "app", app.string(), user});
    }

    bool executable() {
        auto pid = fork();
        require(pid >= 0, "Fork access check");
        if (!pid) {
            if (setgid(20) || setuid(user)) {
                std::cerr << "Set fixture user: " << strerror(errno) << '\n';
                _exit(43);
            }
            execl((app / "Contents/MacOS/fixture").c_str(), "fixture", nullptr);
            if (errno != EACCES)
                std::cerr << "Fixture exec: " << strerror(errno) << '\n';
            _exit(errno == EACCES ? 42 : 43);
        }
        int status;
        require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                    (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 42),
                "Access check must execute or fail with EACCES; status=" + std::to_string(status));
        return WEXITSTATUS(status) == 0;
    }
};

std::string metadata(const fs::path& path) {
    struct stat st{};
    require(stat(path.c_str(), &st) == 0, "Read fixture metadata");
    auto acl = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
    if (!acl && errno == ENOENT)
        acl = acl_init(0);
    require(acl, "Read fixture ACL");
    auto text = acl_to_text(acl, nullptr);
    require(text, "Encode fixture ACL");
    std::ostringstream out;
    out << st.st_uid << ' ' << st.st_gid << ' ' << (st.st_mode & 07777) << ' ' << st.st_flags << ' '
        << text;
    acl_free(text);
    acl_free(acl);
    return out.str();
}

template <class F> void rejected(F action, const std::string& message) {
    try {
        action();
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(message) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("Expected rejection: " + message);
}

void replacement() {
    Fixture f;
    f.create();
    require(f.executable(), "Baseline fixture executes");
    auto definition = f.definition();
    require(definition.requirement.empty(), "Ad-hoc apps use explicit location policy");
    const fs::path signedApp = "/System/Applications/Utilities/Terminal.app";
    auto identity = appRequirement(signedApp);
    require(!identity.empty() && appRequirement(signedApp, identity) == identity,
            "Capture and verify an actual certificate-signed app identity without modifying it");
    // Exercise saved-requirement matching without needing a developer certificate in CI.
    definition.requirement = "identifier \"local.pbl.fixture\"";
    Schedule schedule;
    schedule.resources[definition.id] = definition;
    saveSchedule(f.root, schedule);
    definition = loadSchedule(f.root).resources.at(definition.id);
    MacOSApp app(definition, f.root);
    auto original = metadata(f.app);
    app.block();
    app.block();
    require(!f.executable(), "Repeated block denies actual execution");
    app.unblock();
    require(metadata(f.app) == original && f.executable(), "Restore original metadata");

    fs::rename(f.app, f.root / "old.app");
    f.create("local.pbl.fixture", 0701);
    auto replacementMetadata = metadata(f.app);
    app.block();
    require(!f.executable(), "Same identity at a new inode is blocked");
    app.unblock();
    require(metadata(f.app) == replacementMetadata && f.executable(),
            "Replacement restores its own metadata, not the previous installation's");

    fs::rename(f.app, f.root / "updated.app");
    f.create("local.pbl.unrelated");
    auto unrelated = metadata(f.app);
    rejected(
        [&] {
            app.block();
        },
        "signing identity changed");
    require(metadata(f.app) == unrelated && f.executable(), "Wrong identity stays untouched");
    atomicWrite(f.root / "schedule.txt", "BLOCKER 1 0 1\nresource \"fixture\" app \"" +
                                             f.app.string() + "\" " + std::to_string(f.user) +
                                             " 123 456\n");
    auto legacy = loadSchedule(f.root).resources.at("fixture");
    require(legacy.requirement.empty(), "Legacy inode is not permanent app policy");
    MacOSApp locationPolicy(legacy, f.root);
    locationPolicy.block();
    locationPolicy.unblock();
    require(metadata(f.app) == unrelated, "Legacy location policy accepts a fresh installation");
}

void restoration() {
    Fixture f;
    f.create();
    require(f.executable(), "Baseline fixture executes");
    auto definition = f.definition();
    auto original = metadata(f.app);
    MacOSApp app(definition, f.root);
    app.block();
    // Convert the original journal to the previous release's name/format.
    for (const auto& entry : fs::directory_iterator(f.root))
        if (entry.path().extension() == ".metadata") {
            auto text = readText(entry.path());
            text.replace(0, 9, "METADATA1");
            text.erase(text.rfind(' '));
            text.erase(text.rfind(' '));
            text += '\n';
            atomicWrite(f.root / "fixture.metadata", text);
            fs::remove(entry.path());
            break;
        }
    require(chflags(f.app.c_str(), 0) == 0, "Allow root to relocate protected fixture");
    fs::rename(f.app, f.root / "old.app");
    f.create("local.pbl.fixture", 0711);
    auto replacementMetadata = metadata(f.app);
    rejected(
        [&] {
            app.unblock();
        },
        "replacement left untouched");
    require(metadata(f.app) == replacementMetadata && fs::exists(f.root / "fixture.metadata"),
            "A mismatched old journal cannot change the replacement or disappear");
    rejected(
        [&] {
            app.block();
        },
        "Previous installation needs restoration");
    require(!f.executable(), "Replacement is protected with a separate journal");
    rejected(
        [&] {
            app.unblock();
        },
        "Previous installation needs restoration");
    require(metadata(f.app) == replacementMetadata && f.executable(),
            "Restore the replacement while preserving the unresolved original journal");
    fs::rename(f.app, f.root / "replacement.app");
    fs::rename(f.root / "old.app", f.app);
    app.unblock();
    require(metadata(f.app) == original && f.executable() &&
                !fs::exists(f.root / "fixture.metadata"),
            "Verified original installation recovers using the legacy journal");
}

int main(int argc, char** argv) {
    if (geteuid() != 0 || !std::getenv("SUDO_UID") || !std::stoul(std::getenv("SUDO_UID"))) {
        std::cout << "Run with sudo to test real app permissions on disposable bundles\n";
        return 77;
    }
    try {
        require(argc == 2, "Choose replacement or restoration");
        if (std::string(argv[1]) == "replacement")
            replacement();
        else {
            require(std::string(argv[1]) == "restoration", "Unknown test");
            restoration();
        }
        std::cout << "PASS " << argv[1] << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
