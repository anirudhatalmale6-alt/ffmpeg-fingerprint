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

FFMPEG_VERSION="${FFMPEG_VERSION:-7.0.2}"
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
cp "$SCRIPT_DIR/src/fingerprint_bsf.c" libavcodec/

# Register the BSF in FFmpeg's build system
# 1. Add to libavcodec/Makefile
if ! grep -q "fingerprint_inject" libavcodec/Makefile; then
    echo "Registering BSF in Makefile..."
    # Add after the last BSF entry
    sed -i '/^OBJS-\$(CONFIG_VP9_SUPERFRAME_SPLIT_BSF)/a OBJS-$(CONFIG_FINGERPRINT_INJECT_BSF)        += fingerprint_bsf.o' libavcodec/Makefile
fi

# 2. Add to libavcodec/bitstream_filters.c (register the BSF)
if ! grep -q "fingerprint_inject" libavcodec/bitstream_filters.c; then
    echo "Registering BSF in bitstream_filters.c..."
    # Add extern declaration before the list
    sed -i '/extern const FFBitStreamFilter ff_vp9_superframe_split_bsf;/a extern const FFBitStreamFilter ff_fingerprint_inject_bsf;' libavcodec/bitstream_filters.c
fi

# 3. Add to allfilters.c if it exists (FFmpeg 7.x)
if [ -f "libavcodec/bsf/bsf.c" ]; then
    if ! grep -q "fingerprint_inject" libavcodec/bsf/bsf.c; then
        echo "Registering in bsf.c..."
        cp "$SCRIPT_DIR/src/fingerprint_bsf.c" libavcodec/bsf/
        sed -i '/vp9_superframe_split/a \\    &ff_fingerprint_inject_bsf,' libavcodec/bsf/bsf.c
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
echo "Usage example:"
echo "  $INSTALL_DIR/bin/ffmpeg -i input \\"
echo "    -c:v copy -c:a copy \\"
echo "    -bsf:v fingerprint_inject=zmq_addr=tcp\\\\://127.0.0.1\\\\:5555 \\"
echo "    -f mpegts output.ts"
echo ""
