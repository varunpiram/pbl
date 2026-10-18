#include "macos_app.hpp"
#include "storage.hpp"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <libproc.h>
#include <membership.h>
#include <sstream>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blocker {
namespace {
struct Descriptor {
    int fd;

    explicit Descriptor(int value) : fd(value) {
        require(fd >= 0, std::string("open: ") + strerror(errno));
    }

    ~Descriptor() {
        close(fd);
    }
};

struct ACL {
    acl_t value;

    explicit ACL(acl_t a) : value(a) {
        require(a != nullptr, std::string("ACL: ") + strerror(errno));
    }

    ~ACL() {
        acl_free(value);
    }
};

struct OriginalMetadata {
    std::uint64_t device, inode;
    unsigned uid, gid, mode, flags;
    std::string acl;
};

OriginalMetadata capture(int fd) {
    struct stat st{};
    require(fstat(fd, &st) == 0, "Cannot stat app");
    auto existing = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!existing && errno == ENOENT)
        existing = acl_init(0);
    ACL acl(existing);
    char* text = acl_to_text(acl.value, nullptr);
    require(text, "Cannot encode ACL");
    std::string saved(text);
    acl_free(text);
    return {std::uint64_t(st.st_dev),     st.st_ino,   st.st_uid, st.st_gid,
            unsigned(st.st_mode & 07777), st.st_flags, saved};
}

OriginalMetadata readMetadata(const fs::path& path) {
    OriginalMetadata m{};
    std::string tag;
    std::istringstream in(readText(path));
    require(bool(in >> tag >> m.device >> m.inode >> m.uid >> m.gid >> m.mode >> m.flags >>
                 std::quoted(m.acl)) &&
                tag == "METADATA1",
            "Invalid restoration record");
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
    require(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && std::uint64_t(st.st_dev) == device &&
                st.st_ino == inode,
            "App installation changed; refusing to modify a replacement");
}

void stopInstances(const ResourceDefinition& definition) {
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    require(bytes > 0, "Cannot enumerate running apps");
    std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 256);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), int(pids.size() * sizeof(pid_t)));
    require(bytes >= 0, "Cannot enumerate running apps");
    auto matches = [&](pid_t pid) {
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        struct proc_bsdinfo info{};
        return pid > 1 && pid != getpid() && proc_pidpath(pid, path, sizeof(path)) > 0 &&
               std::string(path).starts_with(definition.target + "/") &&
               proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
               info.pbi_uid == definition.user;
    };
    for (int i = 0; i < bytes / int(sizeof(pid_t)); ++i)
        if (matches(pids[i])) {
            require(kill(pids[i], SIGTERM) == 0 || errno == ESRCH, "Cannot stop running app");
            for (int n = 0; n < 20 && matches(pids[i]); ++n)
                usleep(100000);
            if (matches(pids[i]))
                require(kill(pids[i], SIGKILL) == 0 || errno == ESRCH,
                        "Cannot terminate running app");
        }
}
}

MacOSApp::MacOSApp(ResourceDefinition definition, fs::path state)
    : definition_(std::move(definition)), journal_(state / (definition_.id + ".metadata")) {
}

void MacOSApp::block() {
    require(geteuid() == 0, "App protection requires root");
    Descriptor fd(
        open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
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
    acl_entry_t entry;
    acl_permset_t permissions;
    uuid_t user;
    require(mbr_uid_to_uuid(definition_.user, user) == 0, "Cannot resolve user UUID");
    require(acl_create_entry_np(&acl.value, &entry, 0) == 0 &&
                acl_set_tag_type(entry, ACL_EXTENDED_DENY) == 0 &&
                acl_set_qualifier(entry, user) == 0 && acl_get_permset(entry, &permissions) == 0 &&
                acl_clear_perms(permissions) == 0 && acl_add_perm(permissions, ACL_SEARCH) == 0 &&
                acl_set_permset(entry, permissions) == 0,
            "Cannot construct app ACL");
    require(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED) == 0, "Cannot install app ACL");
    require(fchown(fd.fd, 0, original.gid) == 0, "Cannot protect app ownership");
    require(fchflags(fd.fd, original.flags | UF_IMMUTABLE) == 0, "Cannot protect app flags");
    stopInstances(definition_);
}

void MacOSApp::unblock() {
    if (!fs::exists(journal_))
        return;
    Descriptor fd(
        open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    auto original = readMetadata(journal_);
    verifyIdentity(fd.fd, original.device, original.inode);
    require(fchflags(fd.fd, original.flags & ~UF_IMMUTABLE) == 0, "Cannot remove app protection");
    require(fchown(fd.fd, original.uid, original.gid) == 0, "Cannot restore ownership");
    require(fchmod(fd.fd, original.mode) == 0, "Cannot restore mode");
    ACL acl(acl_from_text(original.acl.c_str()));
    require(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED) == 0, "Cannot restore ACL");
    require(fchflags(fd.fd, original.flags) == 0, "Cannot restore flags");

    require(unlink(journal_.c_str()) == 0, "Cannot remove completed restoration record");
}
}
