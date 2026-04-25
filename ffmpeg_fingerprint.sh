#!/bin/bash
#
# ffmpeg_fingerprint - Single-command FFmpeg + DVB subtitle fingerprint overlay
#
# Combines FFmpeg (for demux/remux) with ts_fingerprint (for DVB subtitle injection)
# into one convenient command. No video re-encoding required.
#
# Features:
#   - DVB subtitle fingerprint injection (random or fixed position)
#   - Built-in stream stats (no ffprobe needed) via --stats or ZMQ STATS
#   - VLC auto-display via --forced and --default-sub
#   - SD/HD/4K auto-scaling
#   - Enigma2 / MAG STB / VLC / Kodi compatible
#
# Usage:
#   ffmpeg_fingerprint -i SOURCE [options] [-f FMT] OUTPUT
#
# Examples:
#   # Live IPTV restream with ZMQ control:
#   ffmpeg_fingerprint -i "udp://239.1.1.1:1234" --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1
#
#   # Static text overlay to file:
#   ffmpeg_fingerprint -i input.ts --text "USERNAME" -f mpegts output.ts
#
#   # 4K stream with forced subtitle and stats:
#   ffmpeg_fingerprint -i "http://source/stream" \
#     --text "USER" --display 3840x2160 --forced --stats 10 \
#     -f mpegts output.ts
#
#   # VLC auto-display (subtitle auto-selected):
#   ffmpeg_fingerprint -i "http://source/stream" \
#     --forced --lang eng --default-sub \
#     -f mpegts output.ts
#
#   # HLS source with reconnect, output to UDP:
#   ffmpeg_fingerprint -i "https://example.com/live.m3u8" \
#     -reconnect 1 -reconnect_streamed 1 \
#     --zmq tcp://127.0.0.1:5555 \
#     -f mpegts "udp://239.1.1.2:1234?pkt_size=1316"
#
# ZMQ Commands (send to the --zmq address):
#   SHOW <text>          - Show fingerprint text (random position)
#   SHOW <text> <pos>    - Show at specific position (0-8)
#   HIDE                 - Hide fingerprint
#   STATUS               - Get fingerprint state
#   STATS                - Get real-time stream statistics (text)
#   STATS_JSON           - Get real-time stream statistics (JSON)
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
FP_LANG=""
FP_DISPLAY=""
FP_FONTSCALE=""
FP_FORCED=0
FP_FONT=""
FP_STATS=""
FP_DEFAULT_SUB=0
INPUT=""
OUTPUT=""
OUTPUT_FMT=""
FFMPEG_EXTRA_ARGS=()
VERBOSE=0

# ---- Cleanup on exit ----
CHILD_PIDS=()

