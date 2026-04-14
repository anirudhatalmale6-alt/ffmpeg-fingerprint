#!/bin/bash
#
# ffmpeg_fingerprint - Single-command FFmpeg + DVB subtitle fingerprint overlay
#
# Combines FFmpeg (for demux/remux) with ts_fingerprint (for DVB subtitle injection)
# into one convenient command. No video re-encoding required.
#
# Usage:
#   ffmpeg_fingerprint -i SOURCE [--zmq ADDR] [--text TEXT] [--position N] [-f FMT] OUTPUT
#
# Examples:
#   # Live IPTV restream with ZMQ control:
#   ffmpeg_fingerprint -i "udp://239.1.1.1:1234" --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1
#
#   # Static text overlay to file:
#   ffmpeg_fingerprint -i input.ts --text "USERNAME" --position 0 -f mpegts output.ts
#
#   # HLS source with reconnect, output to UDP:
#   ffmpeg_fingerprint -i "https://example.com/live.m3u8" \
#     -reconnect 1 -reconnect_streamed 1 \
#     --zmq tcp://127.0.0.1:5555 \
#     -f mpegts "udp://239.1.1.2:1234?pkt_size=1316"
#
# ZMQ Commands (send to the --zmq address):
#   SHOW <text>          - Show fingerprint text
#   SHOW <text> <pos>    - Show at specific position (0-8)
#   HIDE                 - Hide fingerprint
#   STATUS               - Get current status
#
# Positions: 0=top_left 1=top_center 2=top_right
#            3=mid_left 4=center     5=mid_right
#            6=bot_left 7=bot_center 8=bot_right
#
# Copyright (c) 2026 - Custom FFmpeg Plugin

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TS_FINGERPRINT="${SCRIPT_DIR}/bin/ts_fingerprint"
FFMPEG="${FFMPEG_BIN:-ffmpeg}"

# ---- Defaults ----
ZMQ_ADDR=""
FP_TEXT=""
FP_POSITION=""
INPUT=""
OUTPUT=""
OUTPUT_FMT=""
FFMPEG_EXTRA_ARGS=()
VERBOSE=0

# ---- Cleanup on exit ----
CHILD_PIDS=()

cleanup() {
    local exit_code=$?
    # Kill all child processes
    for pid in "${CHILD_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            # Give it a moment, then force kill
            sleep 0.2
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    exit "$exit_code"
}

trap cleanup EXIT INT TERM HUP PIPE

# ---- Usage ----
usage() {
    cat >&2 <<'USAGE'
ffmpeg_fingerprint - FFmpeg + DVB subtitle fingerprint overlay

Usage:
  ffmpeg_fingerprint -i SOURCE [options] [-f FMT] OUTPUT

Fingerprint Options:
  --zmq ADDR       ZeroMQ bind address for dynamic control
                   (default: tcp://127.0.0.1:5556)
  --text TEXT      Initial/static fingerprint text
  --position N     Position 0-8 (-1=random, default=-1)

FFmpeg Options (passed through):
  -i SOURCE        Input URL/file (required)
  -f FMT           Output format (default: mpegts)
  -reconnect 1     Enable HTTP reconnect
  -reconnect_streamed 1  Reconnect on streamed input
  -reconnect_delay_max N  Max reconnect delay
  Any other FFmpeg input/output options

Output:
  pipe:1           Output to stdout
  FILE             Output to file
  udp://...        Output to UDP

Examples:
  # ZMQ-controlled live restream:
  ffmpeg_fingerprint -i "udp://239.1.1.1:1234" \
    --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1

  # Static text to file:
  ffmpeg_fingerprint -i input.ts --text "VIEWER42" -f mpegts output.ts

  # HLS with reconnect:
  ffmpeg_fingerprint -i "https://cdn.example.com/live.m3u8" \
    -reconnect 1 -reconnect_streamed 1 \
    --zmq tcp://127.0.0.1:5555 \
    -f mpegts "udp://239.1.1.2:1234?pkt_size=1316"

USAGE
    exit 1
}

# ---- Parse arguments ----
if [ $# -eq 0 ]; then
    usage
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --zmq)
            [ $# -ge 2 ] || { echo "Error: --zmq requires an address" >&2; exit 1; }
            ZMQ_ADDR="$2"
            shift 2
            ;;
        --text)
            [ $# -ge 2 ] || { echo "Error: --text requires a value" >&2; exit 1; }
            FP_TEXT="$2"
            shift 2
            ;;
        --position)
            [ $# -ge 2 ] || { echo "Error: --position requires a value" >&2; exit 1; }
            FP_POSITION="$2"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --help|-h)
            usage
            ;;
        -i)
            [ $# -ge 2 ] || { echo "Error: -i requires a source" >&2; exit 1; }
            INPUT="$2"
            shift 2
            ;;
        -f)
            [ $# -ge 2 ] || { echo "Error: -f requires a format" >&2; exit 1; }
            OUTPUT_FMT="$2"
            shift 2
            ;;
        -*)
            # Collect as FFmpeg extra arg (with its value if it looks like a flag+value pair)
            FFMPEG_EXTRA_ARGS+=("$1")
            if [ $# -ge 2 ] && [[ ! "$2" =~ ^- ]]; then
                FFMPEG_EXTRA_ARGS+=("$2")
                shift
            fi
            shift
            ;;
        *)
            # Last positional argument is the output
            OUTPUT="$1"
            shift
            ;;
    esac
