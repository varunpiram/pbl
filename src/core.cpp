#include "blocker.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blocker {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
Timestamp currentTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
FileLock::FileLock(const fs::path& path) {
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(fd_ >= 0, "Cannot open schedule lock: " + std::string(strerror(errno)));
    int result;
    do { result = flock(fd_, LOCK_EX); } while (result < 0 && errno == EINTR);
    if (result < 0) { close(fd_); fd_ = -1; throw std::runtime_error("Cannot lock schedule"); }
}
FileLock::~FileLock() { if (fd_ >= 0) close(fd_); }
std::string readText(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot read " + path.string());
    std::ostringstream result; result << input.rdbuf();
    require(!input.bad(), "Read failed: " + path.string());
    return result.str();
}
void atomicWrite(const fs::path& path, const std::string& text) {
    std::string name = path.string() + ".XXXXXX";
    int fd = mkstemp(name.data());
    require(fd >= 0, "Cannot create temporary file for " + path.string());
    try {
        std::size_t offset = 0;
        while (offset < text.size()) {
            auto n = write(fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "Write failed"); offset += static_cast<std::size_t>(n);
        }
        require(fsync(fd) == 0, "File flush failed");
        require(close(fd) == 0, "File close failed"); fd = -1;
        require(rename(name.c_str(), path.c_str()) == 0, "Atomic replacement failed");
        int directory = open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
        require(directory >= 0, "Cannot open parent directory");
        int result = fsync(directory); close(directory);
        require(result == 0, "Directory flush failed");
    } catch (...) { if (fd >= 0) close(fd); unlink(name.c_str()); throw; }
}
static void validateId(const std::string& id) {
    require(!id.empty() && id.size() <= 48 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    }), "IDs must contain 1–48 ASCII letters, digits, '-' or '_'");
}
std::string Operation::id() const {
    return resource + "." + std::to_string(time) + (block ? ".block" : ".unblock");
}
std::vector<Operation> resolveSchedule(const Schedule& schedule) {
    auto requests = schedule.requests;
    std::sort(requests.begin(), requests.end(), [](const Request& a, const Request& b) {
        return std::tie(a.time, a.id) < std::tie(b.time, b.id);
    });
    std::map<std::string, bool> active;
    std::set<std::string> previous;
    std::vector<Operation> result;
    for (std::size_t i = 0; i < requests.size();) {
        Timestamp time = requests[i].time;
        do { active[requests[i].set] = requests[i].block; ++i; }
        while (i < requests.size() && requests[i].time == time);
        std::set<std::string> next;
        for (const auto& [id, blocked] : active) if (blocked) {
            const auto& members = schedule.sets.at(id).resources;
            next.insert(members.begin(), members.end());
        }
        for (const auto& id : previous) if (!next.contains(id)) result.push_back({id, false, time});
        for (const auto& id : next) if (!previous.contains(id)) result.push_back({id, true, time});
        previous = std::move(next);
    }
    return result;
}
std::map<std::string, Operation> currentOperations(const Schedule& schedule, Timestamp now) {
    std::map<std::string, Operation> result;
    for (const auto& op : schedule.operations) if (op.time <= now) result[op.resource] = op;
    return result;
}
Executor::Executor(fs::path state, ExecutionEnvironment& environment, bool simulation,
                   std::function<Timestamp()> clock)
    : state_(fs::absolute(state)), environment_(environment), simulation_(simulation), clock_(std::move(clock)) {
    fs::create_directories(state_);
    struct stat st{};
    require(lstat(state_.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid()
            && !(st.st_mode & 0022), "State directory must be owned by this user and not group/world writable");
}
Schedule Executor::load() {
    Schedule schedule;
    if (!fs::exists(state_ / "schedule.txt")) return schedule;
    std::istringstream input(readText(state_ / "schedule.txt"));
    std::string magic; int version, simulation;
    require(bool(input >> magic >> version >> simulation >> schedule.nextRequest)
            && magic == "BLOCKER" && version == 1 && simulation == int(simulation_) && schedule.nextRequest > 0,
            "Invalid schedule header or wrong simulation mode");
    std::string tag;
    while (input >> tag) {
        if (tag == "resource") {
            ResourceDefinition r;
            require(bool(input >> std::quoted(r.id) >> r.type >> std::quoted(r.target) >> r.user >> r.device >> r.inode), "Malformed resource");
            validateId(r.id);
            require(r.type == "app" || r.type == "website", "Unknown resource type");
            require(schedule.resources.emplace(r.id, r).second, "Duplicate resource");
        } else if (tag == "set") {
            BlockSet set; std::size_t count;
            require(bool(input >> std::quoted(set.id) >> count) && count <= 10000, "Malformed set"); validateId(set.id);
            while (count--) { std::string id; require(bool(input >> std::quoted(id)), "Malformed member"); set.resources.push_back(id); }
            require(schedule.sets.emplace(set.id, set).second, "Duplicate set");
        } else if (tag == "request") {
            Request r{}; require(bool(input >> r.id >> std::quoted(r.set) >> r.block >> r.time), "Malformed request");
            require(r.id > 0 && r.id < schedule.nextRequest && r.time >= 0, "Invalid request");
            schedule.requests.push_back(r);
        } else if (tag == "operation") {
            Operation op; require(bool(input >> std::quoted(op.resource) >> op.block >> op.time), "Malformed operation");
            require(op.time >= 0, "Invalid operation time"); schedule.operations.push_back(op);
        } else throw std::runtime_error("Unknown schedule record: " + tag);
    }
    require(input.eof(), "Cannot parse schedule");
    std::set<std::uint64_t> ids;
    for (const auto& r : schedule.requests)
        require(schedule.sets.contains(r.set) && ids.insert(r.id).second, "Dangling or duplicate request");
    for (const auto& [id, set] : schedule.sets) for (const auto& resource : set.resources)
        require(schedule.resources.contains(resource), "Unknown set resource");
    Timestamp previous = -1;
    for (const auto& op : schedule.operations) {
        require(schedule.resources.contains(op.resource) && op.time >= previous, "Invalid operation sequence"); previous = op.time;
    }
    return schedule;
}
void Executor::save(const Schedule& schedule) {
    std::ostringstream out; out << "BLOCKER 1 " << simulation_ << ' ' << schedule.nextRequest << '\n';
    for (const auto& [id, r] : schedule.resources)
        out << "resource " << std::quoted(id) << ' ' << r.type << ' ' << std::quoted(r.target)
            << ' ' << r.user << ' ' << r.device << ' ' << r.inode << '\n';
    for (const auto& [id, set] : schedule.sets) {
        out << "set " << std::quoted(id) << ' ' << set.resources.size();
        for (const auto& resource : set.resources) out << ' ' << std::quoted(resource);
        out << '\n';
    }
    for (const auto& r : schedule.requests)
        out << "request " << r.id << ' ' << std::quoted(r.set) << ' ' << r.block << ' ' << r.time << '\n';
    for (const auto& op : schedule.operations)
        out << "operation " << std::quoted(op.resource) << ' ' << op.block << ' ' << op.time << '\n';
    atomicWrite(state_ / "schedule.txt", out.str());
}
void Executor::applyCurrent(const Schedule& schedule) {
    auto current = currentOperations(schedule, clock_());
    std::string errors;
    for (const auto& [id, definition] : schedule.resources) {
        try {
            auto resource = environment_.resource(definition);
            if (current.contains(id) && current.at(id).block) Block{}.execute(*resource);
            else Unblock{}.execute(*resource);
        } catch (const std::exception& e) { errors += id + ": " + e.what() + "\n"; }
    }
    require(errors.empty(), "Some resource operations failed:\n" + errors);
}
void Executor::commit(Schedule& schedule) {
    schedule.operations = resolveSchedule(schedule);
    save(schedule); // Intent and generated operations share one atomic replacement.
    environment_.synchronizeJobs(schedule.operations, clock_());
    applyCurrent(schedule);
}
void Executor::addResource(ResourceDefinition definition) {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load(); validateId(definition.id);
    require(definition.type == "app" || definition.type == "website", "Unknown resource type");
    require(!schedule.resources.contains(definition.id), "Resource ID already exists");
    for (const auto& [id, r] : schedule.resources)
        require(!(r.type == definition.type && (r.target == definition.target || (r.type == "app" && r.inode != 0 && r.inode == definition.inode && r.device == definition.device))),
                "Resource already exists as " + id + "; reuse that ID across sets");
    schedule.resources.emplace(definition.id, definition); commit(schedule);
}
void Executor::saveSet(BlockSet set) {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load(); validateId(set.id);
    for (const auto& id : set.resources) require(schedule.resources.contains(id), "Unknown resource: " + id);
    std::sort(set.resources.begin(), set.resources.end());
    set.resources.erase(std::unique(set.resources.begin(), set.resources.end()), set.resources.end());
    schedule.sets[set.id] = std::move(set); commit(schedule);
}
std::uint64_t Executor::submit(const std::string& set, const Action& action, ExecutionPolicy policy) {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load();
    require(schedule.sets.contains(set), "Unknown set: " + set);
    Timestamp time = std::holds_alternative<ExecuteAt>(policy) ? std::get<ExecuteAt>(policy).timestamp : clock_();
    require(time >= 0, "Invalid execution time");
    require(time <= clock_() || time % 60 == 0, "Future scheduled actions must use whole minutes");
    require(schedule.nextRequest < std::numeric_limits<std::uint64_t>::max(), "Request ID space exhausted");
    auto id = schedule.nextRequest++; schedule.requests.push_back({id, set, action.blocks(), time});
    commit(schedule); return id;
}
void Executor::cancel(std::uint64_t id) {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load();
    auto count = std::erase_if(schedule.requests, [id](const Request& r) { return r.id == id; });
    require(count != 0, "Unknown request ID"); commit(schedule);
}
void Executor::execute(const std::string& id) {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load();
    // Only the latest due operation for this resource may run. No overlap resolver.
    for (const auto& [resourceId, op] : currentOperations(schedule, clock_())) if (op.id() == id) {
        auto resource = environment_.resource(schedule.resources.at(resourceId));
        if (op.block) Block{}.execute(*resource); else Unblock{}.execute(*resource);
        return;
    }
}
void Executor::recover() {
    FileLock lock(state_ / "schedule.lock"); auto schedule = load(); commit(schedule);
}
Schedule Executor::inspect() { FileLock lock(state_ / "schedule.lock"); return load(); }
} // namespace blocker
