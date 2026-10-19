#include "config.h"
#include "executor.hpp"
#include "macos_environment.hpp"
#include "storage.hpp"
#include "network_watch.hpp"
#include <iostream>
#include <mach-o/dyld.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    try {
        std::string command = argc > 1 ? argv[1] : "";
        bool renew = argc == 2 && command == "renew";
        bool refresh = argc == 2 && command == "refresh";
        bool watch = argc == 2 && command == "watch";
        blocker::require(renew || refresh || watch ||
                             (argc == 3 && std::string(argv[1]) == "execute"),
                         "Expected execute OPERATION_ID, renew, refresh or watch");
        umask(0077);
        std::uint32_t length = 0;
        _NSGetExecutablePath(nullptr, &length);
        std::string executable(length, '\0');
        blocker::require(_NSGetExecutablePath(executable.data(), &length) == 0,
                         "Cannot locate worker executable");
        const blocker::fs::path state = PBL_STATE;
        blocker::MacOSEnvironment environment(state, executable.c_str());
        blocker::Executor executor(state, environment);
        if (watch)
            blocker::watchNetwork();
        else if (refresh)
            executor.refreshWebsites();
        else if (renew)
            executor.recover();
        else
            executor.execute(argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
