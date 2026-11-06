#!/bin/bash
# Builds the app, puts it into /Applications in place of the previous version and starts it.
set -euo pipefail
cd "$(dirname "$0")/.."

scripts/build.sh

target="/Applications/Google Chrome Profile Tabs.app"
if pgrep -xq GoogleChromeProfileTabs; then
  pkill -x GoogleChromeProfileTabs || true
  for _ in $(seq 1 50); do pgrep -xq GoogleChromeProfileTabs || break; sleep 0.1; done
fi
rm -rf "$target"
cp -R "dist.noindex/Google Chrome Profile Tabs.app" "$target"
open "$target"
echo "Installed and started $target"
