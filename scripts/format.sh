#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-write}
case "$mode" in
    write) clang_options=(-i); swift_options=(--quiet) ;;
    --check) clang_options=(--dry-run --Werror); swift_options=(--lint --quiet) ;;
    *) echo 'Usage: bash scripts/format.sh [--check]' >&2; exit 2 ;;
esac
clang-format "${clang_options[@]}" include/*.hpp include/*.h src/*.cpp bridge/*.h bridge/*.mm tests/*.cpp tests/*.mm
swiftformat ui bridge/BackendClient.swift scripts/generate-icon.swift --config .swiftformat "${swift_options[@]}"
