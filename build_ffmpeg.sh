#!/bin/bash
#
# Build FFmpeg with the fingerprint_inject BSF
#
# This script:
# 1. Downloads FFmpeg source (if not present)
# 2. Copies the fingerprint BSF into the source tree
# 3. Registers it in the build system
# 4. Compiles FFmpeg with ZMQ and the fingerprint BSF
#
# Usage:
#   chmod +x build_ffmpeg.sh
#   ./build_ffmpeg.sh
#
# Prerequisites:
#   sudo apt-get install build-essential yasm nasm pkg-config \
#     libzmq3-dev libx264-dev libx265-dev libfdk-aac-dev \
#     libmp3lame-dev libopus-dev libvpx-dev

set -e

FFMPEG_VERSION="${FFMPEG_VERSION:-8.0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/ffmpeg-build}"
INSTALL_DIR="${INSTALL_DIR:-$SCRIPT_DIR/ffmpeg-dist}"
NPROC=$(nproc 2>/dev/null || echo 4)

echo "============================================"
echo "  FFmpeg Fingerprint BSF Builder"
echo "============================================"
echo ""
echo "FFmpeg version: $FFMPEG_VERSION"
echo "Build dir:      $BUILD_DIR"
echo "Install dir:    $INSTALL_DIR"
echo "CPU cores:      $NPROC"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download FFmpeg source if needed
if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
    echo "Downloading FFmpeg $FFMPEG_VERSION..."
    if [ ! -f "ffmpeg-$FFMPEG_VERSION.tar.bz2" ]; then
        wget "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.bz2"
    fi
    echo "Extracting..."
    tar xjf "ffmpeg-$FFMPEG_VERSION.tar.bz2"
fi

cd "ffmpeg-$FFMPEG_VERSION"

# Copy BSF source into FFmpeg source tree
echo ""
echo "Installing fingerprint_inject BSF..."

# Detect FFmpeg source tree layout
# FFmpeg 7.x+ and 8.x: BSFs live in libavcodec/bsf/ with local includes
# FFmpeg 5.x-6.x: BSFs live directly in libavcodec/
if [ -d "libavcodec/bsf" ]; then
    BSF_DIR="libavcodec/bsf"
    BSF_OBJ_PREFIX="bsf/"
    echo "Detected FFmpeg 7.x/8.x layout (libavcodec/bsf/)"

    # Create version with adjusted includes for bsf/ subdirectory
    sed -e 's|#include "libavcodec/|#include "|g' \
        "$SCRIPT_DIR/src/fingerprint_bsf.c" > "$BSF_DIR/fingerprint_bsf.c"
else
    BSF_DIR="libavcodec"
    BSF_OBJ_PREFIX=""
    echo "Detected FFmpeg 5.x/6.x layout (libavcodec/)"
    cp "$SCRIPT_DIR/src/fingerprint_bsf.c" libavcodec/
fi

# Register the BSF in FFmpeg's build system
# 1. Add to libavcodec/Makefile
if ! grep -q "fingerprint_inject" libavcodec/Makefile; then
    echo "Registering BSF in Makefile..."
    # Find the last BSF entry and add after it
    if grep -q "^OBJS-\$(CONFIG_VP9_SUPERFRAME_SPLIT_BSF)" libavcodec/Makefile; then
        sed -i "/^OBJS-\$(CONFIG_VP9_SUPERFRAME_SPLIT_BSF)/a OBJS-\$(CONFIG_FINGERPRINT_INJECT_BSF)        += ${BSF_OBJ_PREFIX}fingerprint_bsf.o" libavcodec/Makefile
    elif grep -q "^OBJS-.*_BSF)" libavcodec/Makefile; then
        # Fallback: add after the last BSF line
        LAST_BSF=$(grep -n "^OBJS-.*_BSF)" libavcodec/Makefile | tail -1 | cut -d: -f1)
        sed -i "${LAST_BSF}a OBJS-\$(CONFIG_FINGERPRINT_INJECT_BSF)        += ${BSF_OBJ_PREFIX}fingerprint_bsf.o" libavcodec/Makefile
    fi
