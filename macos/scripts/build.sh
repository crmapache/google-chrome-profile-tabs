#!/bin/bash
# Builds "Google Chrome Profile Tabs.app" into dist.noindex and signs it.
#
# The signature should stay the same from build to build: macOS remembers Accessibility access by the
# app's signature, and with an ad-hoc signature every rebuild is a new app to it, so access has to be
# granted again. The first Apple Development certificate in the keychain is used by default; set
# SIGN_IDENTITY to use another one. Without any, the app is signed ad-hoc.
set -euo pipefail
cd "$(dirname "$0")/.."

swift build -c release --arch arm64 --arch x86_64

app="dist.noindex/Google Chrome Profile Tabs.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp .build/apple/Products/Release/GoogleChromeProfileTabs "$app/Contents/MacOS/GoogleChromeProfileTabs"
cp Resources/Info.plist "$app/Contents/Info.plist"
cp Resources/AppIcon.icns "$app/Contents/Resources/AppIcon.icns"

identity="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | sed -nE 's/.*"(Apple Development: [^"]+)".*/\1/p' | head -1)}"
if [ -z "$identity" ]; then
  echo "No Apple Development certificate found: signing ad-hoc. Accessibility access will need to be granted again after each rebuild." >&2
  identity="-"
fi
codesign --force --sign "$identity" "$app"
codesign --verify --strict "$app"
echo "Built $app"
