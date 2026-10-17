#include "blocker.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <libproc.h>
#include <membership.h>
#include <netdb.h>
#include <set>
#include <spawn.h>
#include <sstream>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace blocker {
namespace {
struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) { require(fd >= 0, std::string("open: ") + strerror(errno)); }
    ~Descriptor() { close(fd); }
};
struct ACL {
    acl_t value;
    explicit ACL(acl_t a) : value(a) { require(a != nullptr, std::string("ACL: ") + strerror(errno)); }
    ~ACL() { acl_free(value); }
};
struct CommandResult { int status; std::string output; };
CommandResult run(const std::vector<std::string>& args) {
    int pipes[2]; require(pipe(pipes) == 0, "Cannot create command output pipe");
    Descriptor reader(pipes[0]), writer(pipes[1]);
    fcntl(reader.fd, F_SETFD, FD_CLOEXEC); fcntl(writer.fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, writer.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, writer.fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, reader.fd);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr); pid_t pid;
    int result = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    require(result == 0, "Cannot start " + args[0] + ": " + strerror(result));
    close(writer.fd); writer.fd = -1;
    std::string output; char buffer[4096]; ssize_t count;
    while ((count = read(reader.fd, buffer, sizeof(buffer))) != 0) {
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "Command output read failed"); output.append(buffer, static_cast<std::size_t>(count));
    }
    int status; while (waitpid(pid, &status, 0) < 0) require(errno == EINTR, "waitpid failed");
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, output};
}
void checked(const std::vector<std::string>& args) {
    auto result = run(args); require(result.status == 0, args[0] + " failed: " + result.output);
}
std::string xml(const std::string& value) {
    std::string out;
    for (char c : value) switch (c) {
        case '&': out += "&amp;"; break; case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break; case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break; default: out += c;
    }
    return out;
}
struct OriginalMetadata {
    std::uint64_t device, inode;
    unsigned uid, gid, mode, flags;
    std::string acl;
};
OriginalMetadata capture(int fd) {
    struct stat st{}; require(fstat(fd, &st) == 0, "Cannot stat app");
    auto existing = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!existing && errno == ENOENT) existing = acl_init(0);
    ACL acl(existing);
    char* text = acl_to_text(acl.value, nullptr); require(text, "Cannot encode ACL");
    std::string saved(text); acl_free(text);
    return {std::uint64_t(st.st_dev), st.st_ino, st.st_uid, st.st_gid,
            unsigned(st.st_mode & 07777), st.st_flags, saved};
}
OriginalMetadata readMetadata(const fs::path& path) {
    OriginalMetadata m{}; std::string tag;
    std::istringstream in(readText(path));
    require(bool(in >> tag >> m.device >> m.inode >> m.uid >> m.gid >> m.mode >> m.flags >> std::quoted(m.acl))
            && tag == "METADATA1", "Invalid restoration record");
    return m;
}
void saveMetadata(const fs::path& path, const OriginalMetadata& m) {
    std::ostringstream out;
    out << "METADATA1 " << m.device << ' ' << m.inode << ' ' << m.uid << ' ' << m.gid << ' '
        << m.mode << ' ' << m.flags << ' ' << std::quoted(m.acl) << '\n';
    atomicWrite(path, out.str());
}
void verifyIdentity(int fd, std::uint64_t device, std::uint64_t inode) {
    struct stat st{};
    require(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && std::uint64_t(st.st_dev) == device && st.st_ino == inode,
            "App installation changed; refusing to modify a replacement");
}
void stopInstances(const ResourceDefinition& definition) {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    require(bytes > 0, "Cannot enumerate running apps");
    std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 256);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t)));
    require(bytes >= 0, "Cannot enumerate running apps");
    auto matches = [&](pid_t pid) {
        char path[PROC_PIDPATHINFO_MAXSIZE]{}; struct proc_bsdinfo info{};
        return pid > 1 && pid != getpid() && proc_pidpath(pid, path, sizeof(path)) > 0 &&
            std::string(path).starts_with(definition.target + "/") &&
            proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_uid == definition.user;
    };
    for (int i = 0; i < bytes / int(sizeof(pid_t)); ++i) if (matches(pids[i])) {
        require(kill(pids[i], SIGTERM) == 0 || errno == ESRCH, "Cannot stop running app");
        for (int n = 0; n < 20 && matches(pids[i]); ++n) usleep(100000);
        if (matches(pids[i])) require(kill(pids[i], SIGKILL) == 0 || errno == ESRCH, "Cannot terminate running app");
    }
}
class SimulatedResource final : public Resource {
public:
    explicit SimulatedResource(fs::path file) : file_(std::move(file)) {}
    void block() override { atomicWrite(file_, "blocked\n"); }
    void unblock() override { fs::remove(file_); }
private:
    fs::path file_;
};
} // namespace
MacOSApp::MacOSApp(ResourceDefinition definition, fs::path state, bool protect)
    : definition_(std::move(definition)), journal_(state / (definition_.id + ".metadata")), protect_(protect) {}