done

# ---- Validate ----
if [ -z "$INPUT" ]; then
    echo "Error: -i SOURCE is required" >&2
    usage
fi

if [ -z "$OUTPUT" ]; then
    echo "Error: output destination is required (file path, pipe:1, or udp://...)" >&2
    usage
fi

# Check ts_fingerprint binary exists
if [ ! -x "$TS_FINGERPRINT" ]; then
    echo "Error: ts_fingerprint not found at $TS_FINGERPRINT" >&2
    echo "Build it first: cd $SCRIPT_DIR && make tools" >&2
    exit 1
fi

# Check ffmpeg is available
if ! command -v "$FFMPEG" &>/dev/null; then
    echo "Error: ffmpeg not found. Set FFMPEG_BIN env var or install ffmpeg." >&2
    exit 1
fi

# Default output format
if [ -z "$OUTPUT_FMT" ]; then
    OUTPUT_FMT="mpegts"
fi

# ---- Build ts_fingerprint arguments ----
TS_FP_ARGS=()

if [ -n "$ZMQ_ADDR" ]; then
    TS_FP_ARGS+=(--zmq "$ZMQ_ADDR")
fi

if [ -n "$FP_TEXT" ]; then
    TS_FP_ARGS+=(--text "$FP_TEXT")
fi

if [ -n "$FP_POSITION" ]; then
    TS_FP_ARGS+=(--position "$FP_POSITION")
fi

# ---- Build FFmpeg command ----
FFMPEG_CMD=("$FFMPEG" -hide_banner)

# Input options
if [ ${#FFMPEG_EXTRA_ARGS[@]} -gt 0 ]; then
    # Insert reconnect and other input-side options before -i
    FFMPEG_CMD+=("${FFMPEG_EXTRA_ARGS[@]}")
fi

FFMPEG_CMD+=(-i "$INPUT")
FFMPEG_CMD+=(-c:v copy -c:a copy)
FFMPEG_CMD+=(-f mpegts pipe:1)

# ---- Logging ----
if [ "$VERBOSE" -eq 1 ]; then
    echo "[ffmpeg_fingerprint] FFmpeg command: ${FFMPEG_CMD[*]}" >&2
    echo "[ffmpeg_fingerprint] ts_fingerprint args: ${TS_FP_ARGS[*]}" >&2
    echo "[ffmpeg_fingerprint] Output: $OUTPUT (format: $OUTPUT_FMT)" >&2
fi

echo "[ffmpeg_fingerprint] Starting pipeline..." >&2
echo "[ffmpeg_fingerprint]   Input:  $INPUT" >&2
echo "[ffmpeg_fingerprint]   Output: $OUTPUT" >&2
if [ -n "$ZMQ_ADDR" ]; then
    echo "[ffmpeg_fingerprint]   ZMQ:    $ZMQ_ADDR" >&2
fi
if [ -n "$FP_TEXT" ]; then
    echo "[ffmpeg_fingerprint]   Text:   $FP_TEXT" >&2
fi

# ---- Execute pipeline ----
#
# Pipeline: FFmpeg -> ts_fingerprint -> output
#
# For pipe:1 output, ts_fingerprint writes directly to stdout.
# For file output, we redirect ts_fingerprint stdout to the file.
# For network output (udp://, rtmp://), we pipe through another FFmpeg.
#

case "$OUTPUT" in
    pipe:1|-)
        # Direct stdout output
        "${FFMPEG_CMD[@]}" 2>/dev/null | \
            "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}"
        ;;
    udp://*|rtp://*|rtmp://*|rtsp://*|srt://*)
        # Network output - pipe through a second FFmpeg for proper muxing
        "${FFMPEG_CMD[@]}" 2>/dev/null | \
            "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" | \
            "$FFMPEG" -hide_banner -re -i pipe:0 -c copy -f "$OUTPUT_FMT" "$OUTPUT" 2>/dev/null
        ;;
    *)
        # File output - redirect ts_fingerprint stdout to file
        "${FFMPEG_CMD[@]}" 2>/dev/null | \
            "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" > "$OUTPUT"
        ;;
esac
