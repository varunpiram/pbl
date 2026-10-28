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
        definition.requirement = appRequirement(path, definition.requirement);
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

static void synchronizeJob(const fs::path& directory, const std::string& label,
                           const std::string& contents) {
    auto path = directory / (label + ".plist");
    bool installed = run({"/bin/launchctl", "print", "system/" + label}).status == 0;
    bool changed = contents.empty() || !fs::exists(path) || readText(path) != contents;
    if (installed && changed)
        checked({"/bin/launchctl", "bootout", "system/" + label});
    if (contents.empty())
        fs::remove(path);
    else {
        if (changed)
            atomicWrite(path, contents);
        if (changed || !installed)
            checked({"/bin/launchctl", "bootstrap", "system", path.string()});
    }
}

static std::string jobPlist(const std::string& label, const fs::path& executable,
                            const std::vector<std::string>& arguments, const std::string& trigger) {
    std::ostringstream out;
    out << "<plist version=\"1.0\"><dict><key>Label</key><string>" << xml(label)
        << "</string><key>ProgramArguments</key><array><string>" << xml(executable.string())
        << "</string>";
    for (const auto& argument : arguments)
        out << "<string>" << xml(argument) << "</string>";
    out << "</array><key>RunAtLoad</key><true/>" << trigger << "</dict></plist>\n";
    return out.str();
}

void MacOSEnvironment::synchronizeJobs(const Schedule& schedule, Timestamp now) {
    require(unsetenv("TZ") == 0, "Cannot select system timezone");
    tzset();
    fs::path directory = "/Library/LaunchDaemons";
    fs::create_directories(directory);
    const std::string prefix = "local.productivityblocker.";
    std::map<std::string, std::string> expected;
    expected[prefix + "renew"] =
        jobPlist(prefix + "renew", executable_, {"renew"},
                 "<key>StartCalendarInterval</key><dict><key>Hour</key><integer>3</integer>"
                 "<key>Minute</key><integer>15</integer></dict>");
    synchronizeWebsiteJobs(schedule, now);
    for (const auto& operation : schedule.operations)
        if (!operation.set && operation.time > now) {
            std::time_t timestamp = operation.time;
            std::tm date{};
            require(localtime_r(&timestamp, &date) != nullptr, "Invalid calendar timestamp");

            std::string label = prefix + operation.id();
            std::ostringstream out;
            out << "<key>StartCalendarInterval</key><dict>";
            for (auto [key, value] :
                 std::vector<std::pair<std::string, int>>{{"Month", date.tm_mon + 1},
                                                          {"Day", date.tm_mday},
                                                          {"Hour", date.tm_hour},
                                                          {"Minute", date.tm_min}})
                out << "<key>" << key << "</key><integer>" << value << "</integer>";
            out << "</dict>";
            expected[label] = jobPlist(label, executable_, {"execute", operation.id()}, out.str());
        }

    for (const auto& [label, contents] : expected)
        synchronizeJob(directory, label, contents);
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto label = entry.path().stem().string();
        if (entry.path().extension() != ".plist" || !label.starts_with(prefix) ||
            expected.contains(label) || label == prefix + "watch" || label == prefix + "refresh")
            continue;
        synchronizeJob(directory, label, {});
    }
}

void MacOSEnvironment::synchronizeWebsiteJobs(const Schedule& schedule, Timestamp now) {
    const fs::path directory = "/Library/LaunchDaemons";
    const std::string prefix = "local.productivityblocker.";
    auto active = currentOperations(schedule, now);
    bool websites = std::any_of(active.begin(), active.end(), [&](const auto& entry) {
        return entry.second.block && schedule.resources.at(entry.first).type == "website";
    });
    for (const std::string command : {"refresh", "watch"}) {
        const auto label = prefix + command;
        std::string contents;
        if (websites) {
            std::string trigger = command == "refresh" ? "<key>StartCalendarInterval</key><dict/>"
                                                       : "<key>KeepAlive</key><true/>";
            contents = jobPlist(label, executable_, {command},
                                trigger + "<key>StandardErrorPath</key><string>" +
                                    xml((state_ / "network.log").string()) + "</string>");
        }
        synchronizeJob(directory, label, contents);
    }
}

}
