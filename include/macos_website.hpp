#pragma once
#include "resource.hpp"
namespace blocker {
struct WebsiteRules {
    std::string text;
    std::vector<std::pair<int, std::string>> addresses;
};
WebsiteRules resolveWebsiteRules(const std::string& hostname);
class MacOSWebsite final : public Resource {
public:
    MacOSWebsite(ResourceDefinition, fs::path state);
    void block() override;
    void unblock() override;
private:
    ResourceDefinition definition_;
    fs::path state_;
};
}
