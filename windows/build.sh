#!/bin/bash
# Builds dist/GoogleChromeProfileTabs.exe with MinGW-w64: on macOS or Linux as a cross build
# (brew install mingw-w64, apt install mingw-w64), on Windows from an MSYS2 MinGW shell.
set -euo pipefail
cd "$(dirname "$0")"

prefix=x86_64-w64-mingw32-
command -v ${prefix}g++ >/dev/null || prefix=
mkdir -p build dist
${prefix}windres res/app.rc -O coff -o build/app.res
${prefix}g++ -std=c++17 -O2 -Wall -municode -mwindows -static -s \
  src/main.cpp src/strip.cpp src/chrome.cpp src/settings.cpp build/app.res \
  -o dist/GoogleChromeProfileTabs.exe \
  -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lcomctl32 -lshell32 -lole32 -loleaut32 -lpropsys -luuid -luxtheme
echo "Built dist/GoogleChromeProfileTabs.exe"
