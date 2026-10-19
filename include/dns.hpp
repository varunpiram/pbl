#pragma once
#include "schedule.hpp"
#include <functional>

namespace blocker {
using AddressCache = std::map<std::string, Timestamp>;
std::vector<std::string> websiteHosts(const std::string&);
AddressCache resolveAddresses(const std::string&, Timestamp);
AddressCache mergeAddresses(AddressCache, const AddressCache&, Timestamp);
AddressCache refreshAddresses(const AddressCache&, const std::function<AddressCache()>&, Timestamp);
bool isIPAddress(const std::string&);
}
