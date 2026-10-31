#include "macos_app.hpp"
#include "storage.hpp"
#include <Security/Security.h>
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
template <class T> struct CF {
    T value = nullptr;

    ~CF() {
        if (value)
            CFRelease(value);
    }
};

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
    timespec born{};
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
            unsigned(st.st_mode & 07777), st.st_flags, saved,     st.st_birthtimespec};
}

OriginalMetadata readMetadata(const fs::path& path) {
    OriginalMetadata m{};
    std::string tag;
    std::istringstream in(readText(path));
    require(bool(in >> tag >> m.device >> m.inode >> m.uid >> m.gid >> m.mode >> m.flags >>
                 std::quoted(m.acl)) &&
                (tag == "METADATA1" || tag == "METADATA2"),
            "Invalid restoration record");
    if (tag == "METADATA2")
        require(bool(in >> m.born.tv_sec >> m.born.tv_nsec), "Invalid installation birth time");
    return m;
}

void saveMetadata(const fs::path& path, const OriginalMetadata& m) {
    std::ostringstream out;
    out << "METADATA2 " << m.device << ' ' << m.inode << ' ' << m.uid << ' ' << m.gid << ' '
        << m.mode << ' ' << m.flags << ' ' << std::quoted(m.acl) << ' ' << m.born.tv_sec << ' '
        << m.born.tv_nsec << '\n';
    atomicWrite(path, out.str());
}

bool matches(const struct stat& st, const OriginalMetadata& original) {
    return S_ISDIR(st.st_mode) && std::uint64_t(st.st_dev) == original.device &&
           st.st_ino == original.inode &&
           (!original.born.tv_sec || (st.st_birthtimespec.tv_sec == original.born.tv_sec &&
                                      st.st_birthtimespec.tv_nsec == original.born.tv_nsec));
}

void verifyIdentity(int fd, const OriginalMetadata& original) {
    struct stat st{};
    require(fstat(fd, &st) == 0 && matches(st, original),
            "App installation changed; refusing to modify a replacement");
}

bool sameMetadata(const OriginalMetadata& a, const OriginalMetadata& b) {
    return a.uid == b.uid && a.gid == b.gid && a.mode == b.mode && a.flags == b.flags &&
           a.acl == b.acl;
}

void requireAppChange(int result, const char* message) {
    if (result == 0)
        return;
    const int error = errno;
    throw std::runtime_error(
        std::string(message) + ": " + strerror(error) +
        (error == EPERM ? "; macOS may require Full Disk Access approval "
                          "for pbl Blocking Service in System Settings > Privacy & Security"
                        : ""));
}

std::vector<fs::path> journals(const fs::path& base) {
    std::vector<fs::path> result;
    const auto prefix = base.stem().string() + ".";
    for (const auto& entry : fs::directory_iterator(base.parent_path()))
        if (entry.path().filename().string().starts_with(prefix) &&
            entry.path().extension() == ".metadata")
            result.push_back(entry.path());
    return result;
}

fs::path installationJournal(int fd, const fs::path& base) {
    struct stat st{};
    require(fstat(fd, &st) == 0, "Cannot stat app");
    if (fs::exists(base) && matches(st, readMetadata(base)))
        return base;
    return base.parent_path() / (base.stem().string() + "." + std::to_string(st.st_ino) + "." +
                                 std::to_string(st.st_birthtimespec.tv_sec) + "." +
                                 std::to_string(st.st_birthtimespec.tv_nsec) + ".metadata");
}

