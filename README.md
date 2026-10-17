# Productivity Blocker

First-pass macOS C++ backend for application and website block sets. No UI, database, XPC service, or resident monitoring loop. Submit `Block` or `Unblock` for a set, immediately or at a UTC time. The executor merges overlapping resource intervals before changing launchd jobs.

**Prototype status:** expanded automated tests pass, including real executable ACL denial and recovery after process kills. Real user-session launchd calendar testing failed on the test Mac: the calendar-event agent registered deadlines three hours later than the backend requested. Scheduled blocking is not ready for reliance. Root protection, system-domain launchd registration, PF traffic enforcement, and privileged process termination still require administrator integration tests. See [TESTING.md](TESTING.md) for the precise results and commands.

## Build and test

Requires macOS, Xcode Command Line Tools, and CMake. No third-party runtime dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Tests cover shared resources, touching intervals, cancellation, stale and duplicate execution, missed deadlines, injected interrupted updates, persistent state, invalid input, set edits, a randomized independent overlap oracle, concurrent processes, lock release after exit, disposable app ACL restoration, and job rendering. Tests run without root and do not register launchd jobs or modify PF. The ACL test modifies only a temporary bundle under `/tmp` and restores it.

## Try the CLI safely

Simulation writes only inside the specified directory. It generates inspectable launchd plists but never registers them, modifies a real app, or touches PF. Choose a future UTC minute for scheduling.

```sh
./build/blockerctl --simulate --state /tmp/blocker-demo website reddit reddit.com
./build/blockerctl --simulate --state /tmp/blocker-demo website youtube youtube.com
./build/blockerctl --simulate --state /tmp/blocker-demo set focus reddit youtube
./build/blockerctl --simulate --state /tmp/blocker-demo block focus
./build/blockerctl --simulate --state /tmp/blocker-demo unblock focus 2030-01-01T18:00Z
./build/blockerctl --simulate --state /tmp/blocker-demo status
./build/blockerctl --simulate --state /tmp/blocker-demo cancel 2
./build/blockerctl --simulate --state /tmp/blocker-demo unblock focus
```

Each submitted action prints a request ID. `cancel` removes that original request. App registration is `app ID /absolute/path/App.app UID`; reuse a resource ID across sets. Registering the same installation again under another user is rejected to prevent conflicting restoration records. `set ID RESOURCE_ID...` explicitly replaces membership and recalculates all requests for that set. Empty membership is allowed. `status` runs the recovery hook first; `recover` runs it explicitly. Mutations resolve and recover the final edited schedule rather than briefly applying the previous state first.

The internal `execute OPERATION_ID` command checks the latest due generated operation for that resource under the same lock. It does **not** run the set overlap resolver. The CLI/submission process does not need to stay alive.

## Small storage and execution model

`state/schedule.txt` uses a versioned, quoted text format implemented with the C++ standard library:

```text
BLOCKER 1 1 3
resource "reddit" website "reddit.com" 0 0 0
set "focus" 1 "reddit"
request 1 "focus" 1 1893456000
request 2 "focus" 0 1893459600
operation "reddit" 1 1893456000
operation "reddit" 0 1893459600
```

The header stores format version, simulation mode, and the next request ID. Resource and set definitions plus original requests own the requested behavior. `operation` rows are generated output in the **same atomic file replacement**. They cannot be independently edited through the API. They exist so launchd execution does not recalculate overlap. The integer action tag is the saved representation of `Block`/`Unblock`; the runtime classes have no mutable tag field.

A separate stable `schedule.lock` uses `flock`. Submissions, recovery, and execution hold it across schedule reads/changes and physical operations. No nested locks. The lock descriptor is close-on-exec; child tools do not inherit it. File replacement uses a temporary file, flush, and rename. This is intended to survive process interruption; it is not a claim of comprehensive sudden-power-loss durability.

The recovery hook reloads intent, resolves operations, saves, synchronizes jobs, and applies the current restrictions. Interrupted updates remain visible as errors. No polling recovery process: an inconsistency can persist until the next user interaction. Scheduled jobs that happen to remain installed may also execute. Never delete the state directory while apps are restricted: it contains original permissions needed for restoration.

Schedule semantics in this first pass:

- UTC absolute timestamps; future schedules use whole minutes. The CLI ignores a caller-provided TZ override when converting those instants to the machine-local calendar fields expected by launchd.
- Requests are ordered by timestamp then request ID. At an identical timestamp, the last request for the same set wins.
- Repeated Block/Unblock for an already matching set state is redundant.
- Intervals that touch are merged, so shared resources do not briefly toggle.
- Cancelling an Unblock can leave a block indefinite; cancelling a Block removes that request, not a whole interval pair.
- Current-state recovery considers all defined resources, including resources whose last request was cancelled, so prior restrictions are removed.
- Past requests are retained; no archival/compaction yet.

