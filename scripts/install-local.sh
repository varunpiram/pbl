#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
payload=${1:-"$script_dir/../build"}
[[ ! -f "$script_dir/config.h" ]] || payload=$script_dir
cd "$payload"
[[ $EUID -eq 0 ]] || { echo 'Run this installer with sudo.' >&2; exit 1; }
setting() { sed -n "s/^#define $1 \"\(.*\)\"/\1/p" config.h; }
backend=$(setting PBL_BACKEND_BUNDLE); service=$(setting PBL_SERVICE)
requirement=$(setting PBL_HELPER_REQUIREMENT_FILE)
codesign --verify --strict pbl.app
codesign --verify --strict --deep 'pbl Blocking Service.app'
expected=$(cat helper.requirement)
codesign --verify "-R=$expected" 'pbl Blocking Service.app'
if [[ -e /Applications/pbl.app ]]; then
    [[ $(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' /Applications/pbl.app/Contents/Info.plist) == local.pbl ]] || { echo 'Another app occupies /Applications/pbl.app.' >&2; exit 1; }
fi
install -d -o root -g wheel -m 755 /Library/PrivilegedHelperTools
launchctl bootout "system/$service" 2>/dev/null || true
ditto 'pbl Blocking Service.app' "$backend"
chown -R root:wheel "$backend"
install -o root -g wheel -m 644 helper.requirement "$requirement"
rm -rf /Applications/pbl.app
ditto pbl.app /Applications/pbl.app
chown -R root:wheel /Applications/pbl.app
bash "$script_dir/configure-install.sh" "$PWD/config.h"
printf 'Installed. Open /Applications/pbl.app\n'
