#!/usr/bin/env bash
# Builds ZipNotes.app on macOS (Apple Silicon or Intel) with dependencies from Homebrew.
#
#   scripts/build-macos.sh            build + tests  ->  build-macos/ZipNotes.app
#   scripts/build-macos.sh --bundle   plus a self-contained app (Qt frameworks inside) and ZipNotes.dmg
set -euo pipefail
cd "$(dirname "$0")/.."

bundle=0
[[ "${1:-}" == "--bundle" ]] && bundle=1

xcode-select -p >/dev/null 2>&1 || { echo "Install the Xcode Command Line Tools first: xcode-select --install"; exit 1; }
command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }

deps=(cmake ninja pkgconf qt libsodium libzip)
missing=()
for d in "${deps[@]}"; do
  brew list --formula "$d" >/dev/null 2>&1 || missing+=("$d")
done
if ((${#missing[@]})); then
  echo "==> brew install ${missing[*]}"
  brew install "${missing[@]}"
fi

qt_prefix=$(brew --prefix qt)
jobs=$(sysctl -n hw.ncpu)

cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$qt_prefix;$(brew --prefix)"
cmake --build build-macos -j "$jobs"
ctest --test-dir build-macos --output-on-failure

app=build-macos/ZipNotes.app
if ((bundle)); then
  # copies Qt (with WebEngine), libsodium and libzip into the bundle so it runs on Macs without Homebrew
  "$qt_prefix/bin/macdeployqt" "$app" -always-overwrite
  # macdeployqt rewrites the binaries; Apple Silicon only runs signed code, so sign ad hoc for local use
  codesign --force --deep --sign - "$app"
  rm -f build-macos/ZipNotes.dmg
  hdiutil create -volname ZipNotes -srcfolder "$app" -ov -format UDZO build-macos/ZipNotes.dmg
  echo "==> build-macos/ZipNotes.dmg"
fi
echo "==> open $app"
