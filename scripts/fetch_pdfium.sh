#!/usr/bin/env bash
# Downloads the prebuilt PDFium (bblanchon/pdfium-binaries) the PDF import links against.
# Usage: scripts/fetch_pdfium.sh [win64|linux64]   (default: the current platform)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="chromium/8086"
PLATFORM="${1:-}"
if [ -z "$PLATFORM" ]; then
	case "$(uname -s)" in
		Linux*) PLATFORM=linux64 ;;
		*) PLATFORM=win64 ;;
	esac
fi
DEST="$ROOT/vendor/pdfium/$PLATFORM"
if [ -f "$DEST/include/fpdfview.h" ]; then
	echo "PDFium for $PLATFORM already present"
	exit 0
fi
ASSET=pdfium-win-x64
[ "$PLATFORM" = linux64 ] && ASSET=pdfium-linux-x64
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL -o "$TMP/pdfium.tgz" "https://github.com/bblanchon/pdfium-binaries/releases/download/$VERSION/$ASSET.tgz"
mkdir -p "$TMP/x"
tar -xzf "$TMP/pdfium.tgz" -C "$TMP/x"
mkdir -p "$ROOT/vendor/pdfium"
rm -rf "$DEST"
mv "$TMP/x" "$DEST"
echo "PDFium $VERSION installed in $DEST"
