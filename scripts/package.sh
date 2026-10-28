#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' ui/Info.plist)
bash scripts/prepare-local.sh
cmake --build build --target recurrence-test website-test app-test -j4
ctest --test-dir build --output-on-failure
package="pbl-macos-$(uname -m)"
mkdir -p dist
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
root="$stage/root"
mkdir -p "$root/Applications" "$root/Library/PrivilegedHelperTools" "$stage/scripts" "$stage/resources"
ditto build/pbl.app "$root/Applications/pbl.app"
setting() { sed -n "s/^#define $1 \"\(.*\)\"/\1/p" include/config.h; }
cp build/blocker-helper "$root$(setting PBL_HELPER)"
cp build/blocker-worker "$root$(setting PBL_WORKER)"
cp build/helper.requirement "$root$(setting PBL_HELPER_REQUIREMENT_FILE)"
cp include/config.h scripts/configure-install.sh "$stage/scripts/"
cat > "$stage/scripts/preinstall" <<'PRE'
#!/bin/bash
set -euo pipefail
[[ ${3:-/} == / ]] || { echo 'Install pbl on the running system volume.' >&2; exit 1; }
if [[ -e /Applications/pbl.app ]]; then
    [[ $(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' /Applications/pbl.app/Contents/Info.plist) == local.pbl ]] || exit 1
fi
script_dir=$(cd "$(dirname "$0")" && pwd)
service=$(sed -n 's/^#define PBL_SERVICE "\(.*\)"/\1/p' "$script_dir/config.h")
launchctl bootout "system/$service" 2>/dev/null || true
PRE
cat > "$stage/scripts/postinstall" <<'POST'
#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
bash "$script_dir/configure-install.sh" "$script_dir/config.h"
POST
chmod 755 "$stage/scripts/preinstall" "$stage/scripts/postinstall"
xattr -cr "$root"
pkgbuild --analyze --root "$root" "$stage/components.plist"
python3 - "$stage/components.plist" <<'PY'
import plistlib,sys
path=sys.argv[1]
with open(path,'rb') as f: components=plistlib.load(f)
for component in components:
    component['BundleIsRelocatable']=False
    component['BundleOverwriteAction']='upgrade'
with open(path,'wb') as f: plistlib.dump(components,f)
PY
pkgbuild --root "$root" --component-plist "$stage/components.plist" --scripts "$stage/scripts" \
    --identifier local.pbl.installer --version "$version" --install-location / --ownership recommended "$stage/pbl-component.pkg"
cat > "$stage/resources/welcome.html" <<'HTML'
<html><body style="font-family: -apple-system; font-size: 13px">
<h2>Install pbl</h2>
<p>Please quit pbl before continuing.</p>
<p>This installs pbl in Applications, its privileged helper and scheduling worker,
and the firewall configuration needed for website blocks. Existing schedules are retained.</p>
<p>macOS will ask for administrator authorization. No Terminal commands are needed.</p>
<p>Requires Apple Silicon and macOS 14 or later.</p>
</body></html>
HTML
cat > "$stage/resources/conclusion.html" <<'HTML'
<html><body style="font-family: -apple-system; font-size: 13px">
<h2>pbl is installed</h2><p>Open pbl from Applications.</p>
<p>Scheduled blocks continue when the app is closed.</p>
</body></html>
HTML
cat > "$stage/Distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
<title>pbl</title>
<options customize="never" require-scripts="true" rootVolumeOnly="true" hostArchitectures="arm64"/>
<domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
<volume-check><allowed-os-versions><os-version min="14.0"/></allowed-os-versions></volume-check>
<welcome file="welcome.html"/><conclusion file="conclusion.html"/>
<choices-outline><line choice="pbl"/></choices-outline>
<choice id="pbl" visible="false"><pkg-ref id="local.pbl.installer"/></choice>
<pkg-ref id="local.pbl.installer" version="$version" auth="Root">pbl-component.pkg</pkg-ref>
</installer-gui-script>
XML
productbuild --distribution "$stage/Distribution.xml" --resources "$stage/resources" --package-path "$stage" "dist/$package.pkg"
(cd dist && shasum -a 256 "$package.pkg" > "$package.pkg.sha256")
rm -f "dist/$package.zip" "dist/$package.zip.sha256"
printf 'Installer: %s/dist/%s.pkg\n' "$PWD" "$package"
