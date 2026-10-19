#pragma once
#include "resource.hpp"

namespace blocker {
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
