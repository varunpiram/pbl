#include "blocker.hpp"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <csignal>
#include <limits>
#include <membership.h>
#include <iostream>
#include <random>
#include <set>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace blocker;
namespace {
void check(bool value, const std::string& why) { require(value, "TEST: " + why); }
template<class F> void rejects(F action) {
    bool threw = false; try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected failure");
}
struct Temp {
    fs::path path;
    Temp() { char name[] = "/tmp/productivity-blocker-test-XXXXXX"; auto p = mkdtemp(name); require(p, "mkdtemp"); path = p; }
    ~Temp() { std::error_code error; fs::remove_all(path, error); }
};
struct FakeEnvironment : ExecutionEnvironment {
    std::map<std::string, bool> blocked;
    std::vector<Operation> jobs;
    bool failJobs = false, failResource = false;
    int calls = 0;
    struct Item : Resource {
        FakeEnvironment& owner; std::string id;
        Item(FakeEnvironment& o, std::string value) : owner(o), id(std::move(value)) {}
        void block() override {
            owner.blocked[id] = true; ++owner.calls;
            if (owner.failResource) throw std::runtime_error("simulated partial effect");
        }
        void unblock() override { owner.blocked[id] = false; ++owner.calls; }
    };
    void synchronizeJobs(const std::vector<Operation>& operations, Timestamp now) override {
        if (failJobs) throw std::runtime_error("simulated registration failure");
        jobs.clear(); for (const auto& op : operations) if (op.time > now) jobs.push_back(op);
    }
    std::unique_ptr<Resource> resource(const ResourceDefinition& r) override { return std::make_unique<Item>(*this, r.id); }
};
struct Fixture {
    Temp temp; FakeEnvironment env; Timestamp now = 0;
    Executor executor{temp.path, env, true, [&] { return now; }};
    Fixture() {
        executor.addResource({"spotify", "app", "/fake/Spotify.app"});
        executor.addResource({"reddit", "website", "reddit.com"});
        executor.addResource({"youtube", "website", "youtube.com"});
        executor.saveSet({"A", {"spotify", "reddit"}});
        executor.saveSet({"B", {"spotify", "youtube"}});
    }
};
void overlapAndCancellation() {
    Fixture f;
    f.executor.submit("A", Block{}, ExecuteAt{360});
    f.executor.submit("A", Unblock{}, ExecuteAt{480});
    auto request = f.executor.submit("B", Block{}, ExecuteAt{420});
    f.executor.submit("B", Unblock{}, ExecuteAt{600});
    auto schedule = f.executor.inspect();
    std::vector<Operation> spotify;
    for (const auto& op : schedule.operations) if (op.resource == "spotify") spotify.push_back(op);
    check(spotify == std::vector<Operation>{{"spotify",true,360},{"spotify",false,600}}, "merge shared resource intervals");
    f.now = 480; f.executor.recover();
    check(f.env.blocked["spotify"] && !f.env.blocked["reddit"] && f.env.blocked["youtube"], "overlap at A end");
    f.executor.cancel(request);
    check(!f.env.blocked["spotify"] && !f.env.blocked["youtube"], "cancel reinstates earlier unblock");
}
void touchingAndConflicts() {
    Fixture f;
    f.executor.submit("A", Block{}, ExecuteAt{60}); f.executor.submit("A", Unblock{}, ExecuteAt{120});
    f.executor.submit("B", Block{}, ExecuteAt{120}); f.executor.submit("B", Unblock{}, ExecuteAt{180});
    for (const auto& op : f.executor.inspect().operations)
        check(!(op.resource == "spotify" && op.time == 120), "no transient toggle at touching interval");
    f.executor.submit("A", Block{}, ExecuteAt{120}); // Later request at same instant wins.
    f.now = 200; f.executor.recover(); check(f.env.blocked["spotify"], "same-time explicit last request");
}
void staleDeliveryAndRecovery() {
    Fixture f;
    f.executor.submit("A", Block{}, ExecuteAt{60});
    f.executor.submit("A", Unblock{}, ExecuteAt{120});
    f.executor.submit("A", Block{}, ExecuteAt{180});
    f.now = 190; f.executor.recover(); int calls = f.env.calls;
    f.executor.execute("spotify.120.unblock"); f.executor.execute("spotify.60.block");
    check(f.env.calls == calls && f.env.blocked["spotify"], "late actions cannot override current interval");
    f.executor.execute("spotify.180.block"); f.executor.execute("spotify.180.block");
    check(f.env.blocked["spotify"], "duplicate execution safe");
    f.executor.submit("A", Unblock{}, ExecuteAt{240}); f.now = 300;
    f.env.blocked["spotify"] = true; f.executor.recover();
    check(!f.env.blocked["spotify"], "missed unblock repaired");
}
void cancelledActionCannotRun() {
    Fixture f;
    auto id = f.executor.submit("A", Block{}, ExecuteAt{60});
    f.executor.cancel(id); f.now = 70; int calls = f.env.calls;
    f.executor.execute("spotify.60.block"); check(f.env.calls == calls, "cancelled job skipped");
}
void interruptedUpdate() {
    Fixture f; f.now = 60; f.env.failJobs = true;
    rejects([&] { f.executor.submit("A", Block{}); });
    check(f.executor.inspect().requests.size() == 1, "intent survives interrupted registration");
    f.env.failJobs = false; f.executor.recover(); check(f.env.blocked["spotify"], "recovery applies saved intent");
    f.env.failResource = true; rejects([&] { f.executor.recover(); });
    f.env.failResource = false; f.executor.recover(); check(f.env.blocked["spotify"], "partial resource retry");
}
void persistentStateAndValidation() {
    Fixture f;
    f.executor.submit("A", Block{}, ExecuteAt{60});
    FakeEnvironment other; Executor reopened(f.temp.path, other, true, [] { return 70; });
    reopened.recover(); check(other.blocked["spotify"], "independent process can restore schedule");
    rejects([&] { reopened.addResource({"../escape", "app", "x"}); });
    rejects([&] { reopened.saveSet({"C", {"missing"}}); });
    Executor wrongMode(f.temp.path, other, false); rejects([&] { wrongMode.inspect(); });
    auto contents = readText(f.temp.path / "schedule.txt");
    atomicWrite(f.temp.path / "schedule.txt", contents + "request broken\n");
    rejects([&] { reopened.inspect(); });
    check(readText(f.temp.path / "schedule.txt").ends_with("request broken\n"), "malformed schedule not silently replaced");
}
void setEditsAndRemovedIntervals() {
    Fixture f; f.now = 60;
    auto id = f.executor.submit("A", Block{});
    f.executor.saveSet({"A", {"youtube"}});
    check(!f.env.blocked["spotify"] && !f.env.blocked["reddit"] && f.env.blocked["youtube"], "explicit set edit updates current restrictions");
    f.executor.cancel(id); check(!f.env.blocked["youtube"], "empty timeline clears prior restriction");
}
void resolverAgainstOracle() {
    std::mt19937 random(42);
    for (int trial = 0; trial < 100; ++trial) {
        Schedule s;
        for (int r = 0; r < 4; ++r) s.resources[std::to_string(r)] = {std::to_string(r), "app", "fake"};
        s.sets["A"] = {"A", {"0", "1"}}; s.sets["B"] = {"B", {"1", "2"}}; s.sets["C"] = {"C", {"2", "3"}};
        for (int n = 1; n <= 30; ++n)
            s.requests.push_back({std::uint64_t(n), std::string(1, char('A'+random()%3)), bool(random()%2), Timestamp(random()%12)});
        s.operations = resolveSchedule(s);
        for (Timestamp t = 0; t < 14; ++t) {
            std::set<std::string> expected;
            for (const auto& [id, set] : s.sets) {
                const Request* latest = nullptr;
                for (const auto& request : s.requests) if (request.set == id && request.time <= t &&
                    (!latest || std::tie(request.time, request.id) > std::tie(latest->time, latest->id))) latest = &request;
                if (latest && latest->block) expected.insert(set.resources.begin(), set.resources.end());
            }
            std::set<std::string> actual;
            for (const auto& [id, op] : currentOperations(s, t)) if (op.block) actual.insert(id);
            check(actual == expected, "random schedule matches independent set-membership oracle");
        }
    }
}
void concurrentSubmissions() {
    Fixture f; std::vector<pid_t> children;
    for (int n = 0; n < 6; ++n) {
        auto pid = fork(); require(pid >= 0, "fork");
        if (pid == 0) {
            try {
                FakeEnvironment env; Executor executor(f.temp.path, env, true, [] { return 0; });
                for (int i = 0; i < 10; ++i) executor.submit("A", Block{}, ExecuteAt{60});
                _exit(0);
            } catch (...) { _exit(1); }
        }
        children.push_back(pid);
    }
    for (pid_t pid : children) { int status; waitpid(pid, &status, 0); check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child submission"); }
    auto s = f.executor.inspect(); check(s.requests.size() == 60 && s.nextRequest == 61, "no lost writes or duplicate IDs");
}
void lockReleasedOnExit() {
    Temp temp; auto pid = fork(); require(pid >= 0, "fork");
    if (pid == 0) { FileLock lock(temp.path / "schedule.lock"); _exit(0); }
    int status; waitpid(pid, &status, 0);
    FileLock acquired(temp.path / "schedule.lock");
}
std::string aclText(const fs::path& path) {
    auto acl = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
    if (!acl && errno == ENOENT) acl = acl_init(0);
    require(acl, "read fixture ACL");
    auto text = acl_to_text(acl, nullptr); require(text, "ACL text");
    std::string result(text); acl_free(text); acl_free(acl); return result;
}
void disposableAppACL() {
    Temp temp; auto app = temp.path / "Probe.app";
    fs::create_directories(app / "Contents/MacOS");
    auto executable = app / "Contents/MacOS/probe";
    std::ofstream(executable) << "#!/bin/sh\nexit 0\n"; chmod(executable.c_str(), 0755);
    struct stat st{}; stat(app.c_str(), &st); auto original = aclText(app);
    ResourceDefinition definition{"probe", "app", app.string(), getuid(), std::uint64_t(st.st_dev), st.st_ino};
    MacOSApp resource(definition, temp.path, false); // No root ownership/flags in this test.
    try {
        check(access(executable.c_str(), X_OK) == 0, "fixture initially executable");
        resource.block(); auto metadata = readText(temp.path / "probe.metadata");
        check(access(executable.c_str(), X_OK) != 0 && errno == EACCES, "bundle search ACL prevents execution access");
        resource.block(); check(readText(temp.path / "probe.metadata") == metadata, "retry preserves original metadata");
        resource.unblock(); resource.unblock();
        check(access(executable.c_str(), X_OK) == 0 && aclText(app) == original, "exact ACL restoration");
        resource.block();
        fs::rename(app, temp.path / "Moved.app"); fs::create_directory(app);
        rejects([&] { resource.unblock(); });
        fs::remove(app); fs::rename(temp.path / "Moved.app", app); resource.unblock();
    } catch (...) {
        // Best-effort restore our disposable fixture before propagating failure.
        try { resource.unblock(); } catch (...) {}
        throw;
    }
}
void crashAfterSave() {
    Fixture f; int signalPipe[2]; check(pipe(signalPipe) == 0, "pipe");
    auto pid = fork(); require(pid >= 0, "fork");
    if (pid == 0) {
        close(signalPipe[0]);
        struct CrashEnvironment : FakeEnvironment {
            int signal;
            explicit CrashEnvironment(int fd) : signal(fd) {}
            void synchronizeJobs(const std::vector<Operation>&, Timestamp) override {
                char ready = 'x'; write(signal, &ready, 1);
                for (;;) pause();
            }
        } env(signalPipe[1]);
        Executor executor(f.temp.path, env, true, [] { return 60; });
        executor.submit("A", Block{}); _exit(1);
    }
    close(signalPipe[1]); char ready;
    check(read(signalPipe[0], &ready, 1) == 1, "child reached post-save boundary"); close(signalPipe[0]);
    kill(pid, SIGKILL); int status; waitpid(pid, &status, 0);
    check(WIFSIGNALED(status), "actual killed updater");
    f.now = 60; f.executor.recover();
    check(f.executor.inspect().requests.size() == 1 && f.env.blocked["spotify"], "recover killed writer and released lock");
}
void interruptedAtomicWrite() {
    Temp temp; auto path = temp.path / "schedule.txt";
    const std::string old = "old complete schedule", replacement(4*1024*1024, 'x'); atomicWrite(path, old);
    auto pid = fork(); require(pid >= 0, "fork");
    if (pid == 0) { for (;;) atomicWrite(path, replacement); }
    for (int i = 0; i < 1000; ++i) {
        if (std::distance(fs::directory_iterator(temp.path), fs::directory_iterator{}) > 1) break;
        usleep(1000);
    }
    kill(pid, SIGKILL); int status; waitpid(pid, &status, 0);
    auto saved = readText(path); check(saved == old || saved == replacement, "killed write exposes complete old/new file only");
}
void execAndExistingACL() {
    Temp temp; auto app = temp.path / "Probe.app";
    fs::create_directories(app / "Contents/Helpers");
    auto program = app / "Contents/probe";
    std::ofstream(program) << "#!/bin/sh\nexit 0\n"; chmod(program.c_str(), 0755);
    auto helper = app / "Contents/Helpers/helper"; fs::copy_file(program, helper);
    auto separate = temp.path / "separate"; fs::copy_file(program, separate);
    uuid_t uuid; check(mbr_uid_to_uuid(getuid(), uuid) == 0, "UUID");
    auto acl = acl_init(1); acl_entry_t entry; acl_permset_t perms;
    check(acl_create_entry(&acl, &entry) == 0 && acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
          acl_set_qualifier(entry, uuid) == 0 && acl_get_permset(entry, &perms) == 0 &&
          acl_add_perm(perms, ACL_READ_DATA) == 0 && acl_set_file(app.c_str(), ACL_TYPE_EXTENDED, acl) == 0, "fixture ACL");
    acl_free(acl); auto original = aclText(app); struct stat st{}; stat(app.c_str(), &st);
    ResourceDefinition definition{"probe", "app", app.string(), getuid(), std::uint64_t(st.st_dev), st.st_ino};
    MacOSApp resource(definition, temp.path, false);
    auto execute = [](const fs::path& path) {
        auto pid = fork(); require(pid >= 0, "fork");
        if (pid == 0) { execl(path.c_str(), path.c_str(), nullptr); _exit(errno == EACCES ? 77 : 78); }
        int status; waitpid(pid, &status, 0); return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };
    try {
        check(execute(program) == 0 && execute(helper) == 0, "actual initial exec");
        auto pid = fork(); require(pid >= 0, "fork");
        if (pid == 0) { try { resource.block(); _exit(0); } catch (...) { _exit(1); } }
        int status; waitpid(pid, &status, 0); check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child installed ACL then exited");
        check(execute(program) == 77 && execute(helper) == 77, "actual exec and embedded helper denied after process exits");
        check(execute(separate) == 0, "separate installation unaffected");
        resource.unblock();
        check(execute(program) == 0 && aclText(app) == original, "preexisting ACL restored by separate process");
    } catch (...) { try { resource.unblock(); } catch (...) {} throw; }
}
void resourceAliasesAndCounter() {
    Fixture f;
    rejects([&] { f.executor.addResource({"duplicate", "app", "/fake/Spotify.app", 501}); });
    auto text = readText(f.temp.path / "schedule.txt");
    auto end = text.find('\n'); text.replace(0, end, "BLOCKER 1 1 18446744073709551615");
    atomicWrite(f.temp.path / "schedule.txt", text);
    rejects([&] { f.executor.submit("A", Block{}); });
    check(readText(f.temp.path / "schedule.txt") == text, "request counter overflow cannot corrupt saved intent");
}
void websiteRuleSyntax() {
    Temp temp;
    auto rules = resolveWebsiteRules("localhost");
    check(!rules.addresses.empty() && rules.text.find("block drop out quick") != std::string::npos, "real resolver emits PF rules");
    auto path = temp.path / "rules.pf"; atomicWrite(path, rules.text);
    auto pid = fork(); require(pid >= 0, "fork");
    if (pid == 0) { execl("/sbin/pfctl", "pfctl", "-n", "-f", path.c_str(), nullptr); _exit(1); }
    int status; waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "native PF parser accepts actual generated rules without loading them");
    rejects([&] { resolveWebsiteRules("invalid..hostname"); });
}
void generatedJobs() {
    Temp temp;
    MacOSEnvironment environment(temp.path, "/usr/bin/true", true);
    environment.synchronizeJobs({{"spotify", true, 1893456000}}, 0);
    auto file = temp.path / "jobs/local.productivityblocker.spotify.1893456000.block.plist";
    check(fs::exists(file), "job rendered"); auto text = readText(file);
    check(text.find("StartCalendarInterval") != std::string::npos && text.find("spotify.1893456000.block") != std::string::npos,
          "job contains calendar and resolved ID");
    environment.synchronizeJobs({}, 0); check(!fs::exists(file), "obsolete job removed");
}
}
int main() {
    const std::vector<std::pair<std::string, void(*)()>> tests = {
        {"overlap and cancellation", overlapAndCancellation}, {"touching intervals", touchingAndConflicts},
        {"stale delivery and recovery", staleDeliveryAndRecovery}, {"cancelled actions", cancelledActionCannotRun},
        {"interrupted updates", interruptedUpdate}, {"persistence and validation", persistentStateAndValidation},
        {"set edits", setEditsAndRemovedIntervals}, {"randomized overlap oracle", resolverAgainstOracle},
        {"concurrent submissions", concurrentSubmissions}, {"lock release on exit", lockReleasedOnExit},
        {"disposable app ACL", disposableAppACL}, {"launchd job rendering", generatedJobs},
        {"killed updater recovery", crashAfterSave}, {"interrupted atomic write", interruptedAtomicWrite},
        {"actual exec and preexisting ACL", execAndExistingACL}, {"resource aliases and counter overflow", resourceAliasesAndCounter}, {"PF DNS and rule syntax", websiteRuleSyntax}
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
}
