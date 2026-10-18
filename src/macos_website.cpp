#include "macos_website.hpp"
#include "storage.hpp"
#include "process.hpp"
#include <netdb.h>
#include <set>
#include <sstream>

namespace blocker {
MacOSWebsite::MacOSWebsite(ResourceDefinition definition, fs::path state)
    : definition_(std::move(definition)), state_(std::move(state)) {
}

WebsiteRules resolveWebsiteRules(const std::string& hostname) {
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(hostname.c_str(), nullptr, &hints, &addresses);
    require(error == 0, std::string("DNS resolution failed: ") + gai_strerror(error));
    std::set<std::pair<int, std::string>> unique;
    for (auto* a = addresses; a; a = a->ai_next) {
        char host[NI_MAXHOST];
        if ((a->ai_family == AF_INET || a->ai_family == AF_INET6) &&
            getnameinfo(a->ai_addr, a->ai_addrlen, host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0)
            unique.emplace(a->ai_family, host);
    }
    freeaddrinfo(addresses);
    require(!unique.empty(), "DNS returned no addresses");
    std::ostringstream text;
    for (const auto& [family, address] : unique)
        text << "block return out quick " << (family == AF_INET ? "inet" : "inet6") << " to "
             << address << '\n';
    return {text.str(), {unique.begin(), unique.end()}};
}

void MacOSWebsite::block() {
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
    WebsiteRules ruleset;
    auto file = state_ / (definition_.id + ".pf");
    if (fs::exists(file)) {
        ruleset.text = readText(file);
        std::istringstream input(ruleset.text);
        std::string line;
        while (std::getline(input, line))
            if (!line.empty())
                ruleset.addresses.emplace_back(line.find(" inet6 ") != std::string::npos ? AF_INET6
                                                                                         : AF_INET,
                                               line.substr(line.find_last_of(' ') + 1));
    } else {
        ruleset = resolveWebsiteRules(definition_.target);
        atomicWrite(file, ruleset.text);
    }
    checked({"/sbin/pfctl", "-a", "productivity-blocker/" + definition_.id, "-f", file.string()});
    for (const auto& [family, address] : ruleset.addresses)
        checked({"/sbin/pfctl", "-k", family == AF_INET ? "0.0.0.0/0" : "::/0", "-k", address});
}

void MacOSWebsite::unblock() {
    auto file = state_ / (definition_.id + ".pf");
    if (!fs::exists(file))
        return;
    checked({"/sbin/pfctl", "-a", "productivity-blocker/" + definition_.id, "-f", "/dev/null"});
    fs::remove(file);
}
}
