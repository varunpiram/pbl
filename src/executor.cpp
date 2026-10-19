#include "config.h"
#include "executor.hpp"
#include "storage.hpp"
#include "macos_environment.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace blocker {
Executor::Executor(fs::path state, MacOSEnvironment& environment)
    : state_(fs::absolute(state)), environment_(environment) {
    require(geteuid() == 0 && state_ == fs::path(PBL_STATE),
            "Executor requires root and the system state directory");
    umask(0077);
    fs::create_directories(state_);
    struct stat st{};
    require(lstat(state_.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
                !(st.st_mode & 0022),
            "State directory must be owned by this user and not group/world writable");
}

Schedule Executor::load() {
    require(unsetenv("TZ") == 0, "Cannot select system timezone");
    tzset();
    return loadSchedule(state_);
}

void Executor::applyCurrent(const Schedule& schedule) {
    auto current = currentOperations(schedule, currentTime());
    std::string errors;
    for (const auto& [id, definition] : schedule.resources) {
        try {
            auto resource = environment_.resource(definition);
            if (current.contains(id) && current.at(id).block)
                Block{}.execute(*resource);
            else
                Unblock{}.execute(*resource);
        } catch (const std::exception& e) {
            errors += id + ": " + e.what() + "\n";
        }
    }
    require(errors.empty(), "Some resource operations failed:\n" + errors);
}

void Executor::commit(Schedule& schedule) {
    require(unsetenv("TZ") == 0, "Cannot select system timezone");
    tzset();
    schedule.operations = resolveSchedule(schedule);
    saveSchedule(state_, schedule);
    environment_.synchronizeJobs(schedule, currentTime());
    applyCurrent(schedule);
}

static std::string registerResource(Schedule& schedule, ResourceDefinition definition) {
    validateId(definition.id);
    definition = validateResource(std::move(definition));
    for (const auto& [id, r] : schedule.resources)
        if (r.type == definition.type &&
            (r.target == definition.target ||
             (r.type == "app" && r.device == definition.device && r.inode == definition.inode)))
            return id;
    require(!schedule.resources.contains(definition.id), "Resource ID already exists");
    auto id = definition.id;
    schedule.resources.emplace(id, std::move(definition));
    return id;
}

void Executor::saveSet(BlockSet set, std::vector<ResourceDefinition> resources,
                       std::vector<WeeklyWindow> windows) {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    validateId(set.id);
    set.resources.clear();
    for (auto& resource : resources)
        set.resources.push_back(registerResource(schedule, std::move(resource)));
    std::sort(set.resources.begin(), set.resources.end());
    set.resources.erase(std::unique(set.resources.begin(), set.resources.end()),
                        set.resources.end());
    for (auto& w : windows) {
        w.set = set.id;
        require(w.day >= 0 && w.day < 7 && w.start >= 0 && w.start < 1440 && w.end >= 0 &&
                    w.end < 1440,
                "Invalid weekly window");
    }
    requireValidSetEdit(schedule, set, windows, currentTime());
    std::erase_if(schedule.weekly, [&](const WeeklyWindow& w) {
        return w.set == set.id;
    });
    schedule.weekly.insert(schedule.weekly.end(), windows.begin(), windows.end());
    schedule.sets[set.id] = std::move(set);
    commit(schedule);
}

void Executor::removeSet(const std::string& set) {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    requireEditable(schedule, set, currentTime());
    require(schedule.sets.erase(set) != 0, "Unknown set");
    std::erase_if(schedule.requests, [&](const Request& r) {
        return r.set == set;
    });
    std::erase_if(schedule.weekly, [&](const WeeklyWindow& w) {
        return w.set == set;
    });
    std::erase_if(schedule.operations, [&](const Operation& op) {
        return op.set && op.target == set;
    });
    commit(schedule);
}

void Executor::submit(const std::vector<Submission>& submissions) {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    schedule = applySubmissions(std::move(schedule), submissions, currentTime());
    commit(schedule);
}

void Executor::execute(const std::string& id) {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    environment_.synchronizeWebsiteJobs(schedule, currentTime());

    for (const auto& [resourceId, op] : currentOperations(schedule, currentTime()))
        if (op.id() == id) {
            auto resource = environment_.resource(schedule.resources.at(resourceId));
            if (op.block)
                Block{}.execute(*resource);
            else
                Unblock{}.execute(*resource);
            return;
        }
}

void Executor::recover() {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    commit(schedule);
}

void Executor::refreshWebsites() {
    FileLock lock(state_ / "schedule.lock");
    auto schedule = load();
    std::erase_if(schedule.resources, [](const auto& entry) {
        return entry.second.type != "website";
    });
    applyCurrent(schedule);
}

Schedule Executor::inspect() {
    FileLock lock(state_ / "schedule.lock");
    return load();
}
}
