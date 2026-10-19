#pragma once
#include <string>
#include <vector>

namespace blocker {
std::string websiteHostsFile(const std::string&, const std::string&,
                             const std::vector<std::string>&);
void updateHosts(const std::string&, const std::vector<std::string>&);
}
