# Verification results

Tested on macOS 15.7.3, Apple Clang 15, arm64. This records actual coverage rather than claiming every native adapter is verified.

## Passed

- Debug and Release builds and all 17 backend test groups.
- AddressSanitizer plus UndefinedBehaviorSanitizer build and the same suite, with no reported findings.
- Randomized overlap checking against an independent per-set oracle (100 schedules, 14 timestamps each).
- Six concurrent processes submitting 60 requests: no lost updates or duplicate request IDs.
- Real SIGKILL after the schedule save and before job registration: lock released and recovery reapplied saved intent.
- SIGKILL during atomic writing: readers saw a complete old or new file.
- Actual exec of a disposable app and embedded helper denied after blocking; a separate copy remained executable.
- Pre-existing ACL restored exactly, including after the blocking process exited; replacement inode rejected.
- Duplicate, cancelled, early, and late operation handling; overlap cancellation and set membership changes.
- Native DNS resolution and PF's non-mutating parser accept the actual generated localhost IPv4/IPv6 rules. This does not test network enforcement.
- CLI validation, immediate operations, simulation state, and generated plist syntax with spaces and XML-special characters in paths.
- launchd RunAtLoad callbacks registered and exited successfully without prematurely blocking a future resource.
- Real user-session launchd jobs explicitly kickstarted at block/unblock deadlines changed simulated resource state correctly after the submitting process exited. Both jobs exited with status 0; duplicate and stale callbacks behaved correctly; all temporary jobs were removed. This isolates dispatch from the failed automatic calendar trigger.

## Failed or incomplete

**Full calendar dispatch failed.** For a generated October 16, 2026, 22:24 local-calendar job, `launchctl print` showed Month=10, Day=16, Hour=22, Minute=24. The system log from UserEventAgent said it registered the callback for October 17 at 01:24 Eastern. The host's current timezone was America/New_York. Earlier deadlines showed the same three-hour discrepancy. This is evidence of a timezone disagreement, not proof of its underlying cause. No system timezone, Apple service, or hard-coded offset was changed to make the test pass.

Root-owned/immutable bundle protection, privileged termination, system-domain launchd job registration, and PF packet/state behavior are **not verified**: `sudo -n true` reported that a password is required. No global firewall or installed application was modified. Browser, VPN, DNS-change, sleep/reboot, and updater coverage also remain outstanding.

The user-session dispatch test uses simulated effects. It cannot establish system-domain permissions or native app/PF correctness.

## Bugs found and fixed

1. The same app could be registered under different user IDs, allowing separate restoration journals to conflict. Duplicate targets/known filesystem identities are now rejected regardless of user ID.
2. Exhausting the request ID counter could wrap it and save a schedule that could no longer be loaded. Submission now fails before modifying the saved schedule.

The unresolved calendar discrepancy is a release blocker for scheduled execution on this host.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 tests/integration.py build/blockerctl
```

The calendar integration test installs uniquely named temporary LaunchAgents with simulation payloads, then removes them in a finally block. It takes several minutes and is expected to expose the calendar discrepancy on the affected host:

```sh
python3 tests/integration.py build/blockerctl --launchd
```

To isolate real launchd execution from calendar-event delivery, the following test explicitly kickstarts those real jobs at their deadlines. Passing this is not a calendar test:

```sh
python3 tests/integration.py build/blockerctl --dispatch-only
```

Sanitizers:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize -j 4
ctest --test-dir build-sanitize --output-on-failure
```

## Administrator-only fixture test

Built but not run by the agent. Run this yourself in Terminal; enter the administrator password only in Terminal:

```sh
sudo ./build/privileged_app_test "$(id -u)"
```

It creates its own bundle and executable under `/private/tmp`, starts the fixture as the selected ordinary user, applies real root ownership and immutable protection, checks user tampering and process termination, restores metadata, and removes the fixture. It does not install the backend, register launchd jobs, modify PF, or target existing apps. If restoration fails, it retains the fixture and journal and reports their path. Share the output before treating root protection as verified.

Live PF traffic and system-domain installation tests still need a deliberate administrator session and isolated setup; this fixture test does not cover them.
