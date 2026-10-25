#!/usr/bin/env bash
# Re-downloads the bundled web libraries into third_party/web (needs node/npm and network) and regenerates
# resources/web.qrc. The app itself never touches the network: these files are compiled into the binary.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
cd "$work"
npm pack --silent mermaid@12.2.0 @excalidraw/excalidraw@0.17.6 react@18.3.1 react-dom@18.3.1 @plantuml/core@1.2026.9 >/dev/null
for t in *.tgz; do mkdir -p "${t%.tgz}" && tar xzf "$t" -C "${t%.tgz}"; done

dest="$root/third_party/web"
mkdir -p "$dest/mermaid" "$dest/react" "$dest/excalidraw/excalidraw-assets" "$dest/plantuml"
cp mermaid-12.2.0/package/dist/mermaid.min.js mermaid-12.2.0/package/LICENSE "$dest/mermaid/"
cp react-18.3.1/package/umd/react.production.min.js "$dest/react/"
cp react-dom-18.3.1/package/umd/react-dom.production.min.js "$dest/react/"
cp react-18.3.1/package/LICENSE "$dest/react/LICENSE"
cp excalidraw-excalidraw-0.17.6/package/dist/excalidraw.production.min.js "$dest/excalidraw/"
cp -r excalidraw-excalidraw-0.17.6/package/dist/excalidraw-assets/. "$dest/excalidraw/excalidraw-assets/"
# PlantUML's own engine compiled to JavaScript (the MIT build) with Viz.js, Graphviz compiled to WebAssembly;
# themes, emoji and Open Iconic are loaded on first use. The stdlib bundles (AWS, Azure, C4...) are left out.
(cd plantuml-core-1.2026.9/package && cp plantuml.js viz-global.js themes.js emoji.js openiconic.js LICENSE "$dest/plantuml/")
cat > "$dest/plantuml/THIRD_PARTY.md" <<'NOTICE'
# Third-party code in viz-global.js

`viz-global.js` is Viz.js 3.24.0 (https://github.com/mdaines/viz-js), as shipped in `@plantuml/core`. It contains
Graphviz 14 (https://graphviz.org, Eclipse Public License 1.0, source at https://gitlab.com/graphviz/graphviz) and
Expat (https://libexpat.github.io, MIT) in object code form.

Viz.js license:

MIT License

Copyright (c) Michael Daines

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
NOTICE

{
  echo '<!DOCTYPE RCC><RCC version="1.0">'
  echo '<qresource prefix="/web">'
  echo '    <file alias="mermaid.html">web/mermaid.html</file>'
  echo '    <file alias="excalidraw.html">web/excalidraw.html</file>'
  echo '    <file alias="plantuml.html">web/plantuml.html</file>'
  echo '    <file alias="svg-to-png.js">web/svg-to-png.js</file>'
  (cd "$dest" && find . -type f ! -name '*.md' ! -name 'LICENSE' ! -name '*.LICENSE.txt' | sed 's|^\./||' | LC_ALL=C sort) |
    while read -r f; do echo "    <file alias=\"$f\">../third_party/web/$f</file>"; done
  echo '</qresource>'
  echo '</RCC>'
} > "$root/resources/web.qrc"
echo "updated $dest and resources/web.qrc"
