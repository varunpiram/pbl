#pragma once
#include "resource.hpp"
#include <map>
#include <variant>
namespace blocker {
using Timestamp = std::int64_t;
struct ExecuteImmediately {};
struct ExecuteAt { Timestamp timestamp; };
using ExecutionPolicy = std::variant<ExecuteImmediately, ExecuteAt>;
struct BlockSet { std::string id; std::vector<std::string> resources; };
struct Submission { std::string set; bool block; ExecutionPolicy policy; };
struct Request { std::uint64_t id; std::string set; bool block; Timestamp time; };
struct WeeklyWindow { std::string set; int day, start, end; };
struct Operation {
    std::string target;
    bool block;
    Timestamp time;
    bool set = false;
    std::string id() const;
    bool operator==(const Operation&) const = default;
};
struct Schedule {
    std::uint64_t nextRequest = 1;
    std::map<std::string, ResourceDefinition> resources;
    std::map<std::string, BlockSet> sets;
    std::vector<Request> requests;
    std::vector<Operation> operations;
    std::vector<WeeklyWindow> weekly;
};
Timestamp currentTime();
std::vector<Operation> resolveSchedule(const Schedule&, Timestamp now = currentTime());
std::map<std::string, Operation> currentOperations(const Schedule&, Timestamp, bool sets = false);
std::vector<std::pair<Timestamp, Timestamp>> setIntervals(const Schedule&, const std::string&);
Schedule applySubmissions(Schedule, const std::vector<Submission>&, Timestamp);
void requireEditable(const Schedule&, const std::string&, Timestamp);
}
