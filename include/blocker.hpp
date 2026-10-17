#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace blocker {
namespace fs = std::filesystem;
using Timestamp = std::int64_t; // UTC Unix seconds; scheduled CLI inputs are minutes.
struct ExecuteImmediately {};
struct ExecuteAt { Timestamp timestamp; };
using ExecutionPolicy = std::variant<ExecuteImmediately, ExecuteAt>;
struct ResourceDefinition {
    std::string id, type, target;
    unsigned user = 0;
    std::uint64_t device = 0, inode = 0;
};
class Resource {
public:
    virtual ~Resource() = default;
    virtual void block() = 0;
    virtual void unblock() = 0;
};
class Action {
public:
    virtual ~Action() = default;
    virtual bool blocks() const = 0; // Serialized behavior, not separate object state.
    virtual void execute(Resource&) const = 0;
};
class Block final : public Action {
public:
    bool blocks() const override { return true; }
    void execute(Resource& r) const override { r.block(); }
};
class Unblock final : public Action {
public:
    bool blocks() const override { return false; }
    void execute(Resource& r) const override { r.unblock(); }
};
struct BlockSet { std::string id; std::vector<std::string> resources; };
struct Request { std::uint64_t id; std::string set; bool block; Timestamp time; };
struct Operation {
    std::string resource;
    bool block;
    Timestamp time;
    std::string id() const;
    bool operator==(const Operation&) const = default;
};
struct Schedule {
    std::uint64_t nextRequest = 1;
    std::map<std::string, ResourceDefinition> resources;
    std::map<std::string, BlockSet> sets;
    std::vector<Request> requests;
    std::vector<Operation> operations; // Generated alongside requests in the same file.
};
std::vector<Operation> resolveSchedule(const Schedule&);
std::map<std::string, Operation> currentOperations(const Schedule&, Timestamp);
void atomicWrite(const fs::path&, const std::string&);
std::string readText(const fs::path&);
Timestamp currentTime();
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
struct ExecutionEnvironment {
    virtual ~ExecutionEnvironment() = default;
    virtual void synchronizeJobs(const std::vector<Operation>&, Timestamp) = 0;
    virtual std::unique_ptr<Resource> resource(const ResourceDefinition&) = 0;
};
class Executor {
public:
    Executor(fs::path state, ExecutionEnvironment&, bool simulation,
             std::function<Timestamp()> clock = currentTime);
    void addResource(ResourceDefinition);
    void saveSet(BlockSet);
    std::uint64_t submit(const std::string& set, const Action&, ExecutionPolicy = ExecuteImmediately{});
    void cancel(std::uint64_t request);
    void execute(const std::string& operation); // No overlap resolution here.
    void recover();
    Schedule inspect();
private:
    fs::path state_;
    ExecutionEnvironment& environment_;
    bool simulation_;
    std::function<Timestamp()> clock_;
    Schedule load();
    void save(const Schedule&);
    void applyCurrent(const Schedule&);
    void commit(Schedule&);
};
class MacOSApp final : public Resource {
public:
    MacOSApp(ResourceDefinition, fs::path state, bool protect = true);
    void block() override;
    void unblock() override;
private:
    ResourceDefinition definition_;
    fs::path journal_;
    bool protect_;
};
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
class MacOSEnvironment final : public ExecutionEnvironment {
public:
    MacOSEnvironment(fs::path state, fs::path executable, bool simulation);
    void synchronizeJobs(const std::vector<Operation>&, Timestamp) override;
    std::unique_ptr<Resource> resource(const ResourceDefinition&) override;
private:
    fs::path state_, executable_;
    bool simulation_;
};
} // namespace blocker
