#pragma once
#include "resource.hpp"

namespace blocker {
std::string appRequirement(const fs::path&, const std::string& expected = {});

class MacOSApp final : public Resource {
  public:
    MacOSApp(ResourceDefinition, fs::path state);
    void block() override;
    void unblock() override;

  private:
    ResourceDefinition definition_;
    fs::path journal_;
};
}
