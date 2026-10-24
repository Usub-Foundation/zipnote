#!/usr/bin/env bash
# Re-downloads the bundled web libraries into third_party/web (needs node/npm and network) and regenerates
# resources/web.qrc. The app itself never touches the network: these files are compiled into the binary.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
cd "$work"
npm pack --silent mermaid@12.2.0 @excalidraw/excalidraw@0.17.6 react@18.3.1 react-dom@18.3.1 >/dev/null
for t in *.tgz; do mkdir -p "${t%.tgz}" && tar xzf "$t" -C "${t%.tgz}"; done

dest="$root/third_party/web"
mkdir -p "$dest/mermaid" "$dest/react" "$dest/excalidraw/excalidraw-assets"
cp mermaid-12.2.0/package/dist/mermaid.min.js mermaid-12.2.0/package/LICENSE "$dest/mermaid/"
cp react-18.3.1/package/umd/react.production.min.js "$dest/react/"
cp react-dom-18.3.1/package/umd/react-dom.production.min.js "$dest/react/"
cp react-18.3.1/package/LICENSE "$dest/react/LICENSE"
cp excalidraw-excalidraw-0.17.6/package/dist/excalidraw.production.min.js "$dest/excalidraw/"
cp -r excalidraw-excalidraw-0.17.6/package/dist/excalidraw-assets/. "$dest/excalidraw/excalidraw-assets/"

{
  echo '<!DOCTYPE RCC><RCC version="1.0">'
  echo '<qresource prefix="/web">'
  echo '    <file alias="mermaid.html">web/mermaid.html</file>'
  echo '    <file alias="excalidraw.html">web/excalidraw.html</file>'
  (cd "$dest" && find . -type f ! -name '*.md' ! -name 'LICENSE' ! -name '*.LICENSE.txt' | sed 's|^\./||' | sort) |
    while read -r f; do echo "    <file alias=\"$f\">../third_party/web/$f</file>"; done
  echo '</qresource>'
  echo '</RCC>'
} > "$root/resources/web.qrc"
echo "updated $dest and resources/web.qrc"
