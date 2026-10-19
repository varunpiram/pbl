#include "macos_website.hpp"
#include "dns.hpp"
#include "hosts.hpp"
#include "storage.hpp"
#include "process.hpp"
#include <sstream>

namespace blocker {
MacOSWebsite::MacOSWebsite(ResourceDefinition definition, fs::path state)
    : definition_(std::move(definition)), state_(std::move(state)) {
}

static void ensurePFEnabled() {
    auto rules = run({"/sbin/pfctl", "-s", "rules"});
    if (rules.output.find("anchor \"productivity-blocker/*\"") == std::string::npos) {
        std::istringstream input(rules.output);
        std::string line;
        while (std::getline(input, line))
            require(line.empty() || line == "anchor \"com.apple/*\" all" ||
                        line == "scrub-anchor \"com.apple/*\" all fragment reassemble" ||
                        line.starts_with("No ALTQ") || line.starts_with("ALTQ related"),
                    "PF has custom active rules; install the productivity-blocker anchor without "
                    "replacing them");
        checked({"/sbin/pfctl", "-f", "/etc/pf.conf"});
        rules = run({"/sbin/pfctl", "-s", "rules"});
    }
    require(rules.status == 0 &&
                rules.output.find("anchor \"productivity-blocker/*\"") != std::string::npos,
            "Run local setup to install the PF anchor");
    auto status = run({"/sbin/pfctl", "-s", "info"});
    if (status.output.find("Status: Enabled") == std::string::npos)
        checked({"/sbin/pfctl", "-E"});
}

static AddressCache loadAddresses(const fs::path& file, const fs::path& legacy, Timestamp now) {
    AddressCache previous;
    if (fs::exists(file)) {
        std::istringstream input(readText(file));
        std::string address;
        Timestamp expiry;
        while (input >> address >> expiry) {
            require(isIPAddress(address), "Invalid cached IP");
            previous[address] = expiry;
        }
        require(input.eof(), "Invalid DNS cache");
    }
    if (!fs::exists(file) && fs::exists(legacy)) {
        std::istringstream input(readText(legacy));
        std::string line;
        while (std::getline(input, line)) {
            auto address = line.substr(line.find_last_of(' ') + 1);
            if (line.starts_with("block ") && isIPAddress(address))
                previous[address] = now + 60;
        }
    }
    return previous;
}

void MacOSWebsite::block() {
    updateHosts(definition_.id, websiteHosts(definition_.target));
    ensurePFEnabled();
    const auto file = state_ / (definition_.id + ".dns");
    const auto rulesFile = state_ / (definition_.id + ".pf");
    const auto anchor = "productivity-blocker/" + definition_.id;
    auto now = currentTime();
    auto previous = loadAddresses(file, rulesFile, now);
    auto addresses = refreshAddresses(
        previous,
        [&] {
            return resolveAddresses(definition_.target, now);
        },
        now);
    const std::string filter = "table <blocked> persist\nblock return out quick to <blocked>\n";
    bool reinstall = !fs::exists(rulesFile) || readText(rulesFile) != filter ||
                     run({"/sbin/pfctl", "-a", anchor, "-s", "rules"}).output.find("<blocked>") ==
                         std::string::npos;
    if (reinstall) {
        atomicWrite(rulesFile, filter);
        checked({"/sbin/pfctl", "-a", anchor, "-f", rulesFile.string()});
    }
    std::vector<std::string> replace{"/sbin/pfctl", "-a", anchor, "-t", "blocked", "-T", "replace"};
    std::ostringstream cache;
    for (const auto& [address, expiry] : addresses) {
        replace.push_back(address);
        cache << address << ' ' << expiry << '\n';
    }
    checked(replace);
    for (const auto& [address, expiry] : addresses)
        if (reinstall || !previous.contains(address))
            checked({"/sbin/pfctl", "-k",
                     address.find(':') == std::string::npos ? "0.0.0.0/0" : "::/0", "-k", address});
    atomicWrite(file, cache.str());
}

void MacOSWebsite::unblock() {
    const auto file = state_ / (definition_.id + ".pf");
    if (fs::exists(file)) {
        const auto anchor = "productivity-blocker/" + definition_.id;
        checked({"/sbin/pfctl", "-a", anchor, "-f", "/dev/null"});
        checked({"/sbin/pfctl", "-a", anchor, "-t", "blocked", "-T", "kill"});
        fs::remove(file);
    }
    updateHosts(definition_.id, {});
    fs::remove(state_ / (definition_.id + ".dns"));
    fs::remove(state_ / (definition_.id + ".addresses"));
}
}
