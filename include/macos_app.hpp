#pragma once
#include "resource.hpp"

namespace blocker {
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
