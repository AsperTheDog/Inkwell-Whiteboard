#!/usr/bin/env bash
# Downloads the prebuilt LGPL shared FFmpeg (BtbN builds) the video player links against.
# Usage: scripts/fetch_ffmpeg.sh [win64|linux64]   (default: the current platform)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="8.1"
PLATFORM="${1:-}"
if [ -z "$PLATFORM" ]; then
	case "$(uname -s)" in
		Linux*) PLATFORM=linux64 ;;
		*) PLATFORM=win64 ;;
	esac
fi
DEST="$ROOT/vendor/ffmpeg/$PLATFORM"
if [ -f "$DEST/include/libavcodec/avcodec.h" ]; then
	echo "FFmpeg for $PLATFORM already present"
	exit 0
fi
EXT=zip
[ "$PLATFORM" = linux64 ] && EXT=tar.xz
NAME="ffmpeg-n${VERSION}-latest-${PLATFORM}-lgpl-shared-${VERSION}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL -o "$TMP/ffmpeg.$EXT" "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$NAME.$EXT"
mkdir -p "$TMP/x"
if [ "$EXT" = zip ]; then unzip -q "$TMP/ffmpeg.$EXT" -d "$TMP/x"; else tar -xf "$TMP/ffmpeg.$EXT" -C "$TMP/x"; fi
mkdir -p "$ROOT/vendor/ffmpeg"
rm -rf "$DEST"
mv "$TMP/x/$NAME" "$DEST"
echo "FFmpeg $VERSION installed in $DEST"
