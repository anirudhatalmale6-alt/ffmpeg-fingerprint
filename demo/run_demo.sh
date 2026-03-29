#!/bin/bash
#
# Demo: FFmpeg Fingerprint Plugin
#
# Generates a test video, runs it through the fingerprint pipeline,
# and shows the result.
#
# Prerequisites:
#   - ffmpeg installed
#   - ts_fingerprint built (make tools)
#   - pip3 install pyzmq
#
# Usage:
#   chmod +x demo/run_demo.sh
#   ./demo/run_demo.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$PROJECT_DIR/bin"
DEMO_DIR="$SCRIPT_DIR"

# Check dependencies
if ! command -v ffmpeg &>/dev/null; then
    echo "ERROR: ffmpeg not found. Please install ffmpeg."
    exit 1
fi

if [ ! -f "$BIN_DIR/ts_fingerprint" ]; then
    echo "Building ts_fingerprint..."
    cd "$PROJECT_DIR" && make tools
fi

echo "============================================"
echo "  FFmpeg Fingerprint Plugin - Demo"
echo "============================================"
echo ""

# Step 1: Generate a test video (color bars with timer)
echo "[1/5] Generating test video (10 seconds)..."
ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc=duration=10:size=1280x720:rate=30" \
    -f lavfi -i "sine=frequency=440:duration=10:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -crf 23 \
    -c:a aac -b:a 128k \
    -f mpegts "$DEMO_DIR/test_input.ts"
echo "  Created: demo/test_input.ts"

# Step 2: Run through ts_fingerprint with static text
echo ""
echo "[2/5] Running ts_fingerprint (visible subtitle overlay)..."
echo "  Fingerprint text: 'DEMO_USER_12345'"
echo "  Position: center (4)"

# Start ts_fingerprint in background
ffmpeg -y -hide_banner -loglevel error \
    -i "$DEMO_DIR/test_input.ts" \
    -c:v copy -c:a copy \
    -f mpegts pipe:1 | \
    "$BIN_DIR/ts_fingerprint" \
        --text "DEMO_USER_12345" \
        --position 4 \
        --zmq tcp://127.0.0.1:5560 \
    > "$DEMO_DIR/output_subtitle.ts" 2>/dev/null &

TS_PID=$!
sleep 3

# Wait for processing to complete
wait $TS_PID 2>/dev/null || true
echo "  Created: demo/output_subtitle.ts"

# Step 3: Show stream info
echo ""
echo "[3/5] Verifying output streams..."
echo "--- Input ---"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name,codec_type "$DEMO_DIR/test_input.ts" 2>/dev/null | grep -E "codec_|^\[" || true
echo ""
echo "--- Output (with subtitle) ---"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name,codec_type "$DEMO_DIR/output_subtitle.ts" 2>/dev/null | grep -E "codec_|^\[" || true

# Step 4: Extract frames for comparison
echo ""
echo "[4/5] Extracting comparison frames..."

# Original frame
ffmpeg -y -hide_banner -loglevel error \
    -i "$DEMO_DIR/test_input.ts" \
    -vf "select=eq(n\,90)" -vframes 1 \
    "$DEMO_DIR/frame_original.png"
echo "  Original frame: demo/frame_original.png"

# Output frame (with subtitle rendered if player supports it)
ffmpeg -y -hide_banner -loglevel error \
    -i "$DEMO_DIR/output_subtitle.ts" \
    -vf "select=eq(n\,90)" -vframes 1 \
    "$DEMO_DIR/frame_output.png"
echo "  Output frame: demo/frame_output.png"

# Step 5: Test SEI injection (BSF simulation - using sei_reader)
echo ""
echo "[5/5] Testing SEI reader..."
if [ -f "$BIN_DIR/sei_reader" ]; then
    echo "  Extracting raw H.264 and scanning for fingerprints..."
    ffmpeg -hide_banner -loglevel error \
        -i "$DEMO_DIR/test_input.ts" \
        -c:v copy -f h264 pipe:1 2>/dev/null | \
        timeout 5 "$BIN_DIR/sei_reader" 2>&1 || true
fi

echo ""
echo "============================================"
echo "  Demo Complete!"
echo "============================================"
echo ""
echo "Files created:"
echo "  demo/test_input.ts        - Original test video"
echo "  demo/output_subtitle.ts   - With DVB subtitle fingerprint"
echo "  demo/frame_original.png   - Frame from original"
echo "  demo/frame_output.png     - Frame from output"
echo ""
echo "The video stream was NOT re-encoded (codecs are identical)."
echo "The fingerprint was injected as a DVB subtitle stream."
echo ""
echo "To test dynamic control, run:"
echo "  # Terminal 1: Start the pipeline"
echo "  ffmpeg -i source -c:v copy -c:a copy -f mpegts pipe:1 | \\"
echo "    ./bin/ts_fingerprint --zmq tcp://127.0.0.1:5556 > output.ts"
echo ""
echo "  # Terminal 2: Trigger fingerprint"
echo "  python3 python/trigger.py --addr tcp://127.0.0.1:5556 show 'USER_123' --duration 60"
echo ""
