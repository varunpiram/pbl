#include "hosts.hpp"
#include "storage.hpp"
#include "process.hpp"
#include <sstream>
#include <sys/stat.h>

namespace blocker {
std::string websiteHostsFile(const std::string& text, const std::string& id,
                             const std::vector<std::string>& hosts) {
    const auto marker = " # pbl:" + id;
    std::string result, existing, expected;
    for (const auto& host : hosts)
        for (const auto& address : {"0.0.0.0", "::"})
            expected += std::string(address) + " " + host + marker + "\n";
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        auto& output = line.ends_with(marker) ? existing : result;
        output += line;
        if (!input.eof())
            output += '\n';
    }
    if (existing == expected)
        return text;
    if (!hosts.empty() && !result.empty() && result.back() != '\n')
        result += '\n';
    return result + expected;
}

void updateHosts(const std::string& id, const std::vector<std::string>& hosts) {
    const fs::path path = "/etc/hosts";
    auto before = readText(path);
    auto after = websiteHostsFile(before, id, hosts);
    if (after == before)
        return;
    struct stat metadata{};
    require(stat(path.c_str(), &metadata) == 0 && metadata.st_uid == 0 && metadata.st_gid == 0,
            "Unexpected hosts file ownership");
    atomicWrite(path, after, metadata.st_mode & 0777);
    checked({"/usr/bin/dscacheutil", "-flushcache"});
    checked({"/usr/bin/killall", "-HUP", "mDNSResponder"});
}

}
