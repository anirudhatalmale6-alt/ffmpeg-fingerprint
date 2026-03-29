# FFmpeg Fingerprint Plugin

Zero re-encoding fingerprint injection for live streams.
Works with `-c:v copy -c:a copy` — no transcoding, minimal CPU usage.

## Components

### 1. FFmpeg BSF: `fingerprint_inject` (SEI Injection)
Custom FFmpeg bitstream filter that injects fingerprint data as SEI NAL units
into H.264/H.265 streams. Machine-readable forensic fingerprint.

- Works with `-c:v copy` (zero re-encoding)
- Supports both H.264 and H.265
- Dynamic control via ZeroMQ
- Injected at every keyframe
- Extractable with `sei_reader` tool

### 2. `ts_fingerprint` (Visible DVB Subtitle Overlay)
Standalone MPEG-TS processor that injects DVB subtitle packets with
visible fingerprint text. Video and audio pass through untouched.

- Zero video processing
- Visible text overlay rendered by players
- 9 configurable positions
- Dynamic control via ZeroMQ
- Works with any codec (H.264, H.265, etc.)

### 3. `sei_reader` (Fingerprint Extractor)
Reads H.264/H.265 bitstream and extracts fingerprint SEI data.
Use to verify/detect who leaked a stream.

### 4. `trigger.py` (Python Controller)
Python script to control fingerprint show/hide with automatic timing.
Compatible with both BSF and subtitle modes.

---

## Quick Start

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential pkg-config libzmq3-dev

# Python
pip3 install pyzmq
```

### Build Standalone Tools

```bash
make tools
```

This creates:
- `bin/ts_fingerprint` — MPEG-TS subtitle injector
- `bin/sei_reader` — SEI fingerprint extractor

### Build FFmpeg with BSF

```bash
# Install FFmpeg build dependencies
sudo apt-get install yasm nasm libx264-dev libx265-dev \
  libfdk-aac-dev libmp3lame-dev libopus-dev libvpx-dev

# Build FFmpeg with fingerprint_inject BSF
./build_ffmpeg.sh
```

---

## Usage

### Method A: Visible Subtitle Overlay (ts_fingerprint)

```bash
# Start your stream with copy codecs, pipe through ts_fingerprint
ffmpeg -i "SOURCE_URL" \
  -c:v copy -c:a copy \
  -f mpegts pipe:1 | \
  ./bin/ts_fingerprint --zmq tcp://127.0.0.1:5556 | \
  ffmpeg -i pipe:0 -c copy -f flv rtmp://server/live/key

# Or output to file
ffmpeg -i "SOURCE_URL" \
  -c:v copy -c:a copy \
  -f mpegts pipe:1 | \
  ./bin/ts_fingerprint --zmq tcp://127.0.0.1:5556 > output.ts
```

Trigger fingerprint:
```bash
# Show for 5 minutes at random position
python3 python/trigger.py --addr tcp://127.0.0.1:5556 --mode subtitle \
  show "USER_123" --duration 300

# Show at specific position (center)
python3 python/trigger.py --addr tcp://127.0.0.1:5556 --mode subtitle \
  show "USER_123" --duration 300 --position 4

# Hide
python3 python/trigger.py --addr tcp://127.0.0.1:5556 hide

# Check status
python3 python/trigger.py --addr tcp://127.0.0.1:5556 status
```

### Method B: SEI Forensic Fingerprint (FFmpeg BSF)

```bash
# Use the custom FFmpeg binary with BSF
./ffmpeg-dist/bin/ffmpeg -i "SOURCE_URL" \
  -c:v copy -c:a copy \
  -bsf:v fingerprint_inject=zmq_addr=tcp\\://127.0.0.1\\:5555 \
  -f mpegts output.ts
```

Trigger fingerprint:
```bash
# Show fingerprint
python3 python/trigger.py --addr tcp://127.0.0.1:5555 --mode bsf \
  show "USER_123" --duration 300

# Hide
python3 python/trigger.py --addr tcp://127.0.0.1:5555 hide
```

Static mode (no ZMQ, fixed text):
```bash
./ffmpeg-dist/bin/ffmpeg -i "SOURCE_URL" \
  -c:v copy -c:a copy \
  -bsf:v fingerprint_inject=text=USER_123 \
  -f mpegts output.ts
```

### Extract/Verify Fingerprint

```bash
# Extract from H.264 stream
ffmpeg -i recorded_stream.ts -c:v copy -f h264 pipe:1 | ./bin/sei_reader

# Extract from H.265/HEVC stream
ffmpeg -i recorded_stream.ts -c:v copy -f hevc pipe:1 | ./bin/sei_reader --hevc
```

---

## Positions

```
0: top_left      1: top_center    2: top_right
3: mid_left      4: center        5: mid_right
6: bottom_left   7: bottom_center 8: bottom_right
```

## ZMQ Protocol

Both tools accept commands via ZeroMQ REQ/REP:

| Command | Description |
|---------|-------------|
| `SHOW <text>` | Activate fingerprint with text |
| `SHOW <text> <pos>` | Activate at position 0-8 (subtitle mode) |
| `HIDE` | Deactivate fingerprint |
| `TEXT <text>` | Update text (BSF mode) |
| `STATUS` | Get current state |

## Architecture

```
                    Method A (Visible)
                    ==================
Source → FFmpeg (-c:v copy -c:a copy) → pipe → ts_fingerprint → Output
                                                    ↑
                                              ZMQ (trigger.py)


                    Method B (Forensic)
                    ===================
Source → FFmpeg (-c:v copy -bsf:v fingerprint_inject) → Output
                              ↑
                        ZMQ (trigger.py)


                    Combined
                    ========
Source → FFmpeg (-c:v copy -bsf:v fingerprint_inject) → pipe → ts_fingerprint → Output
                              ↑                                       ↑
                        ZMQ :5555                               ZMQ :5556
                     (forensic SEI)                        (visible subtitle)
```

## File Structure

```
ffmpeg-fingerprint/
├── src/
│   ├── fingerprint_bsf.c    # FFmpeg BSF (SEI injection)
│   ├── ts_fingerprint.c     # MPEG-TS subtitle injector
│   └── sei_reader.c         # SEI fingerprint extractor
├── python/
│   └── trigger.py           # Python ZMQ trigger script
├── Makefile                  # Build standalone tools
├── build_ffmpeg.sh           # Build FFmpeg with BSF
└── README.md
```