void MacOSApp::block() {
    require(!protect_ || geteuid() == 0, "App protection requires root");
    Descriptor fd(open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    verifyIdentity(fd.fd, definition_.device, definition_.inode);
    if (!fs::exists(journal_)) {
        auto original = capture(fd.fd);
        require(!(original.flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)),
                "App already has restrictive flags; refusing to modify it");
        saveMetadata(journal_, original);
    }
    auto original = readMetadata(journal_);
    verifyIdentity(fd.fd, original.device, original.inode);
    require(fchflags(fd.fd, original.flags) == 0, "Cannot clear our immutable flag");
    ACL acl(acl_from_text(original.acl.c_str()));
    acl_entry_t entry; acl_permset_t permissions; uuid_t user;
    require(mbr_uid_to_uuid(definition_.user, user) == 0, "Cannot resolve user UUID");
    require(acl_create_entry_np(&acl.value, &entry, 0) == 0 && acl_set_tag_type(entry, ACL_EXTENDED_DENY) == 0 &&
            acl_set_qualifier(entry, user) == 0 && acl_get_permset(entry, &permissions) == 0 &&
            acl_clear_perms(permissions) == 0 && acl_add_perm(permissions, ACL_SEARCH) == 0 &&
            acl_set_permset(entry, permissions) == 0, "Cannot construct app ACL");
    require(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED) == 0, "Cannot install app ACL");
    if (protect_) {
        require(fchown(fd.fd, 0, original.gid) == 0, "Cannot protect app ownership");
        require(fchflags(fd.fd, original.flags | UF_IMMUTABLE) == 0, "Cannot protect app flags");
        stopInstances(definition_);
    }
}
void MacOSApp::unblock() {
    if (!fs::exists(journal_)) return;
    Descriptor fd(open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    auto original = readMetadata(journal_);
    verifyIdentity(fd.fd, original.device, original.inode);
    require(fchflags(fd.fd, original.flags & ~UF_IMMUTABLE) == 0, "Cannot remove app protection");
    if (protect_) require(fchown(fd.fd, original.uid, original.gid) == 0, "Cannot restore ownership");
    require(fchmod(fd.fd, original.mode) == 0, "Cannot restore mode");
    ACL acl(acl_from_text(original.acl.c_str()));
    require(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED) == 0, "Cannot restore ACL");
    require(fchflags(fd.fd, original.flags) == 0, "Cannot restore flags");
    // Keep the journal until every restoration step has completed.
    require(unlink(journal_.c_str()) == 0, "Cannot remove completed restoration record");
}
MacOSWebsite::MacOSWebsite(ResourceDefinition definition, fs::path state)
    : definition_(std::move(definition)), state_(std::move(state)) {}
