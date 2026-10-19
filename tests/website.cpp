#include "dns.hpp"
#include "hosts.hpp"
#include "storage.hpp"
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
using namespace blocker;

int main() {
    require(websiteHosts("x.com") == std::vector<std::string>{"x.com", "www.x.com"},
            "Include www alias");
    require(websiteHosts("www.x.com") == std::vector<std::string>{"www.x.com", "x.com"},
            "Include apex alias");
    require(websiteHosts("192.0.2.1").size() == 1 && websiteHosts("2001:db8::1").size() == 1,
            "Numeric addresses have no aliases");

    AddressCache old{{"192.0.2.1", 400}, {"2001:db8::1", 300}};
    AddressCache fresh{{"192.0.2.2", 500}, {"192.0.2.1", 200}};
    auto merged = mergeAddresses(old, fresh, 300);
    require(merged == AddressCache{{"192.0.2.1", 400}, {"192.0.2.2", 500}},
            "Expire absent addresses at deadline; retain previously valid lifetime");
    require(mergeAddresses(merged, {}, 500).empty(), "Successful empty response allows expiration");
    auto failed = []() -> AddressCache {
        throw std::runtime_error("fixture DNS failure");
    };
    require(refreshAddresses(old, failed, 1000) == old,
            "DNS failure preserves expired working table");
    bool threw = false;
    try {
        refreshAddresses({}, failed, 1000);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "Initial DNS failure is reported");

    const std::string original =
        "127.0.0.1 localhost\n::1 localhost\n192.0.2.3 example.org # existing\n";
    auto one = websiteHostsFile(original, "one", websiteHosts("x.com"));
    require(websiteHostsFile(one, "one", websiteHosts("x.com")) == one,
            "Repeating an installation is idempotent");
    auto two = websiteHostsFile(one, "two", websiteHosts("www.x.com"));
    require(websiteHostsFile(two, "one", websiteHosts("x.com")) == two,
            "Refreshing multiple resources must not reorder hosts or flush DNS again");
    auto remaining = websiteHostsFile(two, "one", {});
    require(remaining.find(" # pbl:two") != std::string::npos && remaining.starts_with(original),
            "Removing one resource preserves overlapping aliases and unrelated hosts");
    require(websiteHostsFile(remaining, "two", {}) == original,
            "Full unblock restores unrelated text");
    require(websiteHostsFile(original, "absent", {}) == original, "Absent unblock changes nothing");
    require(websiteHostsFile("127.0.0.1 localhost", "absent", {}) == "127.0.0.1 localhost" &&
                websiteHostsFile(original + "0.0.0.0 x.com # pbl:one", "one", {}) == original,
            "Preserve or remove a final line without a newline");

    char directory[] = "/tmp/pbl-website-test-XXXXXX";
    require(mkdtemp(directory), "Create test directory");
    auto file = fs::path(directory) / "hosts";
    atomicWrite(file, one, 0644);
    atomicWrite(file, original, 0644);
    struct stat metadata{};
    require(stat(file.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0644 &&
                readText(file) == original,
            "Atomic hosts replacement retains readable permissions");
    fs::remove_all(directory);
    std::cout << "PASS address expiration, DNS failure, overlapping hosts, retries and file "
                 "permissions\n";
}
