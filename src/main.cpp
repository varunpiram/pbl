#include "blocker.hpp"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mach-o/dyld.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace blocker;
namespace {
constexpr const char* usage = R"(Usage: blockerctl [--simulate] [--state DIRECTORY] COMMAND
  app ID /absolute/path/App.app UID     Register a known app installation
  website ID example.com               Register an exact hostname
  set ID RESOURCE_ID...                Create/replace a set's membership
  block SET [YYYY-MM-DDTHH:MMZ]         Submit now or at a UTC minute
  unblock SET [YYYY-MM-DDTHH:MMZ]       Submit now or at a UTC minute
  cancel REQUEST_ID                    Remove an original request
  status                               Recover and print the saved schedule
  recover                              Repair jobs and current restrictions
  execute OPERATION_ID                 Internal launchd entry; skips overlap logic

Simulation only writes files in its state directory; it never installs jobs or
changes app permissions/PF. Live mode requires the root-owned installed binary.
)";
std::uint64_t number(const std::string& value) {
    std::uint64_t result; auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), result);
    require(error == std::errc{} && end == value.data()+value.size(), "Invalid integer: " + value); return result;
}
Timestamp parseTimestamp(const std::string& text) {
    std::tm date{}; std::istringstream in(text);
    in >> std::get_time(&date, "%Y-%m-%dT%H:%MZ");
    require(!in.fail() && in.peek() == EOF, "Use UTC YYYY-MM-DDTHH:MMZ");
    auto timestamp = timegm(&date); char normalized[32];
    strftime(normalized, sizeof(normalized), "%Y-%m-%dT%H:%MZ", &date);
    require(timestamp >= 0 && normalized == text, "Invalid calendar date"); return timestamp;
}
fs::path executablePath() {
    std::uint32_t count = 0; _NSGetExecutablePath(nullptr, &count);
    std::string buffer(count, '\0'); require(_NSGetExecutablePath(buffer.data(), &count) == 0, "Cannot locate executable");
    return fs::canonical(buffer.c_str());
}
void show(const Schedule& schedule) {
    std::cout << "Resources: " << schedule.resources.size() << ", sets: " << schedule.sets.size() << '\n';
    for (const auto& r : schedule.requests)
        std::cout << "request " << r.id << ": " << (r.block ? "block " : "unblock ") << r.set << " at " << r.time << '\n';
    for (const auto& op : schedule.operations) std::cout << "resolved " << op.id() << '\n';
}
}
int main(int argc, char** argv) {
    try {
        umask(0077);
        bool simulation = false; fs::path state; int i = 1;
        while (i < argc && std::string(argv[i]).starts_with("--")) {
            std::string option = argv[i++];
            if (option == "--simulate") simulation = true;
            else if (option == "--state") { require(i < argc, "--state requires a path"); state = argv[i++]; }
            else if (option == "--help") { std::cout << usage; return 0; }
            else throw std::runtime_error("Unknown option: " + option);
        }
        if (i == argc) { std::cout << usage; return 0; }
        if (state.empty()) state = simulation ? fs::current_path() / "simulation-state" : fs::path("/Library/Application Support/ProductivityBlocker");
        std::string command = argv[i++]; std::vector<std::string> arguments(argv+i, argv+argc);
        MacOSEnvironment environment(state, executablePath(), simulation);
        Executor executor(state, environment, simulation);
        // Mutations reconcile after the edit, avoiding recovery of an obsolete state first.
        if (command == "status") executor.recover();
        if (command == "app") {
            require(arguments.size() == 3, "app requires ID PATH UID");
            auto path = fs::canonical(arguments[1]);
            require(path.extension() == ".app" && path.string().starts_with("/") &&
                    !path.string().starts_with("/System/") && !path.string().starts_with("/Library/Apple/"),
                    "Select a third-party .app bundle outside protected system directories");
            struct stat st{}; require(stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "Not an app directory");
            auto user = number(arguments[2]); require(user > 0 && user <= std::numeric_limits<unsigned>::max(), "Select a non-root user UID");
            executor.addResource({arguments[0], "app", path.string(), unsigned(user), std::uint64_t(st.st_dev), st.st_ino});
        } else if (command == "website") {
            require(arguments.size() == 2, "website requires ID HOSTNAME");
            auto host = arguments[1];
            std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (!host.empty() && host.back() == '.') host.pop_back();
            require(!host.empty() && host.size() <= 253 && host.front() != '-' && host.back() != '-' &&
                std::all_of(host.begin(), host.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.'; }), "Use an ASCII hostname, not a URL or wildcard");
            executor.addResource({arguments[0], "website", host});
        } else if (command == "set") {
            require(!arguments.empty(), "set requires an ID");
            executor.saveSet({arguments[0], {arguments.begin()+1, arguments.end()}});
        } else if (command == "block" || command == "unblock") {
            require(arguments.size() == 1 || arguments.size() == 2, "Action requires SET and optional UTC time");
            ExecutionPolicy policy = ExecuteImmediately{};
            if (arguments.size() == 2) policy = ExecuteAt{parseTimestamp(arguments[1])};
            Block block; Unblock unblock;
            std::cout << "request " << executor.submit(arguments[0], command == "block" ? static_cast<const Action&>(block) : unblock, policy) << '\n';
        } else if (command == "cancel") {
            require(arguments.size() == 1, "cancel requires REQUEST_ID"); executor.cancel(number(arguments[0]));
        } else if (command == "execute") {
            require(arguments.size() == 1, "execute requires OPERATION_ID"); executor.execute(arguments[0]);
        } else if (command == "recover") {
            require(arguments.empty(), "recover takes no arguments"); executor.recover();
        } else if (command == "status") {
            require(arguments.empty(), "status takes no arguments"); show(executor.inspect());
        } else throw std::runtime_error("Unknown command: " + command);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\nSaved intent may need recovery; run status or recover after fixing the error.\n";
        return 1;
    }
}
