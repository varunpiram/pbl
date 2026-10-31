#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $EUID -ne 0 ]] || { echo 'Build as your normal user.' >&2; exit 1; }
cmake -S . -B build -DPBL_BUILD_APP=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build build --target pbl-app blocker-worker -j4
codesign --force --sign - --options runtime --identifier local.pbl build/pbl.app
client_hash=$(codesign -d --verbose=4 build/pbl.app 2>&1 | sed -n 's/^CDHash=//p')
[[ -n "$client_hash" ]]
cmake -S . -B build "-DPBL_CLIENT_REQUIREMENT=cdhash H\"$client_hash\""
cmake --build build --target blocker-helper -j4
codesign --force --sign - --options runtime build/blocker-helper build/blocker-worker
backend="build/pbl Blocking Service.app"
mkdir -p "$backend/Contents/MacOS"
cp assets/BackendInfo.plist "$backend/Contents/Info.plist"
cp build/blocker-helper "$backend/Contents/MacOS/local.pbl.helper"
cp build/blocker-worker "$backend/Contents/MacOS/local.productivityblocker.worker"
codesign --force --sign - --options runtime "$backend"
codesign -d --verbose=4 "$backend" 2>&1 | sed -n 's/^CDHash=\(.*\)/cdhash H"\1"/p' > build/helper.requirement
cp include/config.h build/config.h
printf 'Prepared. Install with: sudo bash "%s/scripts/install-local.sh"\n' "$PWD"
