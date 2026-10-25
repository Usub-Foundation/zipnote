#!/usr/bin/env bash
# Builds ZipNotes.app on macOS (Apple Silicon or Intel) with dependencies from Homebrew.
#
#   scripts/build-macos.sh            build + tests  ->  build-macos/ZipNotes.app
#   scripts/build-macos.sh --bundle   plus a self-contained app (Qt frameworks inside) and ZipNotes.dmg
#
# Qt comes from Homebrew unless QT_ROOT_DIR points at another Qt (the online installer's ~/Qt/6.x/macos, or
# what install-qt-action sets in CI): Homebrew no longer builds Intel bottles, and its Qt from source takes hours.
set -euo pipefail
cd "$(dirname "$0")/.."

bundle=0
[[ "${1:-}" == "--bundle" ]] && bundle=1

xcode-select -p >/dev/null 2>&1 || { echo "Install the Xcode Command Line Tools first: xcode-select --install"; exit 1; }
command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }

deps=(libsodium libzip)
[[ -n "${QT_ROOT_DIR:-}" ]] || deps+=(qt)
missing=()
# build tools that are already on PATH (Xcode's, a CI image's) are used as they are
for tool in cmake:cmake ninja:ninja pkg-config:pkgconf; do
  command -v "${tool%%:*}" >/dev/null || missing+=("${tool#*:}")
done
for d in "${deps[@]}"; do
  brew list --formula "$d" >/dev/null 2>&1 || missing+=("$d")
done
if ((${#missing[@]})); then
  echo "==> brew install ${missing[*]}"
  brew install "${missing[@]}"
fi

# Helpers for the self-contained bundle. No `cmd | grep -q` here: grep exiting early kills cmd with SIGPIPE,
# which pipefail turns into a failure.
rpaths() { otool -l "$1" | awk '$1 == "cmd" && $2 == "LC_RPATH" { getline; getline; print $2 }'; }

# install_name_tool warns on every call that the change invalidates the signature (the bundle is re-signed
# afterwards); show its output only when it fails
rewrite() {
  local out
  out=$(install_name_tool "$@" 2>&1) || { echo "$out" >&2; return 1; }
}

add_rpath() {  # add_rpath <binary> <path>, unless the binary already has it
  local existing
  existing=$(rpaths "$1")
  grep -qxF -- "$2" <<<"$existing" || rewrite -add_rpath "$2" "$1"
}

macho_files() {  # every Mach-O file inside a bundle, NUL-separated
  local f
  find "$1/Contents" -type f -print0 | while IFS= read -r -d '' f; do
    if [[ $(file -b "$f") == *Mach-O* ]]; then printf '%s\0' "$f"; fi
  done
}

deps() {  # libraries a Mach-O file loads, without its own install name
  local id
  id=$(otool -D "$1" | awk 'NR == 2')
  otool -L "$1" | awk -v id="$id" 'NR > 1 && $1 != id { print $1 }'
}

# Homebrew's Qt has absolute install names. macdeployqt turns them into @executable_path/../Frameworks/...,
# which suits ZipNotes itself, and leaves Qt WebEngine's helper process (inside QtWebEngineCore.framework)
# loading Qt straight from Homebrew. Without Homebrew the helper cannot start, and even with it the bundled
# QtWebEngineCore it loads looks for its libraries relative to the helper: Mermaid and Excalidraw never ran.
# Refer to bundled libraries through @rpath instead, give both executables an rpath to Contents/Frameworks
# and drop absolute rpaths (into Homebrew, or into the Qt the app was built against).
fix_bundle_paths() {
  local app=$1 f dep rp rel helper
  while IFS= read -r -d '' f; do
    for dep in $(deps "$f" | grep -E '^(@executable_path/../Frameworks/|/opt/|/usr/local/)' || true); do
      case $dep in
        @executable_path/*) rel=${dep#@executable_path/../Frameworks/} ;;
        *) rel=${dep#*/lib/} ;;  # /opt/homebrew/opt/qtbase/lib/QtCore.framework/... -> QtCore.framework/...
      esac
      # a Homebrew library that macdeployqt did not copy stays as it is, and check_bundle reports it
      [[ -e "$app/Contents/Frameworks/$rel" ]] || continue
      rewrite -change "$dep" "@rpath/$rel" "$f"
    done
    for rp in $(rpaths "$f" | grep '^/' || true); do
      rewrite -delete_rpath "$rp" "$f"
    done
  done < <(macho_files "$app")
  add_rpath "$app/Contents/MacOS/ZipNotes" "@executable_path/../Frameworks"
  helper=$(find "$app/Contents/Frameworks" -path '*/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess' -type f -print -quit)
  if [[ -n "$helper" ]]; then
    # Frameworks is 5 levels up through the framework's Helpers symlink, 7 through Versions/A/Helpers
    add_rpath "$helper" "@executable_path/../../../../.."
    add_rpath "$helper" "@executable_path/../../../../../../.."
  fi
}

# Fails if a binary in the bundle still loads anything outside the bundle and the system (Homebrew, the Qt it
# was built against): on the build Mac such a bundle works by accident, and breaks everywhere else.
check_bundle() {
  local app=$1 f found bad=0
  while IFS= read -r -d '' f; do
    found=$({ deps "$f" | grep -vE '^(@|/usr/lib/|/System/)'; rpaths "$f" | grep '^/'; } || true)
    if [[ -n "$found" ]]; then
      echo "  ${f#"$app"/} refers to:"
      echo "$found" | sed 's/^/    /'
      bad=1
    fi
  done < <(macho_files "$app")
  ((bad == 0)) || { echo "The bundle still refers to libraries outside it"; return 1; }
}

qt_prefix=${QT_ROOT_DIR:-$(brew --prefix qt)}
jobs=$(sysctl -n hw.ncpu)

cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$qt_prefix;$(brew --prefix)"
cmake --build build-macos -j "$jobs"
ctest --test-dir build-macos --output-on-failure

app=build-macos/ZipNotes.app
if ((bundle)); then
  # copies Qt (with WebEngine), libsodium and libzip into the bundle so it runs on Macs without Homebrew
  "$qt_prefix/bin/macdeployqt" "$app" -always-overwrite
  fix_bundle_paths "$app"
  check_bundle "$app"
  # the binaries were rewritten; Apple Silicon only runs signed code, so sign ad hoc for local use
  codesign --force --deep --sign - "$app"
  # hdiutil now and then fails with "Resource busy" while macOS is still looking at the fresh bundle
  # (seen on CI runners); a few seconds later the same command works
  for attempt in 1 2 3 4 5; do
    rm -f build-macos/ZipNotes.dmg
    if hdiutil create -volname ZipNotes -srcfolder "$app" -ov -format UDZO build-macos/ZipNotes.dmg; then
      break
    fi
    ((attempt < 5)) || { echo "hdiutil failed $attempt times"; exit 1; }
    echo "==> hdiutil failed, retrying in $((attempt * 5)) s"
    sleep $((attempt * 5))
  done
  echo "==> build-macos/ZipNotes.dmg"
fi
echo "==> open $app"
