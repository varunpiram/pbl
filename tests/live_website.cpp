#include "config.h"
#include "macos_website.hpp"
#include "dns.hpp"
#include "hosts.hpp"
#include "process.hpp"
#include "storage.hpp"
#include <iostream>
#include <unistd.h>
using namespace blocker;

int main() {
    require(geteuid() == 0, "Run this fixture with sudo");
    FileLock lock(fs::path(PBL_STATE) / "schedule.lock");
    char directory[] = "/private/tmp/pbl-live-website-XXXXXX";
    require(mkdtemp(directory), "Create fixture state");
    ResourceDefinition definition;
    definition.id = "live-website-" + std::to_string(getpid());
    definition.type = "website";
    definition.target = "example.org";
    MacOSWebsite website(definition, directory);
    const auto before = readText("/etc/hosts");
    auto request = [](const std::vector<std::string>& extra = {}) {
        std::vector<std::string> args{"/usr/bin/curl", "--noproxy",  "*", "--silent",
                                      "--show-error",  "--max-time", "5", "--output",
                                      "/dev/null"};
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back("https://example.org/");
        return run(args);
    };
    try {
        require(request().status == 0, "Fixture website is not initially reachable");
        auto addresses = resolveAddresses(definition.target, currentTime());
        require(!addresses.empty(), "Fixture DNS returned no addresses");
        website.block();
        require(request().status != 0, "Hosts did not block fixture");
        bool ipv4 = false;
        for (const auto& [address, expiry] : addresses)
            if (address.find(':') == std::string::npos) {
                require(request({"--resolve", "example.org:443:" + address}).status != 0,
                        "PF did not block direct IPv4 address");
                ipv4 = true;
                break;
            }
        require(ipv4, "Fixture requires an IPv4 address");
        website.block();
        website.unblock();
        website.unblock();
        require(readText("/etc/hosts") == before, "Hosts were not restored exactly");
        require(request().status == 0, "Fixture remains blocked after restoration");
        fs::remove_all(directory);
        std::cout << "PASS live hosts, direct-IP PF rejection, refresh, repeated unblock and "
                     "restoration\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            website.unblock();
            fs::remove_all(directory);
        } catch (const std::exception& cleanup) {
            std::cerr << "Cleanup failed; fixture state at " << directory << ": " << cleanup.what()
                      << '\n';
        }
        return 1;
    }
}
