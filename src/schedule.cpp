#include "schedule.hpp"
#include "storage.hpp"
#include <algorithm>
#include <ctime>
#include <set>
#include <tuple>
#include <limits>

namespace blocker {
static Timestamp resolveLocalTime(std::tm day, int minutes, int extraDay) {
    auto date = day;
    date.tm_mday += extraDay;
    date.tm_isdst = -1;
    require(mktime(&date) != -1, "Invalid recurrence day");
    date.tm_hour = minutes / 60;
    date.tm_min = minutes % 60;
    date.tm_isdst = -1;
    auto fields = [](const std::tm& t) {
        return std::tuple(t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min);
    };
    auto requested = date;
    auto result = mktime(&date);
    require(result != -1, "Invalid recurrence deadline");
    auto alternate = requested;
    alternate.tm_isdst = !date.tm_isdst;
    auto earlier = mktime(&alternate);
    if (earlier != -1 && earlier < result && fields(alternate) == fields(requested))
        result = earlier;
    while (fields(date) > fields(requested)) {
        auto previous = result - 60;
        require(localtime_r(&previous, &date) != nullptr, "Invalid recurrence deadline");
        if (fields(date) >= fields(requested))
            result = previous;
        else
            break;
    }
    return result;
}

std::string Operation::id() const {
    return (set ? "set." : "") + target + "." + std::to_string(time) +
           (block ? ".block" : ".unblock");
}

static std::vector<Operation> resolveRequests(const Schedule& schedule) {
    auto requests = schedule.requests;
    std::sort(requests.begin(), requests.end(), [](const Request& a, const Request& b) {
        return std::tie(a.time, a.id) < std::tie(b.time, b.id);
    });
    std::map<std::string, bool> active;
    std::set<std::string> previous, previousSets;
    std::vector<Operation> result;
    for (std::size_t i = 0; i < requests.size();) {
        Timestamp time = requests[i].time;
        do {
            active[requests[i].set] = requests[i].block;
            ++i;
        } while (i < requests.size() && requests[i].time == time);
        std::set<std::string> next, nextSets;
        for (const auto& [id, blocked] : active)
            if (blocked) {
                nextSets.insert(schedule.sets.at(id).id);
                const auto& members = schedule.sets.at(id).resources;
                next.insert(members.begin(), members.end());
            }
        for (const auto& id : previous)
            if (!next.contains(id))
                result.push_back({id, false, time});
        for (const auto& id : next)
            if (!previous.contains(id))
                result.push_back({id, true, time});
        for (const auto& id : previousSets)
            if (!nextSets.contains(id))
                result.push_back({id, false, time, true});
        for (const auto& id : nextSets)
            if (!previousSets.contains(id))
                result.push_back({id, true, time, true});
        previousSets = std::move(nextSets);
        previous = std::move(next);
    }
    return result;
}

std::vector<Operation> resolveSchedule(const Schedule& schedule, Timestamp now) {
    auto expanded = schedule;
    auto interval = [&](const std::string& set, Timestamp start, Timestamp end) {
        auto id = ".weekly" + std::to_string(expanded.nextRequest);
        expanded.sets[id] = {set, schedule.sets.at(set).resources};
        expanded.requests.push_back({expanded.nextRequest++, id, true, start});
        expanded.requests.push_back({expanded.nextRequest++, id, false, end});
    };
    std::time_t time = now;
    std::tm today{};
    require(localtime_r(&time, &today) != nullptr, "Invalid recurrence time");
    for (int offset = -1; offset <= 14; ++offset) {
        auto day = today;
        day.tm_mday += offset;
        day.tm_hour = 12;
        day.tm_min = day.tm_sec = 0;
        day.tm_isdst = -1;
        require(mktime(&day) != -1, "Invalid recurrence date");
        for (const auto& w : schedule.weekly)
            if (w.day == day.tm_wday) {
                auto start = resolveLocalTime(day, w.start, 0),
                     end = resolveLocalTime(day, w.end, w.end <= w.start);
                if (start < now + 14 * 86400 && end > now && end > start)
                    interval(w.set, start, end);
            }
    }
    for (const auto& [set, op] : currentOperations(schedule, now, true))
        if (op.block) {
            auto end = std::find_if(
                schedule.operations.begin(), schedule.operations.end(), [&](const Operation& next) {
                    return next.set && next.target == set && !next.block && next.time > now;
                });
            if (end != schedule.operations.end())
                interval(set, op.time, end->time);
        }
    return resolveRequests(expanded);
}

std::map<std::string, Operation> currentOperations(const Schedule& schedule, Timestamp now,
                                                   bool sets) {
    std::map<std::string, Operation> result;
    for (const auto& op : schedule.operations)
        if (op.set == sets && op.time <= now)
            result[op.target] = op;
    return result;
}

std::vector<std::pair<Timestamp, Timestamp>> setIntervals(const Schedule& schedule,
                                                          const std::string& set, bool quickOnly) {
    const auto operations = quickOnly ? resolveRequests(schedule) : schedule.operations;
    std::vector<std::pair<Timestamp, Timestamp>> result;
    Timestamp start = -1;
    for (const auto& op : operations)
        if (op.set && op.target == set) {
            if (op.block)
                start = op.time;
            else if (start >= 0) {
                result.emplace_back(start, op.time);
                start = -1;
            }
        }
    return result;
}

void requireEditable(const Schedule& schedule, const std::string& set, Timestamp now) {
    auto resolved = schedule;
    resolved.operations = resolveSchedule(schedule, now);
    auto active = currentOperations(resolved, now, true);
    require(!active.contains(set) || !active.at(set).block, "Cannot modify an active set");
}

void requireValidSetEdit(const Schedule& schedule, const BlockSet& set,
                         const std::vector<WeeklyWindow>& windows, Timestamp now) {
    auto previous = schedule.sets.find(set.id);
    if (previous == schedule.sets.end())
        return;
    bool retainsResources = std::ranges::all_of(previous->second.resources, [&](const auto& id) {
        return std::ranges::find(set.resources, id) != set.resources.end();
    });
    auto previousWindows = schedule.weekly;
    std::erase_if(previousWindows, [&](const WeeklyWindow& window) {
        return window.set != set.id;
    });
    if (!retainsResources || previousWindows != windows)
        requireEditable(schedule, set.id, now);
}

Schedule applySubmissions(Schedule schedule, const std::vector<Submission>& submissions,
                          Timestamp now) {
    for (const auto& r : submissions) {
        require(schedule.sets.contains(r.set) && !schedule.sets.at(r.set).resources.empty(),
                "Select a nonempty set");
        requireEditable(schedule, r.set, now);
    }
    for (const auto& r : submissions) {
        auto time = std::holds_alternative<ExecuteAt>(r.policy)
                        ? std::get<ExecuteAt>(r.policy).timestamp
                        : now;
        require(time >= now, "Actions must not be in the past");
        require(schedule.nextRequest < std::numeric_limits<std::uint64_t>::max(),
                "Request ID space exhausted");
        schedule.requests.push_back({schedule.nextRequest++, r.set, r.block, time});
    }
    return schedule;
}

}