cleanup() {
    local exit_code=$?
    for pid in "${CHILD_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
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
ffmpeg_fingerprint - FFmpeg + DVB subtitle fingerprint overlay (zero re-encoding)

Usage:
  ffmpeg_fingerprint -i SOURCE [options] [-f FMT] OUTPUT

Fingerprint Options:
  --zmq ADDR       ZeroMQ bind address for dynamic control
                   (default: tcp://127.0.0.1:5556)
  --text TEXT      Initial/static fingerprint text
  --position N     Position 0-8 (-1=random, default=-1 random)
  --lang CODE      Subtitle language code (default: eng)
  --display WxH    Display resolution (default: 1920x1080)
                   720x576 for SD, 1920x1080 for HD, 3840x2160 for 4K
  --fontscale N    Font scale 1-4 (default: auto based on display)
  --font FILE      Use custom TTF font instead of built-in bitmap font
  --forced         Mark as hearing-impaired (auto-selects on some players)
  --default-sub    Add FFmpeg -disposition:s:0 default to output for VLC auto-display
  --stats N        Print stream stats to stderr every N seconds

FFmpeg Options (passed through):
  -i SOURCE        Input URL/file (required)
  -f FMT           Output format (default: mpegts)
  -reconnect 1     Enable HTTP reconnect
  -reconnect_streamed 1  Reconnect on streamed input
  Any other FFmpeg input/output options

Output:
  pipe:1           Output to stdout
  FILE             Output to file
  udp://...        Output to UDP
  rtmp://...       Output to RTMP

ZMQ Commands (send to --zmq address):
  SHOW <text>          Show fingerprint (random position)
  SHOW <text> <pos>    Show at position (0-8)
  HIDE                 Hide fingerprint
  STATUS               Get fingerprint state
  STATS                Get stream statistics (text format)
  STATS_JSON           Get stream statistics (JSON format)

Examples:
  # ZMQ-controlled live restream:
  ffmpeg_fingerprint -i "udp://239.1.1.1:1234" \
    --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1

  # Static text with VLC auto-display:
  ffmpeg_fingerprint -i input.ts --text "VIEWER42" \
    --forced --default-sub -f mpegts output.ts

  # 4K stream with monitoring:
  ffmpeg_fingerprint -i "http://source/4k" \
    --display 3840x2160 --stats 10 --forced \
    -f mpegts output.ts

  # HLS with reconnect to UDP output:
  ffmpeg_fingerprint -i "https://cdn.example.com/live.m3u8" \
    -reconnect 1 -reconnect_streamed 1 \
    --zmq tcp://127.0.0.1:5555 \
    -f mpegts "udp://239.1.1.2:1234?pkt_size=1316"

  # Stats-only mode (no fingerprint, just monitoring):
  ffmpeg_fingerprint -i "http://source/stream" \
    --stats 5 -f mpegts pipe:1

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
        --lang)
            [ $# -ge 2 ] || { echo "Error: --lang requires a code" >&2; exit 1; }
            FP_LANG="$2"
            shift 2
            ;;
        --display)
            [ $# -ge 2 ] || { echo "Error: --display requires WxH" >&2; exit 1; }
            FP_DISPLAY="$2"
            shift 2
            ;;
        --fontscale)
            [ $# -ge 2 ] || { echo "Error: --fontscale requires a number" >&2; exit 1; }
            FP_FONTSCALE="$2"
            shift 2
            ;;
        --font)
            [ $# -ge 2 ] || { echo "Error: --font requires a file path" >&2; exit 1; }
            FP_FONT="$2"
            shift 2
            ;;
        --forced)
            FP_FORCED=1
            shift
            ;;
        --default-sub)
            FP_DEFAULT_SUB=1
            shift
            ;;
        --stats)
            [ $# -ge 2 ] || { echo "Error: --stats requires interval in seconds" >&2; exit 1; }
            FP_STATS="$2"
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
            FFMPEG_EXTRA_ARGS+=("$1")
            if [ $# -ge 2 ] && [[ ! "$2" =~ ^- ]]; then
                FFMPEG_EXTRA_ARGS+=("$2")
                shift
            fi
            shift
            ;;
        *)
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

if [ ! -x "$TS_FINGERPRINT" ]; then
    echo "Error: ts_fingerprint not found at $TS_FINGERPRINT" >&2
    echo "Build it first: cd $SCRIPT_DIR && make" >&2
    exit 1
fi

if ! command -v "$FFMPEG" &>/dev/null; then
    echo "Error: ffmpeg not found. Set FFMPEG_BIN env var or install ffmpeg." >&2
    exit 1
fi

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

if [ -n "$FP_LANG" ]; then
    TS_FP_ARGS+=(--lang "$FP_LANG")
fi

if [ -n "$FP_DISPLAY" ]; then
    TS_FP_ARGS+=(--display "$FP_DISPLAY")
fi

if [ -n "$FP_FONTSCALE" ]; then
    TS_FP_ARGS+=(--fontscale "$FP_FONTSCALE")
fi

if [ -n "$FP_FONT" ]; then
    TS_FP_ARGS+=(--font "$FP_FONT")
fi

if [ "$FP_FORCED" -eq 1 ]; then
    TS_FP_ARGS+=(--forced)
fi

if [ -n "$FP_STATS" ]; then
    TS_FP_ARGS+=(--stats "$FP_STATS")
fi

# ---- Build FFmpeg input command ----
FFMPEG_CMD=("$FFMPEG" -hide_banner -loglevel error)

if [ ${#FFMPEG_EXTRA_ARGS[@]} -gt 0 ]; then
    FFMPEG_CMD+=("${FFMPEG_EXTRA_ARGS[@]}")
fi

FFMPEG_CMD+=(-i "$INPUT")
FFMPEG_CMD+=(-c:v copy -c:a copy)
FFMPEG_CMD+=(-f mpegts pipe:1)

# ---- Build output FFmpeg command (if needed) ----
OUTPUT_FFMPEG_ARGS=()
if [ "$FP_DEFAULT_SUB" -eq 1 ]; then
    OUTPUT_FFMPEG_ARGS+=(-disposition:s:0 default)
fi

# ---- Logging ----
if [ "$VERBOSE" -eq 1 ]; then
    echo "[ffmpeg_fingerprint] FFmpeg input: ${FFMPEG_CMD[*]}" >&2
    echo "[ffmpeg_fingerprint] ts_fingerprint: ${TS_FP_ARGS[*]}" >&2
    echo "[ffmpeg_fingerprint] Output: $OUTPUT (format: $OUTPUT_FMT)" >&2
    if [ "$FP_DEFAULT_SUB" -eq 1 ]; then
        echo "[ffmpeg_fingerprint] VLC auto-display: enabled (-disposition:s:0 default)" >&2
    fi
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
if [ -n "$FP_DISPLAY" ]; then
    echo "[ffmpeg_fingerprint]   Display: $FP_DISPLAY" >&2
fi
if [ "$FP_FORCED" -eq 1 ]; then
    echo "[ffmpeg_fingerprint]   Forced subtitle: yes" >&2
fi
if [ -n "$FP_STATS" ]; then
    echo "[ffmpeg_fingerprint]   Stats interval: ${FP_STATS}s" >&2
fi

# ---- Execute pipeline ----
#
# Pipeline: FFmpeg(input) -> ts_fingerprint -> [FFmpeg(output)] -> destination
#
# When --default-sub is used or output is network protocol,
# a second FFmpeg instance handles the output with disposition flags.
#

case "$OUTPUT" in
    pipe:1|-)
        if [ "$FP_DEFAULT_SUB" -eq 1 ]; then
            # Need output FFmpeg for disposition flag
            "${FFMPEG_CMD[@]}" 2>/dev/null | \
                "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" | \
                "$FFMPEG" -hide_banner -loglevel error -i pipe:0 \
                    -c copy "${OUTPUT_FFMPEG_ARGS[@]}" -f "$OUTPUT_FMT" pipe:1
        else
            "${FFMPEG_CMD[@]}" 2>/dev/null | \
                "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}"
        fi
        ;;
    udp://*|rtp://*|rtmp://*|rtsp://*|srt://*)
        # Network output - pipe through second FFmpeg for muxing
        "${FFMPEG_CMD[@]}" 2>/dev/null | \
            "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" | \
            "$FFMPEG" -hide_banner -loglevel error -re -i pipe:0 \
                -c copy "${OUTPUT_FFMPEG_ARGS[@]}" -f "$OUTPUT_FMT" "$OUTPUT" 2>/dev/null
        ;;
    *)
        if [ "$FP_DEFAULT_SUB" -eq 1 ]; then
            # Need output FFmpeg for disposition flag
            "${FFMPEG_CMD[@]}" 2>/dev/null | \
                "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" | \
                "$FFMPEG" -hide_banner -loglevel error -i pipe:0 \
                    -c copy "${OUTPUT_FFMPEG_ARGS[@]}" -f "$OUTPUT_FMT" "$OUTPUT"
        else
            # Direct file output
            "${FFMPEG_CMD[@]}" 2>/dev/null | \
                "$TS_FINGERPRINT" "${TS_FP_ARGS[@]}" > "$OUTPUT"
        fi
        ;;
esac
