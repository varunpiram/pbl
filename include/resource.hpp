#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace blocker {
namespace fs = std::filesystem;

struct ResourceDefinition {
    std::string id, type, target;
    unsigned user = 0;
    std::string requirement = {};
};

class Resource {
  public:
    virtual ~Resource();
    virtual void block() = 0;
    virtual void unblock() = 0;
};

class Action {
  public:
    virtual ~Action();
    virtual bool blocks() const = 0;
    virtual void execute(Resource&) const = 0;
};

class Block final : public Action {
  public:
    bool blocks() const override;
    void execute(Resource&) const override;
};

class Unblock final : public Action {
  public:
    bool blocks() const override;
    void execute(Resource&) const override;
};
}
