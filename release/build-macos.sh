#!/bin/sh
# Builds dist/WarOfGenesis-<version>-macOS.zip: a universal (Apple silicon + Intel)
# "War of Genesis.app" with SDL2.framework inside and the English translation as
# a patch (no game data). Needs the game folder to build the translation patch.
#   release/build-macos.sh [GAME_DIR]
set -eu
cd "$(dirname "$0")/.."
GAME=${1:-The-War-of-Genesis_DOS_KO}
PYTHON=${PYTHON:-python3}
VERSION=$(cat VERSION)
SDL_VER=2.32.10
CACHE=release/cache
BUILD=build/macos
APP="dist/macos/War of Genesis.app"
mkdir -p "$CACHE" dist

# SDL2.framework (universal) from the official SDL release
if [ ! -d "$CACHE/SDL2.framework" ]; then
    curl -fL -o "$CACHE/SDL2-$SDL_VER.dmg" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.dmg"
    MNT=$(mktemp -d)
    hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$CACHE/SDL2-$SDL_VER.dmg" >/dev/null
    cp -R "$MNT/SDL2.framework" "$CACHE/"
    hdiutil detach "$MNT" >/dev/null
fi
FW=$(cd "$CACHE" && pwd)

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_SKIP_BUILD_RPATH=ON -DCMAKE_FIND_FRAMEWORK=FIRST -DCMAKE_FRAMEWORK_PATH="$FW" -DSDL2_DIR="$FW/SDL2.framework/Resources/CMake" \
    -DCMAKE_EXE_LINKER_FLAGS="-F$FW -Wl,-rpath,@executable_path/../Frameworks"
ninja -C "$BUILD" genesis

# English translation: rebuilt from translations/en, shipped as a patch
PYTHON=$PYTHON tools/text/build_lang.sh "$GAME" en
rm -rf "$BUILD/lang" && $PYTHON tools/text/make_patch.py "$GAME" lang/en "$BUILD/lang/en"

rm -rf dist/macos && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BUILD/genesis" "$APP/Contents/MacOS/WarOfGenesis"
strip -S "$APP/Contents/MacOS/WarOfGenesis"
cp -R "$FW/SDL2.framework" "$APP/Contents/Frameworks/"
cp -R "$BUILD/lang" "$APP/Contents/Resources/"
[ -f release/icon.icns ] && cp release/icon.icns "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>War of Genesis</string>
  <key>CFBundleDisplayName</key><string>The War of Genesis</string>
  <key>CFBundleIdentifier</key><string>io.github.acr-gd.war-of-genesis-recomp</string>
  <key>CFBundleExecutable</key><string>WarOfGenesis</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --deep --sign - "$APP"     # ad-hoc signature (required on Apple silicon)
cp release/README-player.txt "dist/macos/README.txt"
rm -f "dist/WarOfGenesis-$VERSION-macOS.zip"
(cd dist/macos && zip -qry "../WarOfGenesis-$VERSION-macOS.zip" "War of Genesis.app" README.txt)
echo "dist/WarOfGenesis-$VERSION-macOS.zip"
