#include "config.h"
#include "macos_environment.hpp"
#include "macos_app.hpp"
#include "macos_website.hpp"
#include "storage.hpp"
#include "process.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace blocker {
ResourceDefinition validateResource(ResourceDefinition definition) {
    require(definition.type == "app" || definition.type == "website", "Unknown resource type");
    if (definition.type == "app") {
        auto path = fs::canonical(definition.target);
        require(path.extension() == ".app" && !path.string().starts_with("/System/") &&
                    !path.string().starts_with("/Library/Apple/"),
                "Select a third-party app bundle");
        struct stat metadata{};
        require(stat(path.c_str(), &metadata) == 0 && S_ISDIR(metadata.st_mode),
                "Not an app directory");
        require(definition.user > 0, "Select a non-root user UID");
        definition.target = path.string();
        definition.device = metadata.st_dev;
        definition.inode = metadata.st_ino;
    } else {
        auto& hostname = definition.target;
        std::transform(hostname.begin(), hostname.end(), hostname.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        });
        if (!hostname.empty() && hostname.back() == '.')
            hostname.pop_back();
        require(!hostname.empty() && hostname.size() <= 253 && hostname.front() != '-' &&
                    hostname.back() != '-' &&
                    std::all_of(hostname.begin(), hostname.end(),
                                [](unsigned char c) {
                                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                           c == '-' || c == '.';
                                }),
                "Use an ASCII hostname, not a URL or wildcard");
    }
    return definition;
}

MacOSEnvironment::MacOSEnvironment(fs::path state, fs::path executable)
    : state_(fs::absolute(state)), executable_(fs::canonical(executable)) {
    require(geteuid() == 0, "Live execution requires root");
    struct stat st{};
    require(stat(executable_.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022),
            "Install the backend as a root-owned, non-writable executable before live use");
    for (auto parent = executable_.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        require(stat(parent.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022),
                "Installed executable ancestors must be root-owned and not group/world writable");
        if (parent == parent.root_path())
            break;
    }
    require(state_ == fs::path(PBL_STATE), "Live mode uses the fixed system state directory");
}

std::unique_ptr<Resource> MacOSEnvironment::resource(const ResourceDefinition& definition) {
    if (definition.type == "app")
        return std::make_unique<MacOSApp>(definition, state_);
    return std::make_unique<MacOSWebsite>(definition, state_);
}

void MacOSEnvironment::synchronizeJobs(const std::vector<Operation>& operations, Timestamp now) {
    require(unsetenv("TZ") == 0, "Cannot select system timezone");
    tzset();
    fs::path directory = "/Library/LaunchDaemons";
    fs::create_directories(directory);
    const std::string prefix = "local.productivityblocker.";
    std::map<std::string, std::string> expected;
    expected[prefix + "renew"] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
                                 "<key>Label</key><string>" +
                                 prefix +
                                 "renew</string><key>ProgramArguments</key><array><string>" +
                                 xml(executable_.string()) +
                                 "</string><string>renew</string></array>"
                                 "<key>StartCalendarInterval</key><dict><key>Hour</key><integer>3</"
                                 "integer><key>Minute</key><integer>15</integer></dict>"
                                 "<key>RunAtLoad</key><true/></dict></plist>\n";
    for (const auto& operation : operations)
        if (!operation.set && operation.time > now) {
            std::time_t timestamp = operation.time;
            std::tm date{};
            require(localtime_r(&timestamp, &date) != nullptr, "Invalid calendar timestamp");

            require(date.tm_sec == 0, "Future scheduled operations must use whole minutes");
            std::string label = prefix + operation.id();
            std::ostringstream out;
            out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n"
                << "<key>Label</key><string>" << xml(label)
                << "</string>\n<key>ProgramArguments</key><array>";
            std::vector<std::string> arguments{executable_.string()};
            arguments.insert(arguments.end(), {"execute", operation.id()});
            for (const auto& argument : arguments)
                out << "<string>" << xml(argument) << "</string>";
            out << "</array>\n<key>StartCalendarInterval</key><dict>";
            for (auto [key, value] :
                 std::vector<std::pair<std::string, int>>{{"Month", date.tm_mon + 1},
                                                          {"Day", date.tm_mday},
                                                          {"Hour", date.tm_hour},
                                                          {"Minute", date.tm_min}})
                out << "<key>" << key << "</key><integer>" << value << "</integer>";
            out << "</dict>\n<key>RunAtLoad</key><true/>\n</dict></plist>\n";
            expected[label] = out.str();
        }

    for (const auto& [label, contents] : expected) {
        auto path = directory / (label + ".plist");
        bool changed = !fs::exists(path) || readText(path) != contents;
        if (changed) {
            if (run({"/bin/launchctl", "print", "system/" + label}).status == 0)
                checked({"/bin/launchctl", "bootout", "system/" + label});
            atomicWrite(path, contents);
        }
        if (run({"/bin/launchctl", "print", "system/" + label}).status != 0)
            checked({"/bin/launchctl", "bootstrap", "system", path.string()});
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto label = entry.path().stem().string();
        if (entry.path().extension() != ".plist" || !label.starts_with(prefix) ||
            expected.contains(label))
            continue;
        if (run({"/bin/launchctl", "print", "system/" + label}).status == 0)
            checked({"/bin/launchctl", "bootout", "system/" + label});
        fs::remove(entry.path());
    }
}
}