void MacOSWebsite::block() {
    auto status = run({"/sbin/pfctl", "-s", "info"});
    require(status.status == 0 && status.output.find("Status: Enabled") != std::string::npos,
            "PF must already be enabled; see README setup");
    auto rules = run({"/sbin/pfctl", "-s", "rules"});
    require(rules.status == 0 && rules.output.find("anchor \"productivity-blocker/*\"") != std::string::npos,
            "Required PF anchor is not installed; see README setup");
    addrinfo hints{}, *addresses = nullptr; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(definition_.target.c_str(), nullptr, &hints, &addresses);
    require(error == 0, std::string("DNS resolution failed: ") + gai_strerror(error));
    std::set<std::pair<int, std::string>> unique;
    for (auto* a = addresses; a; a = a->ai_next) {
        char host[NI_MAXHOST];
        if ((a->ai_family == AF_INET || a->ai_family == AF_INET6) &&
            getnameinfo(a->ai_addr, a->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0)
            unique.emplace(a->ai_family, host);
    }
    freeaddrinfo(addresses); require(!unique.empty(), "DNS returned no addresses");
    std::ostringstream text;
    for (const auto& [family, address] : unique)
        text << "block drop out quick " << (family == AF_INET ? "inet" : "inet6") << " to " << address << '\n';
    auto file = state_ / (definition_.id + ".pf"); atomicWrite(file, text.str());
    checked({"/sbin/pfctl", "-a", "productivity-blocker/" + definition_.id, "-f", file.string()});
    for (const auto& [family, address] : unique)
        checked({"/sbin/pfctl", "-k", family == AF_INET ? "0.0.0.0/0" : "::/0", "-k", address});
}
void MacOSWebsite::unblock() {
    auto file = state_ / (definition_.id + ".pf");
    if (!fs::exists(file)) return;
    checked({"/sbin/pfctl", "-a", "productivity-blocker/" + definition_.id, "-f", "/dev/null"});
    fs::remove(file);
}
MacOSEnvironment::MacOSEnvironment(fs::path state, fs::path executable, bool simulation)
    : state_(fs::absolute(state)), executable_(fs::canonical(executable)), simulation_(simulation) {
    if (!simulation_) {
        require(geteuid() == 0, "Live execution requires root");
        struct stat st{};
        require(stat(executable_.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022),
                "Install the backend as a root-owned, non-writable executable before live use");
        for (auto parent = executable_.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            require(stat(parent.c_str(), &st) == 0 && st.st_uid == 0 && !(st.st_mode & 0022),
                    "Installed executable ancestors must be root-owned and not group/world writable");
            if (parent == parent.root_path()) break;
        }
        require(state_ == fs::path("/Library/Application Support/ProductivityBlocker"),
                "Live mode uses the fixed system state directory");
    }
}
std::unique_ptr<Resource> MacOSEnvironment::resource(const ResourceDefinition& definition) {
    if (simulation_) return std::make_unique<SimulatedResource>(state_ / (definition.id + ".blocked"));
    if (definition.type == "app") return std::make_unique<MacOSApp>(definition, state_);
    return std::make_unique<MacOSWebsite>(definition, state_);
}
void MacOSEnvironment::synchronizeJobs(const std::vector<Operation>& operations, Timestamp now) {
    fs::path directory = simulation_ ? state_ / "jobs" : fs::path("/Library/LaunchDaemons");
    fs::create_directories(directory);
    const std::string prefix = "local.productivityblocker.";
    std::map<std::string, std::string> expected;
    for (const auto& operation : operations) if (operation.time > now) {
        std::time_t timestamp = operation.time; std::tm date{};
        require(localtime_r(&timestamp, &date) != nullptr, "Invalid calendar timestamp");
        // CLI accepts minute precision. The absolute timestamp is rechecked at execution.
        require(date.tm_sec == 0, "Future scheduled operations must use whole minutes");
        std::string label = prefix + operation.id();
        std::ostringstream out;
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>\n"
            << "<key>Label</key><string>" << xml(label) << "</string>\n<key>ProgramArguments</key><array>";
        std::vector<std::string> arguments{executable_.string()};
        if (simulation_) arguments.push_back("--simulate");
        arguments.insert(arguments.end(), {"--state", state_.string(), "execute", operation.id()});
        for (const auto& argument : arguments)
            out << "<string>" << xml(argument) << "</string>";
        out << "</array>\n<key>StartCalendarInterval</key><dict>";
        for (auto [key, value] : std::vector<std::pair<std::string, int>>{{"Month",date.tm_mon+1},{"Day",date.tm_mday},
                {"Hour",date.tm_hour},{"Minute",date.tm_min}})
            out << "<key>" << key << "</key><integer>" << value << "</integer>";
        out << "</dict>\n<key>RunAtLoad</key><true/>\n</dict></plist>\n";
        expected[label] = out.str();
    }
    // Add missing jobs before removing obsolete ones. Jobs never acquire our lock in launchctl itself.
    for (const auto& [label, contents] : expected) {
        auto path = directory / (label + ".plist");
        bool changed = !fs::exists(path) || readText(path) != contents;
        if (changed) {
            if (!simulation_ && run({"/bin/launchctl", "print", "system/" + label}).status == 0)
                checked({"/bin/launchctl", "bootout", "system/" + label});
            atomicWrite(path, contents);
        }
        if (!simulation_ && run({"/bin/launchctl", "print", "system/" + label}).status != 0)
            checked({"/bin/launchctl", "bootstrap", "system", path.string()});
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto label = entry.path().stem().string();
        if (entry.path().extension() != ".plist" || !label.starts_with(prefix) || expected.contains(label)) continue;
        if (!simulation_ && run({"/bin/launchctl", "print", "system/" + label}).status == 0)
            checked({"/bin/launchctl", "bootout", "system/" + label});
        fs::remove(entry.path());
    }
}
} // namespace blocker
