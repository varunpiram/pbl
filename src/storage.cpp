#include "storage.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace blocker {
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}

Timestamp currentTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

FileLock::FileLock(const fs::path& path) {
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(fd_ >= 0, "Cannot open schedule lock: " + std::string(strerror(errno)));
    int result;
    do {
        result = flock(fd_, LOCK_EX);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot lock schedule");
    }
}

FileLock::~FileLock() {
    if (fd_ >= 0)
        close(fd_);
}

std::string readText(const fs::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot read " + path.string());
    std::ostringstream result;
    result << input.rdbuf();
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
            if (n < 0 && errno == EINTR)
                continue;
            require(n > 0, "Write failed");
            offset += static_cast<std::size_t>(n);
        }
        require(fsync(fd) == 0, "File flush failed");
        require(close(fd) == 0, "File close failed");
        fd = -1;
        require(rename(name.c_str(), path.c_str()) == 0, "Atomic replacement failed");
        int directory = open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
        require(directory >= 0, "Cannot open parent directory");
        int result = fsync(directory);
        close(directory);
        require(result == 0, "Directory flush failed");
    } catch (...) {
        if (fd >= 0)
            close(fd);
        unlink(name.c_str());
        throw;
    }
}

void validateId(const std::string& id) {
    require(!id.empty() && id.size() <= 48 &&
                std::all_of(id.begin(), id.end(),
                            [](unsigned char c) {
                                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9') || c == '-' || c == '_';
                            }),
            "IDs must contain 1–48 ASCII letters, digits, '-' or '_'");
}

Schedule loadSchedule(const fs::path& state) {
    Schedule schedule;
    if (!fs::exists(state / "schedule.txt"))
        return schedule;
    std::istringstream input(readText(state / "schedule.txt"));
    std::string magic;
    int version, legacyMode;
    require(bool(input >> magic >> version >> legacyMode >> schedule.nextRequest) &&
                magic == "BLOCKER" && version == 1 && legacyMode == 0 && schedule.nextRequest > 0,
            "Invalid live schedule header");
    std::string tag;
    while (input >> tag) {
        if (tag == "resource") {
            ResourceDefinition r;
            require(bool(input >> std::quoted(r.id) >> r.type >> std::quoted(r.target) >> r.user >>
                         r.device >> r.inode),
                    "Malformed resource");
            validateId(r.id);
            require(r.type == "app" || r.type == "website", "Unknown resource type");
            require(schedule.resources.emplace(r.id, r).second, "Duplicate resource");
        } else if (tag == "set") {
            BlockSet set;
            std::size_t count;
            require(bool(input >> std::quoted(set.id) >> count) && count <= 10000, "Malformed set");
            validateId(set.id);
            while (count--) {
                std::string id;
                require(bool(input >> std::quoted(id)), "Malformed member");
                set.resources.push_back(id);
            }
            require(schedule.sets.emplace(set.id, set).second, "Duplicate set");
        } else if (tag == "request") {
            Request r{};
            require(bool(input >> r.id >> std::quoted(r.set) >> r.block >> r.time),
                    "Malformed request");
            require(r.id > 0 && r.id < schedule.nextRequest && r.time >= 0, "Invalid request");
            schedule.requests.push_back(r);
        } else if (tag == "weekly") {
            WeeklyWindow w;
            require(bool(input >> std::quoted(w.set) >> w.day >> w.start >> w.end),
                    "Malformed weekly window");
            require(w.day >= 0 && w.day < 7 && w.start >= 0 && w.start < 1440 && w.end >= 0 &&
                        w.end < 1440,
                    "Invalid weekly window");
            schedule.weekly.push_back(w);
        } else if (tag == "operation" || tag == "setoperation") {
            Operation op;
            op.set = tag == "setoperation";
            require(bool(input >> std::quoted(op.target) >> op.block >> op.time),
                    "Malformed operation");
            require(op.time >= 0, "Invalid operation time");
            schedule.operations.push_back(op);
        } else
            throw std::runtime_error("Unknown schedule record: " + tag);
    }
    require(input.eof(), "Cannot parse schedule");
    for (const auto& w : schedule.weekly)
        require(schedule.sets.contains(w.set), "Unknown weekly set");
    std::set<std::uint64_t> ids;
    for (const auto& r : schedule.requests)
        require(schedule.sets.contains(r.set) && ids.insert(r.id).second,
                "Dangling or duplicate request");
    for (const auto& [id, set] : schedule.sets)
        for (const auto& resource : set.resources)
            require(schedule.resources.contains(resource), "Unknown set resource");
    Timestamp previous = -1;
    for (const auto& op : schedule.operations) {
        require(
            (op.set ? schedule.sets.contains(op.target) : schedule.resources.contains(op.target)) &&
                op.time >= previous,
            "Invalid operation sequence");
        previous = op.time;
    }
    return schedule;
}

void saveSchedule(const fs::path& state, const Schedule& schedule) {
    std::ostringstream out;
    out << "BLOCKER 1 0 " << schedule.nextRequest << '\n';
    for (const auto& [id, r] : schedule.resources)
        out << "resource " << std::quoted(id) << ' ' << r.type << ' ' << std::quoted(r.target)
            << ' ' << r.user << ' ' << r.device << ' ' << r.inode << '\n';
    for (const auto& [id, set] : schedule.sets) {
        out << "set " << std::quoted(id) << ' ' << set.resources.size();
        for (const auto& resource : set.resources)
            out << ' ' << std::quoted(resource);
        out << '\n';
    }
    for (const auto& r : schedule.requests)
        out << "request " << r.id << ' ' << std::quoted(r.set) << ' ' << r.block << ' ' << r.time
            << '\n';
    for (const auto& w : schedule.weekly)
        out << "weekly " << std::quoted(w.set) << ' ' << w.day << ' ' << w.start << ' ' << w.end
            << '\n';
    for (const auto& op : schedule.operations)
        out << (op.set ? "setoperation " : "operation ") << std::quoted(op.target) << ' '
            << op.block << ' ' << op.time << '\n';
    atomicWrite(state / "schedule.txt", out.str());
}
}
