#!/bin/bash
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$REPO/../build/mac}
OUT=${2:-$REPO/../dist}
VERSION=${3:-$(sed -n 's/^#define XBASED_VERSION "\(.*\)"/\1/p' "$REPO/native/app/win_main.c")}
APP=$BUILD/xbased.app
if [ ! -x "$APP/Contents/MacOS/xbased" ]; then
  echo "no $APP: build the hakux target first (BUILDING.md)" >&2
  exit 1
fi
if [ ! -f "$APP/Contents/Frameworks/libMoltenVK.dylib" ]; then
  echo "$APP has no MoltenVK (tools/fetch_deps.sh fetches it; then build again)" >&2
  exit 1
fi
STAGE=$OUT/xbased-$VERSION-macos
mkdir -p "$OUT"
rm -rf "$STAGE"
mkdir -p "$STAGE/licenses"

ditto "$APP" "$STAGE/xbased.app"
strip -S "$STAGE/xbased.app/Contents/MacOS/xbased" 2>/dev/null
codesign --force --sign - --timestamp=none "$STAGE/xbased.app/Contents/Frameworks/libMoltenVK.dylib"
codesign --force --sign - --timestamp=none "$STAGE/xbased.app"
codesign --verify --strict "$STAGE/xbased.app"

README=$REPO/docs/xbased/README.md; [ -f "$README" ] || README=$REPO/README.md
cp "$README" "$REPO/COPYING" "$REPO/LICENSE" "$STAGE/"
cp "$REPO/mods/README.md" "$STAGE/MODS.md"
cp "$REPO"/licenses/* "$STAGE/licenses/"

ZIP=$OUT/xbased-$VERSION-macos.zip
rm -f "$ZIP"
(cd "$OUT" && ditto -c -k --norsrc --noextattr --keepParent "$(basename "$STAGE")" "$ZIP")
ls -la "$ZIP"
