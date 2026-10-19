# pbl

A minimal native macOS app for blocking distracting websites and apps. Organize them into sets, start a timed quick block, or schedule recurring weekly blocks. Blocks stay active when the app is closed.

Built with SwiftUI and C++, with light, dark, and system appearance.

## Install

Requires Apple Silicon and macOS 14 or later.

Download and open [pbl-macos-arm64.pkg](https://github.com/varunpiram/pbl/releases/latest/download/pbl-macos-arm64.pkg), follow the macOS Installer prompts, then open pbl from Applications. Quit the app before updating. Existing sets and schedules are retained.

The installer is unsigned and the app is ad-hoc signed, not notarized. macOS may require approval in System Settings > Privacy & Security.

## Use

- **Sets:** group websites and apps, reorder sets, and add resources during an active block.
- **Quick Block:** choose a set and a duration, then start the block.
- **Schedule:** choose weekdays and time ranges that repeat each week.

Blocking continues while the app is closed and across restarts.

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

Reinstall after rebuilding the app.

To run tests:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```
