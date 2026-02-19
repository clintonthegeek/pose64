#!/bin/bash
# Build a POSE64 AppImage
# Prerequisites: cmake, make, Qt6 dev packages, wget
# Run from the project root: ./scripts/build-appimage.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-appimage"
APPDIR="$BUILD_DIR/AppDir"

echo "=== POSE64 AppImage Builder ==="

# Clean previous build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Download tools if not present
cd "$BUILD_DIR"

if [ ! -f linuxdeploy-x86_64.AppImage ]; then
    echo "--- Downloading linuxdeploy ---"
    wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    chmod +x linuxdeploy-x86_64.AppImage
fi

if [ ! -f linuxdeploy-plugin-qt-x86_64.AppImage ]; then
    echo "--- Downloading linuxdeploy Qt plugin ---"
    wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
    chmod +x linuxdeploy-plugin-qt-x86_64.AppImage
fi

# Build POSE64
echo "--- Building POSE64 ---"
cmake "$PROJECT_ROOT" -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)

# Install into AppDir
echo "--- Installing into AppDir ---"
make install DESTDIR="$APPDIR"

# Create AppImage
echo "--- Creating AppImage ---"
export QMAKE=$(which qmake6 2>/dev/null || which qmake)
export PATH="$BUILD_DIR:$PATH"

./linuxdeploy-x86_64.AppImage \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/ca.vibekoder.pose64.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/ca.vibekoder.pose64.png" \
    --plugin qt \
    --output appimage

# Move result to project root
mv POSE64-*.AppImage "$PROJECT_ROOT/"

echo "=== Done! AppImage is in the project root ==="
ls -lh "$PROJECT_ROOT"/POSE64-*.AppImage
