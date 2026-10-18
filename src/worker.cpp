#include "config.h"
#include "executor.hpp"
#include "macos_environment.hpp"
#include "storage.hpp"
#include <iostream>
#include <mach-o/dyld.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    try {
        bool renew = argc == 2 && std::string(argv[1]) == "renew";
        blocker::require(renew || (argc == 3 && std::string(argv[1]) == "execute"),
                         "Expected execute OPERATION_ID or renew");
        umask(0077);
        std::uint32_t length = 0;
        _NSGetExecutablePath(nullptr, &length);
        std::string executable(length, '\0');
        blocker::require(_NSGetExecutablePath(executable.data(), &length) == 0,
                         "Cannot locate worker executable");
        const blocker::fs::path state = PBL_STATE;
        blocker::MacOSEnvironment environment(state, executable.c_str());
        blocker::Executor executor(state, environment);
        if (renew) executor.recover(); else executor.execute(argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