## Native adapters

### Applications

The app adapter captures device/inode, owner/group, mode, flags, and the full original ACL before modifying anything. It denies the selected user's search permission at the bundle root. Live mode changes that directory's owner to root and sets `UF_IMMUTABLE`. It then signals matching running processes inside that bundle. Unblock restores original metadata and only then deletes the restoration record. Retries rebuild the ACL from original metadata instead of adding duplicate entries. Replacement installations are rejected by filesystem identity checks.

Only explicit installed third-party `.app` paths are supported. No app discovery UI, future-copy prevention, external-helper handling, or updater compatibility guarantee. Root protection is experimental and may need adjustment after privileged integration tests. App-bundle privacy permissions can still prevent operations. Already running external helpers and mappings are not reliably covered.

### Websites

The PF adapter resolves the exact hostname to IPv4/IPv6 addresses and installs block rules in `productivity-blocker/RESOURCE_ID`. It removes only that resource's rules on unblock, preserving blocks from other resources, including shared addresses. It clears connection states targeting the resolved addresses after installing the rules.

Live website blocking requires PF already enabled and a reachable top-level anchor:

```pf
anchor "productivity-blocker/*"
```

The adapter checks for that anchor and refuses to silently edit the global PF configuration or enable PF. Placement and reachability must be verified against the host's existing rules, especially earlier quick rules. This repo does not include an automatic PF installer. Do not replace the system ruleset with this one-line fragment.

Address blocking has shared-host collateral effects, covers only the exact resolved hostname, and can be bypassed by VPN/proxy routing or alternate addresses. Resolution refresh occurs on block/recovery/user interactions; there is no periodic DNS refresh. PF state clearing affects all connections to those IPs, including other websites sharing an address. Browser/QUIC/VPN behavior remains untested.

### launchd

Jobs run the installed executable with `execute OPERATION_ID`; each job has a calendar trigger and `RunAtLoad`. They are generated under `/Library/LaunchDaemons` in live mode and under `state/jobs` in simulation. Job registration/removal uses `launchctl bootstrap`, `print`, and `bootout` without a shell. The backend never waits for a scheduled backend process while holding the lock.

Before touching a resource, the invocation verifies that its operation is still the latest due operation in the saved schedule. Early, obsolete, cancelled, and superseded callbacks do nothing. A repeated current operation safely reapplies the requested state. Calendar jobs have no year field; the saved absolute timestamp supplies that check. Completed jobs are cleaned up on the next interaction, not from their own process. Time-zone changes require recovery to regenerate calendar fields; this does not guarantee the system calendar-event agent uses the same timezone. Testing observed a three-hour discrepancy between generated fields and the agent's registered date. This remains unresolved; DST fallback/ambiguous calendar times also need dedicated integration testing.

Boot/load callbacks for still-installed jobs can repair their own current effects. This is not automatic full reboot recovery; `status`/`recover` reapplies all resources and PF after missed work. This matches the accepted recovery-on-next-interaction tradeoff.

## Optional manual live installation

**Not performed by the implementation/test session.** Finish the privileged integration tests before relying on live blocks. Native mode requires root and a root-owned executable whose ancestors are not writable by non-root users. The fixed live state path is `/Library/Application Support/ProductivityBlocker`. Simulation and live data cannot be mixed.

For a deliberate local experiment, an administrator can install the built CLI:

```sh
sudo install -d -o root -g wheel -m 755 /Library/PrivilegedHelperTools
sudo install -o root -g wheel -m 755 build/blockerctl /Library/PrivilegedHelperTools/productivity-blocker
sudo /Library/PrivilegedHelperTools/productivity-blocker status
```

Then register a disposable test bundle for your ordinary user's UID, create a set, and test block/unblock before selecting real apps. Do not use `sudo` with simulation unless you intentionally want root-owned simulation files. No signed installer, authorization UI, or distribution packaging is included.

## Layout

- `include/blocker.hpp`: Resource/Action/Executor interfaces and simple records.
- `src/core.cpp`: lock, atomic text storage, overlap resolution, submission, execution, recovery.
- `src/macos.cpp`: filesystem/PF adapters, launchd registration, simulation.
- `src/main.cpp`: CLI.
- `tests/tests.cpp`: focused tests and disposable ACL integration.

The small `ExecutionEnvironment` seam lets tests substitute effects and launchd registration; it is not a separate service. Later UI code can call the same Executor API once the privilege boundary is agreed.

References: [Apple filesystem ACLs and flags](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemDetails/FileSystemDetails.html), [Apple launchd scheduling](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/ScheduledJobs.html). Native command behavior should also be checked against the installed `launchd.plist(5)` and `pfctl(8)` manuals.
