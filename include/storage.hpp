#pragma once
#include "schedule.hpp"
#include <sys/types.h>

namespace blocker {
void atomicWrite(const fs::path&, const std::string&, mode_t mode = 0600);
std::string readText(const fs::path&);
void require(bool, const std::string&);

class FileLock {
  public:
    explicit FileLock(const fs::path&);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

  private:
    int fd_ = -1;
};

Schedule loadSchedule(const fs::path&);
void saveSchedule(const fs::path&, const Schedule&);
void validateId(const std::string&);
}
