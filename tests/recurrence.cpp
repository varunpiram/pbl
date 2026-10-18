#include "schedule.hpp"
#include "storage.hpp"
#include <cstdlib>
#include <algorithm>
#include <ctime>
#include <iostream>
using namespace blocker;

Timestamp at(int day, int hour, int minute = 0, int month = 10, int year = 2026) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    return mktime(&t);
}

std::vector<Operation> resources(std::vector<Operation> operations) {
    std::erase_if(operations, [](const Operation& op) {
        return op.set;
    });
    return operations;
}

int main() {
    setenv("TZ", "America/New_York", 1);
    tzset();
    Schedule s;
    s.sets["focus"] = {"focus", {"app"}};
    s.weekly = {{"focus", 1, 540, 600}};
    auto now = at(19, 8);
    s.operations = resolveSchedule(s, now);
    require(resources(s.operations).size() == 4 && s.operations.front().time == at(19, 9) &&
                s.operations.back().time == at(26, 10),
            "Two-week horizon");
    require(resolveSchedule(s, now) == s.operations && s.requests.empty(),
            "Renewal is idempotent without generated requests");
    auto advanced = resources(resolveSchedule(s, at(20, 8)));
    require(advanced.size() == 4 && advanced.back().time == at(2, 10, 0, 11), "Horizon advances");
    s.requests = {{1, "focus", true, at(19, 9, 30)}, {2, "focus", false, at(19, 11)}};
    s.nextRequest = 3;
    s.weekly.push_back({"focus", 1, 570, 630});
    s.operations.clear();
    auto overlap = resources(resolveSchedule(s, now));
    require(overlap[0].time == at(19, 9) && overlap[1].time == at(19, 11),
            "Quick and weekly blocks union");
    s.requests.clear();
    s.weekly.resize(1);
    s.operations = resolveSchedule(s, now);
    auto active = at(19, 9, 30), deadline = at(19, 10);
    setenv("TZ", "America/Los_Angeles", 1);
    tzset();
    auto moved = resources(resolveSchedule(s, active));
    require(moved[1].time == deadline && moved[2].time == at(19, 9),
            "Travel preserves active deadline and rebases future blocks");
    setenv("TZ", "America/New_York", 1);
    tzset();
    s.operations.clear();
    s.weekly = {{"focus", 1, 1320, 420}};
    auto overnight = resources(resolveSchedule(s, at(20, 1)));
    require(overnight[0].time == at(19, 22) && overnight[1].time == at(20, 7), "Overnight renewal");
    s.weekly = {{"focus", 0, 540, 600}};
    auto dst = resources(resolveSchedule(s, at(7, 8, 0, 3, 2027)));
    require(dst[2].time - dst[0].time == 7 * 86400 - 3600, "Weekly wall time follows DST");
    s.weekly = {{"focus", 0, 150, 240}};
    require(resolveSchedule(s, at(14, 0, 0, 3, 2027))[0].time == at(14, 3, 0, 3, 2027),
            "Missing time advances to first valid minute");
    s.weekly = {{"focus", 0, 90, 150}};
    require(resolveSchedule(s, at(1, 0, 0, 11))[0].time == 1793511000,
            "Repeated time uses first occurrence");
    s.operations = resolveSchedule(s, at(1, 0, 0, 11));
    require(resolveSchedule(s, at(1, 8, 0, 6, 2027)).front().time > at(1, 8, 0, 6, 2027),
            "Renewal after extended downtime");
    s.sets["shared"] = {"shared", {"app"}};
    s.weekly = {{"focus", 1, 540, 600}};
    s.operations = resolveSchedule(s, at(19, 9, 30));
    requireEditable(s, "shared", at(19, 9, 30));
    bool rejected = false;
    try {
        requireEditable(s, "focus", at(19, 9, 30));
    } catch (...) {
        rejected = true;
    }
    require(rejected, "Only the active set is locked");
    Schedule batch;
    batch.sets["focus"] = {"focus", {"app"}};
    auto submitted = applySubmissions(
        batch, {{"focus", true, ExecuteImmediately{}}, {"focus", false, ExecuteAt{now + 3600}}},
        now);
    require(batch.requests.empty() && submitted.requests.size() == 2,
            "Batch produces one complete candidate");
    rejected = false;
    try {
        applySubmissions(
            batch, {{"focus", true, ExecuteImmediately{}}, {"focus", false, ExecuteAt{now - 1}}},
            now);
    } catch (...) {
        rejected = true;
    }
    require(rejected && batch.requests.empty(), "Invalid batch leaves the schedule unchanged");
    std::cout << "PASS recurrence, renewal, overlap, travel, overnight, DST, downtime\n";
}