void checkPending(const fs::path& base, const fs::path& current = {}) {
    for (const auto& path : journals(base))
        require(path == current, "Previous installation needs restoration: " + path.string());
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

std::string appRequirement(const fs::path& path, const std::string& expected) {
    CF<CFURLRef> url{CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()), path.string().size(), true)};
    CF<SecStaticCodeRef> code;
    require(url.value && SecStaticCodeCreateWithPath(url.value, 0, &code.value) == errSecSuccess,
            "Cannot inspect app signature");
    CF<SecRequirementRef> requirement;
    if (!expected.empty()) {
        CF<CFStringRef> text{
            CFStringCreateWithCString(nullptr, expected.c_str(), kCFStringEncodingUTF8)};
        require(text.value && SecRequirementCreateWithString(text.value, 0, &requirement.value) ==
                                  errSecSuccess,
                "Invalid saved app signing requirement");
    } else {
        CF<CFDictionaryRef> info;
        auto status =
            SecCodeCopySigningInformation(code.value, kSecCSSigningInformation, &info.value);
        if (status == errSecCSUnsigned)
            return {};
        require(status == errSecSuccess, "Cannot read app signing identity");
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(info.value, kSecCodeInfoFlags));
        unsigned flags = 0;
        require(number && CFNumberGetValue(number, kCFNumberIntType, &flags),
                "Invalid signature flags");
        if (flags & kSecCodeSignatureAdhoc)
            return {};
        require(SecCodeCopyDesignatedRequirement(code.value, 0, &requirement.value) ==
                    errSecSuccess,
                "Cannot capture app signing identity");
    }
    require(SecStaticCodeCheckValidity(code.value, kSecCSCheckAllArchitectures,
                                       requirement.value) == errSecSuccess,
            "App signing identity changed or signature is invalid");
    CF<CFStringRef> text;
    require(SecRequirementCopyString(requirement.value, 0, &text.value) == errSecSuccess,
            "Cannot encode app signing identity");
    CF<CFDataRef> data{
        CFStringCreateExternalRepresentation(nullptr, text.value, kCFStringEncodingUTF8, 0)};
    require(data.value, "Cannot encode app signing identity");
    return {reinterpret_cast<const char*>(CFDataGetBytePtr(data.value)),
            static_cast<std::size_t>(CFDataGetLength(data.value))};
}

MacOSApp::MacOSApp(ResourceDefinition definition, fs::path state)
    : definition_(std::move(definition)), journal_(state / (definition_.id + ".metadata")) {
}

void MacOSApp::block() {
    require(geteuid() == 0, "App protection requires root");
    Descriptor fd(
        open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!definition_.requirement.empty())
        appRequirement(definition_.target, definition_.requirement);
    auto journal = installationJournal(fd.fd, journal_);
    if (!fs::exists(journal)) {
        auto original = capture(fd.fd);
        require(!(original.flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)),
                "App already has restrictive flags; refusing to modify it");
        saveMetadata(journal, original);
    }
    auto original = readMetadata(journal);
    verifyIdentity(fd.fd, original);
    struct stat path{};
    require(lstat(definition_.target.c_str(), &path) == 0 && matches(path, original),
            "App moved during blocking; retry");
    requireAppChange(fchflags(fd.fd, original.flags), "Cannot clear our immutable flag");
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
    requireAppChange(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED), "Cannot install app ACL");
    requireAppChange(fchown(fd.fd, 0, original.gid), "Cannot protect app ownership");
    requireAppChange(fchflags(fd.fd, original.flags | UF_IMMUTABLE), "Cannot protect app flags");
    stopInstances(definition_);
    require(lstat(definition_.target.c_str(), &path) == 0 && matches(path, original),
            "App moved during blocking; restoration record retained");
    checkPending(journal_, journal);
}

void MacOSApp::unblock() {
    if (journals(journal_).empty())
        return;
    require(geteuid() == 0, "App restoration requires root");
    Descriptor fd(
        open(definition_.target.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    auto journal = installationJournal(fd.fd, journal_);
    require(fs::exists(journal),
            "Previous installation needs restoration; replacement left untouched");
    auto original = readMetadata(journal);
    verifyIdentity(fd.fd, original);
    // A failed block may have saved a journal before macOS denied its first mutation.
    if (!sameMetadata(capture(fd.fd), original)) {
        requireAppChange(fchflags(fd.fd, original.flags & ~UF_IMMUTABLE),
                         "Cannot remove app protection");
        requireAppChange(fchown(fd.fd, original.uid, original.gid), "Cannot restore ownership");
        requireAppChange(fchmod(fd.fd, original.mode), "Cannot restore mode");
        ACL acl(acl_from_text(original.acl.c_str()));
        requireAppChange(acl_set_fd_np(fd.fd, acl.value, ACL_TYPE_EXTENDED), "Cannot restore ACL");
        requireAppChange(fchflags(fd.fd, original.flags), "Cannot restore flags");
    }

    require(unlink(journal.c_str()) == 0, "Cannot remove completed restoration record");
    checkPending(journal_);
}
}
