#pragma once
#include "schedule.hpp"

namespace blocker {
class MacOSEnvironment;

class Executor {
  public:
    Executor(fs::path state, MacOSEnvironment&);
    void saveSet(BlockSet, std::vector<ResourceDefinition>, std::vector<WeeklyWindow>);
    void removeSet(const std::string&);
    void submit(const std::vector<Submission>&);
    void execute(const std::string& operation);
    void recover();
    void refreshWebsites();
    Schedule inspect();

  private:
    fs::path state_;
    MacOSEnvironment& environment_;
    Schedule load();
    void applyCurrent(const Schedule&);
    void commit(Schedule&);
};
}
