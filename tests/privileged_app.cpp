// Run explicitly with sudo; never registered with ordinary ctest.
#include "blocker.hpp"
#include <csignal>
#include <grp.h>
#include <iostream>
#include <libproc.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace blocker;
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--fixture") { for (;;) pause(); }
    fs::path root; pid_t running = -1; std::unique_ptr<MacOSApp> resource;
    try {
        require(geteuid() == 0 && argc == 2, "Usage: sudo ./build/privileged_app_test ORDINARY_USER_UID");
        unsigned uid = std::stoul(argv[1]); auto* account = getpwuid(uid);
        require(uid != 0 && account, "Use a valid non-root UID"); auto gid = account->pw_gid;
        char temporary[] = "/private/tmp/blocker-privileged-XXXXXX";
        require(mkdtemp(temporary), "mkdtemp"); root = temporary; chmod(root.c_str(), 0755);
        auto parent = root / "user-area", state = root / "state", app = parent / "Probe.app";
        fs::create_directories(app / "Contents/MacOS"); fs::create_directory(state); chmod(state.c_str(), 0700);
        require(chown(parent.c_str(), uid, gid) == 0 && chown(app.c_str(), uid, gid) == 0, "fixture ownership");
        auto executable = app / "Contents/MacOS/probe"; fs::copy_file(fs::canonical(argv[0]), executable);
        chmod(executable.c_str(), 0755); chmod(app.c_str(), 0755);
        struct stat before{}; stat(app.c_str(), &before);
        ResourceDefinition definition{"probe", "app", app.string(), uid, std::uint64_t(before.st_dev), before.st_ino};
        resource = std::make_unique<MacOSApp>(definition, state);
        running = fork(); require(running >= 0, "fork");
        if (running == 0) {
            if (setgroups(0, nullptr) || setgid(gid) || setuid(uid)) _exit(90);
            execl(executable.c_str(), executable.c_str(), "--fixture", nullptr); _exit(91);
        }
        bool started = false;
        for (int i = 0; i < 50; ++i) {
            char path[PROC_PIDPATHINFO_MAXSIZE]{};
            if (proc_pidpath(running, path, sizeof(path)) > 0 && fs::path(path) == executable) { started = true; break; }
            usleep(100000);
        }
        require(started, "fixture process did not start");
        resource->block();
        int status; bool stopped = false;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(running, &status, WNOHANG) == running) { stopped = true; running = -1; break; }
            usleep(100000);
        }
        require(stopped, "running app was not stopped");
        struct stat restricted{}; stat(app.c_str(), &restricted);
        require(restricted.st_uid == 0 && (restricted.st_flags & UF_IMMUTABLE), "root ownership and immutable flag not installed");
        pid_t child = fork(); require(child >= 0, "fork");
        if (child == 0) {
            if (setgroups(0, nullptr) || setgid(gid) || setuid(uid)) _exit(90);
            if (access(executable.c_str(), X_OK) == 0 || chflags(app.c_str(), 0) == 0 || chmod(app.c_str(), 0777) == 0 ||
                rename(app.c_str(), (parent / "Moved.app").c_str()) == 0) _exit(92);
            _exit(0);
        }
        waitpid(child, &status, 0); require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "ordinary user can bypass protected fixture");
        resource->block(); resource->unblock(); resource->unblock();
        struct stat restored{}; stat(app.c_str(), &restored);
        require(restored.st_uid == before.st_uid && restored.st_gid == before.st_gid && restored.st_mode == before.st_mode &&
                restored.st_flags == before.st_flags, "metadata restoration mismatch");
        fs::remove_all(root);
        std::cout << "PASS root protection, user tamper attempts, process termination, retries, restoration; fixture removed\n";
        return 0;
    } catch (const std::exception& e) {
        if (running > 0) { kill(running, SIGKILL); waitpid(running, nullptr, 0); }
        bool restored = true;
        if (resource) { try { resource->unblock(); } catch (...) { restored = false; } }
        if (!root.empty() && restored) { std::error_code ignored; fs::remove_all(root, ignored); }
        std::cerr << e.what() << '\n';
        if (!restored) std::cerr << "Restoration failed; retained fixture and journal at " << root << '\n';
        return 1;
    }
}
