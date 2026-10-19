#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $EUID -eq 0 ]] || { echo 'Run with sudo.'; exit 1; }
build/live-website-test
bash scripts/install-local.sh
worker=/Library/PrivilegedHelperTools/local.productivityblocker.worker
"$worker" refresh
if launchctl print system/local.productivityblocker.watch >/dev/null 2>&1; then
    echo 'Network watcher is running.'
    launchctl print system/local.productivityblocker.refresh | sed -n '/event triggers/,/end event triggers/p'
    echo 'Refresh job installed; inspect network.log if a later DNS lookup fails.'
else
    echo 'No active website block: watcher and minute refresh should be absent.'
fi
printf 'Live test and installation complete. Test a scheduled website block with the UI closed.\n'
