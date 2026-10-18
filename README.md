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

Timezone changes may take effect later while the app is closed. Blocks ending during the repeated daylight-saving hour may finish late.

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

Reinstall after rebuilding the app.

To run tests:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

## Formatting

Use clang-format 22 and SwiftFormat 0.61.1 with the repository's checked-in settings:

```sh
bash scripts/format.sh
bash scripts/format.sh --check
```
