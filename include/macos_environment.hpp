#pragma once
#include "schedule.hpp"
#include <memory>

namespace blocker {
class MacOSEnvironment final {
  public:
    MacOSEnvironment(fs::path state, fs::path executable);
    void synchronizeJobs(const Schedule&, Timestamp);
    void synchronizeWebsiteJobs(const Schedule&, Timestamp);
    std::unique_ptr<Resource> resource(const ResourceDefinition&);

  private:
    fs::path state_, executable_;
};

ResourceDefinition validateResource(ResourceDefinition);
}
