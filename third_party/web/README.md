# Bundled web libraries

Loaded from Qt resources (qrc:) by an off-the-record Qt WebEngine profile that blocks every non-local
request. Refresh them with `scripts/fetch-web-assets.sh`.

| Library | Version | License | Used for |
|---|---|---|---|
| mermaid | 12.2.0 | MIT | rendering ```mermaid blocks |
| @excalidraw/excalidraw | 0.17.6 (UMD build) | MIT | the drawing editor |
| react, react-dom | 18.3.1 (UMD) | MIT | runtime for Excalidraw |

Excalidraw 0.18 ships only ES modules with bare imports (react, jotai, radix…), which would need a bundler;
0.17.6 is the last release with a self-contained UMD build. Its scene format is the same `.excalidraw` JSON.
