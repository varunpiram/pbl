#!/bin/bash
set -euo pipefail
[[ $EUID -eq 0 ]] || exit 1
config=$1
setting() { sed -n "s/^#define $1 \"\(.*\)\"/\1/p" "$config"; }
worker=$(setting PBL_WORKER); helper=$(setting PBL_HELPER); service=$(setting PBL_SERVICE)
requirement=$(setting PBL_HELPER_REQUIREMENT_FILE)
codesign --verify --strict /Applications/pbl.app
codesign --verify --strict "$worker"
codesign --verify --strict "-R=$(cat "$requirement")" "$helper"
job="/Library/LaunchDaemons/$service.plist"
cat > "$job" <<PLIST
<?xml version="1.0"?><plist version="1.0"><dict>
<key>Label</key><string>$service</string>
<key>ProgramArguments</key><array><string>$helper</string></array>
<key>MachServices</key><dict><key>$service</key><true/></dict>
</dict></plist>
PLIST
chown root:wheel "$job"
chmod 644 "$job"
if ! grep -Fq 'anchor "productivity-blocker/*"' /etc/pf.conf; then
    [[ -e /etc/pf.conf.pbl-backup ]] || cp -p /etc/pf.conf /etc/pf.conf.pbl-backup
    printf '\nanchor "productivity-blocker/*"\n' >> /etc/pf.conf
fi
launchctl bootstrap system "$job"
"$worker" renew || printf 'Open pbl to finish setup and resolve the blocking errors above.\n' >&2
