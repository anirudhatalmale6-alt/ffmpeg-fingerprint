# FFmpeg Fingerprint Plugin

Per-user IPTV stream fingerprinting system. Designed for Xtream Codes panels with 70-100k concurrent users. Zero video re-encoding, minimal CPU.

## Features

- **DVB 2-bit Subtitles** - visible text overlay, works on IBO Player, Set IPTV, VLC, MAG, Enigma2
- **Flash Mode** - brief black screen with user ID, works on ALL players regardless of settings
- **A/B Audio Watermark** - inaudible ultrasonic tones (18.5kHz/19.5kHz), per-user binary pattern
- **CEA-608+708 Captions** - closed captions injected into H.264 SEI NAL units
- **Burn-In Mode** - text drawn into video frames (for raw .ts or guaranteed visibility)
- **Detection GUI** - Flask web app for audio watermark detection from recordings

## Architecture

```
1 FFmpeg per CHANNEL (video passthrough, optional dual audio encode)
    |
    v
1 ts_fingerprint per USER (~5-10MB RAM, <1% CPU each)
    - DVB subtitle injection (2-bit for IPTV apps)
    - A/B audio track switching (per-user pattern)
    - Flash mode (on demand via ZMQ)
    |
    v
Output (MPEG-TS / HLS / RTMP / UDP)
```

No per-user FFmpeg encoding. Scales to 70k+ concurrent users.

## Quick Start

```bash
# Install dependencies
sudo apt-get install build-essential libzmq3-dev pkg-config
pip3 install pyzmq

# Build
make

# Run with DVB 2-bit subtitle + audio watermark
./ffmpeg_fingerprint.sh -i "udp://239.1.1.1:1234" \
  --dvb-2bit --forced --audio-watermark \
  --zmq tcp://127.0.0.1:5555 \
  -f mpegts pipe:1

# Trigger fingerprint via ZMQ
python3 python/db_trigger.py "USERNAME_123" 300 tcp://127.0.0.1:5555

# Flash mode (works on ALL players)
# Send via ZMQ: FLASH username_12345
```

## ZMQ Commands

| Command | Description |
|---------|-------------|
| `SHOW text` | Show DVB subtitle fingerprint (random position) |
| `SHOW text 3` | Show at specific position (0-8) |
| `HIDE` | Hide fingerprint |
| `FLASH text` | Flash user ID on black screen (all players) |
| `FLASH_STOP` | Stop active flash |
| `AB_PATTERN 01101001` | Set A/B audio watermark pattern |
| `AB_STATUS` | Get A/B audio watermark state |
| `STATUS` | Get fingerprint state |
| `STATS` | Stream statistics (text) |
| `STATS_JSON` | Stream statistics (JSON) |

## File Structure

```
ffmpeg-fingerprint/
├── src/
│   ├── ts_fingerprint.c     # Core: MPEG-TS fingerprint processor
│   ├── ffmpeg_fingerprint.c  # Single-command wrapper
│   ├── sei_reader.c          # SEI fingerprint extractor
│   ├── embedded_font.h       # Built-in TTF font data
│   └── stb_truetype.h        # Font rendering library
├── python/
│   ├── db_trigger.py             # Simple trigger: SHOW for N seconds
│   ├── xtream_fingerprint.py     # Xtream Codes panel integration
│   ├── source_failover.py        # Automatic source failover
│   ├── stream_monitor.py         # Stream health monitoring
│   └── fingerprint_server.py     # HTTP fingerprint server
├── audio/
│   ├── detect.py                 # Audio watermark detector (CLI)
│   ├── generate_pattern.py       # Per-user pattern generator
│   └── web/                      # Flask detection web GUI
├── hls/
│   ├── hls-fingerprint.js        # HLS.js fingerprint integration
│   └── example-server.js         # Example HLS server
├── fonts/
│   └── dash.ttf                  # Bundled font for burn-in mode
├── ffmpeg_fingerprint.sh         # Shell wrapper (recommended entry point)
├── Makefile
├── SETUP.md                      # Full documentation
└── README.md
```

## Documentation

See [SETUP.md](SETUP.md) for complete documentation including:
- All CLI options and ZMQ commands
- DVB 2-bit subtitle setup for IPTV apps
- Flash mode configuration
- A/B audio watermark with single audio track support
- Burn-in mode for guaranteed visibility
- Xtream Codes panel integration (PHP + Python)
- Source failover and stream monitoring
- Troubleshooting guide
