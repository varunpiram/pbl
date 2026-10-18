# pbl

A minimal native macOS app for blocking distracting websites and apps. Organize them into sets, start a timed quick block, or schedule recurring weekly blocks. Blocks stay active when the app is closed.

Built with SwiftUI and C++, with light, dark, and system appearance.

## Install

Requires Apple Silicon and macOS 14 or later.

Download and open `pbl-macos-arm64.pkg`, follow the macOS Installer prompts, then open Pbl from Applications. Quit the app before updating. Existing sets and schedules are retained.

The installer is unsigned and the app is ad-hoc signed, not notarized. macOS may require approval in System Settings > Privacy & Security.

## Use

- **Sets:** group websites and apps, and reorder sets.
- **Quick Block:** choose a set and a duration, then start the block.
- **Schedule:** choose weekdays and time ranges that repeat each week.

Active sets cannot be edited or deleted. Blocks continue when the app is closed. Appearance follows the system or your choice of light or dark mode.

Weekly schedules follow the system timezone. Active blocks retain their absolute deadlines. Schedules renew daily, at boot, and when the app becomes active or receives a timezone-change notification. With the app closed, timezone changes may take effect at the next renewal. Deadlines in the repeated daylight-saving hour may run late.

App blocks cover the selected installation. Website blocks use resolved IP addresses; shared addresses can affect other sites, and addresses are not continuously refreshed.

## Build

Requires CMake and Apple's command-line developer tools.

```sh
bash scripts/package.sh
```

Produces `dist/pbl-macos-arm64.pkg` and its SHA-256 checksum.

For a local build and installation:

```sh
bash scripts/prepare-local.sh
sudo bash scripts/install-local.sh
```

The app and helper authenticate using exact code-signature hashes. Install matching builds together after changes.

To run tests:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

## Structure

- `ui/`: SwiftUI views and display state.
- `bridge/`: authenticated XPC interface and privileged helper.
- `include/`, `src/`: C++ resources, actions, scheduling, storage, and macOS effects.
- `tests/`: recurrence and authentication checks.
- `scripts/`: building, packaging, installation, and icon generation.
- `assets/`: app icon and vector artwork.

The backend stores schedules in `/Library/Application Support/ProductivityBlocker/schedule.txt`. A file lock serializes updates; atomic replacement saves requests and resolved operations together. Batch submission commits starts and ends together and resolves overlapping resources. Set activity and deadlines come from the same resolved plan.

launchd invokes the worker for resolved actions and renews a fourteen-day horizon daily at 03:15 and at boot. Recovery reconciles saved state with OS effects. The UI retains user-visible names, ordering, selection, and appearance; the backend owns membership, schedules, and deadlines.

App restoration records preserve original permissions, ACLs, ownership, and flags. Website blocks use PF `block return` rules and clear connection states for blocked addresses.

To regenerate the icon:

```sh
swift scripts/generate-icon.swift assets
iconutil -c icns assets/AppIcon.iconset -o assets/AppIcon.icns
```