fi

# 2. Register in bitstream_filters.c or bsf.c (depends on version)
if [ -f "libavcodec/bsf/bsf.c" ]; then
    # FFmpeg 7.x/8.x: register in bsf/bsf.c
    if ! grep -q "fingerprint_inject" libavcodec/bsf/bsf.c; then
        echo "Registering BSF in bsf/bsf.c..."
        # Add extern + list entry
        if grep -q "ff_vp9_superframe_split_bsf" libavcodec/bsf/bsf.c; then
            sed -i '/extern const FFBitStreamFilter ff_vp9_superframe_split_bsf;/a extern const FFBitStreamFilter ff_fingerprint_inject_bsf;' libavcodec/bsf/bsf.c
            sed -i '/&ff_vp9_superframe_split_bsf/a \\    \&ff_fingerprint_inject_bsf,' libavcodec/bsf/bsf.c
        else
            # Find last extern BSF and add after it
            LAST_EXTERN=$(grep -n "extern const FFBitStreamFilter" libavcodec/bsf/bsf.c | tail -1 | cut -d: -f1)
            sed -i "${LAST_EXTERN}a extern const FFBitStreamFilter ff_fingerprint_inject_bsf;" libavcodec/bsf/bsf.c
        fi
    fi
elif [ -f "libavcodec/bitstream_filters.c" ]; then
    # FFmpeg 5.x/6.x: register in bitstream_filters.c
    if ! grep -q "fingerprint_inject" libavcodec/bitstream_filters.c; then
        echo "Registering BSF in bitstream_filters.c..."
        sed -i '/extern const FFBitStreamFilter ff_vp9_superframe_split_bsf;/a extern const FFBitStreamFilter ff_fingerprint_inject_bsf;' libavcodec/bitstream_filters.c
    fi
fi

# Configure FFmpeg
echo ""
echo "Configuring FFmpeg..."
./configure \
    --prefix="$INSTALL_DIR" \
    --enable-gpl \
    --enable-nonfree \
    --enable-libzmq \
    --enable-libx264 \
    --enable-libx265 \
    --enable-libfdk-aac \
    --enable-libmp3lame \
    --enable-libopus \
    --enable-libvpx \
    --extra-libs="-lzmq -lpthread" \
    --extra-cflags="-I/usr/include" \
    --extra-ldflags="-L/usr/lib"

# Build
echo ""
echo "Building FFmpeg (using $NPROC cores)..."
make -j"$NPROC"

# Install
echo ""
echo "Installing to $INSTALL_DIR..."
make install

echo ""
echo "============================================"
echo "  Build Complete!"
echo "============================================"
echo ""
echo "FFmpeg binary: $INSTALL_DIR/bin/ffmpeg"
echo ""
echo "Verify the BSF is available:"
echo "  $INSTALL_DIR/bin/ffmpeg -bsfs 2>/dev/null | grep fingerprint"
echo ""
echo "NOTE: The BSF plugin (fingerprint_inject) embeds data in H.264 SEI NALUs."
echo "For VISIBLE DVB subtitle fingerprinting, use the standalone tools instead:"
echo ""
echo "  # Build standalone tools (ts_fingerprint, sei_reader, etc):"
echo "  cd $SCRIPT_DIR && make"
echo ""
echo "  # Single-command wrapper (recommended):"
echo "  ./ffmpeg_fingerprint.sh -i SOURCE --text USERNAME --forced -f mpegts output.ts"
echo ""
echo "  # Or manual pipeline:"
echo "  ffmpeg -i SOURCE -c:v copy -c:a copy -f mpegts pipe:1 | \\"
echo "    ./bin/ts_fingerprint --text USERNAME --forced --lang eng --stats 10 | \\"
echo "    ffmpeg -i pipe:0 -c copy -disposition:s:0 default -f mpegts output.ts"
echo ""
echo "Full documentation: see SETUP.md"
echo ""
