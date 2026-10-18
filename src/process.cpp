#include "process.hpp"
#include "storage.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace blocker {
struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) { require(fd >= 0, std::string("open: ") + strerror(errno)); }
    ~Descriptor() { close(fd); }
};
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
}
