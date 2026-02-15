#!/bin/bash
#
# build.sh - Build POSE 3.5 (Palm OS Emulator) on modern Linux x86_64
#
# Tested on: Manjaro Linux, GCC 15.2, x86_64
# Requires:  32-bit multilib (lib32-glibc, lib32-gcc-libs),
#            lib32-libx11, lib32-libxext,
#            autoconf, automake, perl, make
#
# Usage:  cd pose/ && ./build.sh
#
# Extracts source into src/, builds in-tree, output binary at:
#   src/Emulator_Src_3.5/BuildUnix/pose
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"

echo "=== POSE 3.5 Modern Linux Build ==="
echo "Working directory: $SCRIPT_DIR"
echo ""

# ---------------------------------------------------------------
# Check prerequisites
# ---------------------------------------------------------------
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "ERROR: $1 not found. $2"
        exit 1
    fi
}

check_cmd gcc    "Install base-devel (Arch) or build-essential (Debian)"
check_cmd g++    "Install base-devel (Arch) or build-essential (Debian)"
check_cmd make   "Install make"
check_cmd perl   "Install perl"
check_cmd automake "Install automake (needed for config.sub/config.guess)"
check_cmd ranlib "Install binutils"

# Check 32-bit compilation support
if ! echo 'int main(){}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
    echo "ERROR: 32-bit compilation not working. Install multilib packages:"
    echo "  Arch/Manjaro: sudo pacman -S lib32-glibc lib32-gcc-libs"
    echo "  Debian/Ubuntu: sudo apt install gcc-multilib g++-multilib"
    exit 1
fi

for lib in libX11.so libXext.so; do
    if [ ! -f "/usr/lib32/$lib" ]; then
        echo "ERROR: /usr/lib32/$lib not found."
        echo "  Arch/Manjaro: sudo pacman -S lib32-libx11 lib32-libxext"
        echo "  Debian/Ubuntu: sudo apt install libx11-dev:i386 libxext-dev:i386"
        exit 1
    fi
done

echo "Prerequisites OK."
echo ""

mkdir -p "$SRC_DIR"

# ---------------------------------------------------------------
# Phase 1: Build FLTK 1.1.10 (32-bit)
# ---------------------------------------------------------------
echo "=== Phase 1: FLTK 1.1.10 ==="

FLTK_SRC="$SRC_DIR/fltk-1.1.10"
FLTK_INSTALL="$SRC_DIR/fltk-install"

if [ -f "$FLTK_INSTALL/lib/libfltk.a" ]; then
    echo "FLTK already built, skipping."
else
    echo "Extracting fltk-1.1.10-source.tar.bz2..."
    tar xjf "$SCRIPT_DIR/sources/fltk-1.1.10-source.tar.bz2" -C "$SRC_DIR"
    chmod -R u+w "$FLTK_SRC"

    echo "Applying FLTK patches..."
    for p in "$SCRIPT_DIR"/patches/1[1-9]-fltk-*.patch; do
        [ -f "$p" ] || continue
        echo "  $(basename "$p")"
        patch -d "$FLTK_SRC" -p1 < "$p"
    done

    echo "Updating config.sub/config.guess..."
    for f in config.sub config.guess; do
        src="$(find /usr/share/automake* -name "$f" 2>/dev/null | head -1)"
        if [ -n "$src" ]; then
            cp "$src" "$FLTK_SRC/$f"
        fi
    done

    echo "Configuring FLTK..."
    cd "$FLTK_SRC"
    CC="gcc -m32 -std=gnu89" \
    CXX="g++ -m32 -std=gnu++98 -fpermissive" \
    CFLAGS="-O2 -m32" \
    CXXFLAGS="-O2 -m32" \
    LDFLAGS="-m32 -L/usr/lib32" \
    ./configure --prefix="$FLTK_INSTALL" --x-libraries=/usr/lib32 \
        --enable-localpng

    echo "Building FLTK..."
    make -j"$(nproc)"

    echo "Installing FLTK to $FLTK_INSTALL..."
    make install

    echo "FLTK done."
fi
echo ""

# ---------------------------------------------------------------
# Phase 2: Build POSE 3.5 (32-bit)
# ---------------------------------------------------------------
echo "=== Phase 2: POSE 3.5 ==="

POSE_SRC="$SRC_DIR/Emulator_Src_3.5"
POSE_BUILD="$POSE_SRC/BuildUnix"

if [ -f "$POSE_BUILD/pose" ]; then
    echo "POSE already built: $POSE_BUILD/pose"
    echo "To rebuild: make clean -C $POSE_BUILD && ./build.sh"
    exit 0
fi

echo "Extracting emulator_src_3.5.tar.gz..."
tar xzf "$SCRIPT_DIR/sources/emulator_src_3.5.tar.gz" -C "$SRC_DIR"
chmod -R u+w "$POSE_SRC"

echo "Applying distribution patches (from original src.rpm)..."
for p in "$SCRIPT_DIR"/patches/00-pose-*.patch \
         "$SCRIPT_DIR"/patches/00b-pose-*.patch; do
    [ -f "$p" ] || continue
    echo "  $(basename "$p")"
    patch -d "$POSE_SRC" -p1 < "$p"
done

echo "Applying GCC 15 / modern Linux portability patches..."
for p in "$SCRIPT_DIR"/patches/0[1-9]-pose-*.patch \
         "$SCRIPT_DIR"/patches/10-pose-*.patch; do
    [ -f "$p" ] || continue
    echo "  $(basename "$p")"
    patch -d "$POSE_SRC" -p1 < "$p"
done

cd "$POSE_BUILD"

echo "Installing pre-generated configure (autoconf 2.72 is incompatible with this configure.in)..."
cp "$SCRIPT_DIR/generated/configure" .
chmod +x configure

# Prevent Makefile auto-regeneration rules from firing
# (patching configure.in makes it newer than the generated files)
touch aclocal.m4 Makefile.in configure
touch */Makefile.in 2>/dev/null || true

echo "Updating config.sub/config.guess (originals are from 1998)..."
for f in config.sub config.guess; do
    src="$(find /usr/share/automake* -name "$f" 2>/dev/null | head -1)"
    if [ -n "$src" ]; then
        cp "$src" "$f"
    fi
done

echo "Configuring POSE..."
CC="gcc -m32 -std=gnu89" \
CXX="g++ -m32 -std=gnu++98 -fpermissive" \
CFLAGS="-O2 -m32" \
CXXFLAGS="-O2 -m32 -fpermissive -Wno-narrowing" \
LDFLAGS="-m32 -L/usr/lib32" \
./configure \
    --with-fltk="$FLTK_INSTALL" \
    --disable-gl \
    --x-libraries=/usr/lib32

echo "Generating source files..."
perl -x ../SrcShared/Strings.txt
"$FLTK_INSTALL/bin/fluid" -c ../SrcUnix/EmDlgFltkFactory.fl

echo "Applying post-generation fixes (NULL include)..."
cp "$SCRIPT_DIR/generated/ResStrings.cpp" .

echo "Building POSE..."
make -j"$(nproc)"

echo ""
echo "=== BUILD SUCCESSFUL ==="
BINARY="$POSE_BUILD/pose"
file "$BINARY"
ls -lh "$BINARY"
echo ""
echo "Run it:  $BINARY"
