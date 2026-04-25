# MPEG-TS Fingerprint Injection - Complete Setup Guide

Zero re-encoding DVB subtitle fingerprint injection for IPTV streams.
Works on VLC, MAG/Infomir STBs, Enigma2 (Zgemma/Dreambox), Kodi, and all IPTV players.

---

## Quick Start

```bash
# 1. Install dependencies
sudo apt-get install build-essential libzmq3-dev pkg-config
pip3 install pyzmq

# 2. Build
make

# 3. Run (fingerprint always visible)
ffmpeg -i "SOURCE_URL" -c:v copy -c:a copy -f mpegts pipe:1 | \
  ./bin/ts_fingerprint --text "USERNAME_123" | \
  ffmpeg -i pipe:0 -c copy -f mpegts output.ts

# 4. Run with ZMQ control (show/hide on demand)
ffmpeg -i "SOURCE_URL" -c:v copy -c:a copy -f mpegts pipe:1 | \
  ./bin/ts_fingerprint --zmq tcp://127.0.0.1:5556 | \
  ffmpeg -i pipe:0 -c copy -f mpegts output.ts

# 5. Trigger fingerprint via Python
python3 python/db_trigger.py "USERNAME_123" 300 tcp://127.0.0.1:5556
```

---

## Tools Overview

| Tool | Description |
|------|-------------|
| `bin/ts_fingerprint` | Core tool: injects DVB subtitles into MPEG-TS streams |
| `bin/ffmpeg_fingerprint` | Single-command wrapper (FFmpeg + ts_fingerprint) |
| `bin/sei_reader` | Debug tool: reads SEI/subtitle data from streams |
| `python/db_trigger.py` | Simple trigger: SHOW for N seconds then HIDE |
| `python/xtream_fingerprint.py` | Xtream Codes panel integration |
| `python/source_failover.py` | Automatic source failover with priority |
| `python/stream_monitor.py` | Real-time stream health monitoring |

---

## ts_fingerprint Options

```
Fingerprint Options:
  --zmq ADDR       ZeroMQ bind address (default: tcp://127.0.0.1:5556)
  --text TEXT       Initial fingerprint text (show immediately)
  --position N     Position 0-8 (-1=random, default=-1)
  --lang CODE      Subtitle language code (default: eng)
  --display WxH    Display resolution (default: 1920x1080)
                   720x576 for SD, 1920x1080 for HD, 3840x2160 for 4K
  --fontscale N    Font scale factor 1-4 (default: auto based on display)
  --forced         Mark subtitle as hearing-impaired (auto-selects on some players)

Stream Statistics (built-in ffprobe replacement):
  --stats N        Print stream stats to stderr every N seconds (0=off)
                   Stats also available via ZMQ STATS/STATS_JSON commands
```

### Display Resolution

The `--display` option sets the DVB subtitle coordinate space:

```bash
# SD streams (720x576 or 720x480)
--display 720x576

# HD streams (default - works for most streams)
--display 1920x1080

# 4K/UHD streams
--display 3840x2160
```

Font size auto-scales: 1x for SD, 2x for HD, 3x for 4K.
Override with `--fontscale N`.

### Subtitle Language

```bash
--lang eng    # English (default)
--lang tur    # Turkish
--lang fng    # Custom code (won't auto-select on any player)
--lang und    # Undefined
```

### Random Positioning

Default behavior: fully random X,Y position across the entire screen.
Each SHOW cycle picks a new random position. Users cannot predict or block it.

Fixed positions are still available:
```
--position 0  top-left       --position 1  top-center     --position 2  top-right
--position 3  mid-left       --position 4  center         --position 5  mid-right
--position 6  bottom-left    --position 7  bottom-center  --position 8  bottom-right
```

---

## ZMQ Commands

| Command | Response | Description |
|---------|----------|-------------|
| `SHOW text` | `OK` | Show fingerprint with random position |
| `SHOW text 3` | `OK` | Show at specific position (0-8) |
| `HIDE` | `OK` | Hide fingerprint |
| `STATUS` | `active=1 text=USER pos=-1` | Get fingerprint state |
| `STATS` | `uptime=1h23m... video_codec=H.264...` | Stream stats (text) |
| `STATS_JSON` | `{"uptime_seconds":...}` | Stream stats (JSON) |

### STATS Response Fields

| Field | Description |
|-------|-------------|
| `uptime_seconds` | Time since ts_fingerprint started |
| `video_codec` | Detected video codec (H.264, H.265, MPEG-2) |
| `audio_codec` | Detected audio codec (AAC, MP3, AC3) |
| `video_pid` / `audio_pid` | Stream PID numbers |
| `total_bitrate_kbps` | Total stream bitrate |
| `video_bitrate_kbps` | Video bitrate |
| `audio_bitrate_kbps` | Audio bitrate |
| `fps` | Detected frames per second |
| `total_packets` | Total TS packets processed |
| `cc_errors` | Continuity counter errors (packet loss) |
| `idle_seconds` | Seconds since last data received |
| `stream_healthy` | true/false based on data flow |
| `fingerprint_active` | Whether fingerprint is currently shown |
| `fingerprint_text` | Current fingerprint text |

---

## VLC Auto-Display (No Manual Subtitle Selection)

DVB subtitles require the player to have subtitles enabled.
For VLC to auto-display the fingerprint without selecting it:

### Option 1: FFmpeg disposition flag (recommended)

Pipe ts_fingerprint output through FFmpeg with `-disposition:s:0 default`:

```bash
ffmpeg -i "SOURCE" -c:v copy -c:a copy -f mpegts pipe:1 | \
  ./bin/ts_fingerprint --zmq tcp://127.0.0.1:5556 --forced --lang eng | \
  ffmpeg -i pipe:0 -c copy -disposition:s:0 default -f mpegts pipe:1
```

The `-disposition:s:0 default` marks the first subtitle track as "default"
in the output. VLC and most players auto-display default subtitle tracks.
No re-encoding needed (uses `-c copy`).

### Option 2: Hearing-impaired flag

```bash
./bin/ts_fingerprint --forced --lang eng
```

The `--forced` flag sets the DVB subtitling_type to "hearing impaired"
which some players auto-select.

### Option 3: VLC settings (one-time per user)

VLC > Tools > Preferences > Subtitles/OSD:
- Set "Subtitle track" to 0
- Or set "Subtitle language" to match your --lang setting

### Option 4: VLC command-line parameter

```bash
vlc --sub-track=0 "stream_url"
```

Your IPTV panel can add this parameter when launching VLC.

---

## Enigma2 Devices (Zgemma, Dreambox)

DVB subtitles are the NATIVE subtitle format for Enigma2 devices.
These boxes are designed for DVB-S/DVB-T/DVB-C which uses exactly the
same subtitle standard. ts_fingerprint works natively on Enigma2.

### Setup for Enigma2

```bash
# Use matching language for auto-display
./bin/ts_fingerprint --lang eng --forced

# The m3u list entry just points to the stream output as normal
# Enigma2 auto-detects and displays DVB subtitles
```

### Enigma2 Subtitle Settings

On the Enigma2 box: Menu > Setup > Subtitles
- Enable DVB subtitles
- Set preferred language to match your --lang setting
- Subtitles will auto-display when fingerprint is triggered

Most Enigma2 images (OpenATV, OpenPLi, VTi) have DVB subtitle
support enabled by default.

---

## MAG/Infomir STB Compatibility

MAG boxes are fully supported. The fingerprint uses full-width bitmap
rendering which prevents MAG's subtitle centering from breaking the
position. Horizontal and vertical positioning works correctly.

MAG Portal Settings: Settings > Subtitle language > set to match --lang
This auto-enables subtitle display for all users.

---

## Source Failover

Automatic failover between multiple stream sources with priority management.

### Quick Start

```bash
python3 python/source_failover.py \
  --sources "main=http://main-src/stream,backup1=http://backup1/stream,backup2=http://backup2/stream" \
  --zmq tcp://127.0.0.1:5556 \
  --output pipe:1
```

### How It Works

1. Starts with the highest-priority (main) source
2. Monitors stream health via built-in STATS (no extra connections)
3. Detects: process death, data stall, low FPS, low bitrate, audio/video loss
4. After 3 consecutive health check failures, switches to next priority source
5. Periodically checks if main source is back online (every 30s)
6. Auto-reconnects to main source when it recovers

### Config File

```bash
python3 python/source_failover.py --config python/failover_example.json
```

Example `failover_example.json`:
```json
{
    "sources": [
        {"name": "main", "url": "http://main/stream", "priority": 0},
        {"name": "backup_eu", "url": "http://eu/stream", "priority": 1},
        {"name": "backup_us", "url": "http://us/stream", "priority": 2}
    ],
    "zmq_addr": "tcp://127.0.0.1:5556",
    "check_interval": 5,
    "fail_threshold": 3,
    "main_retry_interval": 30,
    "ts_fp_args": ["--lang", "eng", "--forced"]
}
```

### Priority System

Priority 0 = main (highest priority, always preferred)
Priority 1 = first backup
Priority 2 = second backup
...

When main source fails:
1. Try backup sources in priority order
2. Skip recently-failed sources (10s cooldown)
3. Background thread checks main source every 30s
4. When main is back, auto-switch back to main

### Failover Options

```
--check-interval N    Health check every N seconds (default: 5)
--fail-threshold N    Consecutive failures before failover (default: 3)
--main-retry N        Seconds between main source recovery checks (default: 30)
--lang CODE           Subtitle language for fingerprint
--display WxH         Display resolution
--forced              Force subtitle display
```

---

## Xtream Codes Panel Integration

### Channel Management

```bash
# Start a channel with fingerprint support
python3 python/xtream_fingerprint.py start \
  --channel 17832 --source "http://source/live/stream" \
  --lang eng --forced

# Stop a channel
python3 python/xtream_fingerprint.py stop --channel 17832

# List all running channels
python3 python/xtream_fingerprint.py list
```

### Triggering Fingerprints

```bash
# Blocking trigger (waits for duration, then hides)
python3 python/xtream_fingerprint.py trigger \
  --channel 17832 --username "test12345" --duration 300

# Non-blocking trigger (returns immediately, auto-hides in background)
python3 python/xtream_fingerprint.py trigger-async \
  --channel 17832 --username "test12345" --duration 300

# Trigger on multiple channels at once
python3 python/xtream_fingerprint.py bulk-trigger \
  --channels "17832,17833,17834" --username "test12345" --duration 300
```

### Stream Monitoring

```bash
# Get stats for a channel
python3 python/xtream_fingerprint.py stats --channel 17832

# JSON format (for API integration)
python3 python/xtream_fingerprint.py stats --channel 17832 --json
```

### ZMQ Port Assignment

Each channel gets a unique ZMQ port: `5600 + (channel_id % 20000)`

Examples:
- Channel 17832 -> port 5600 + 17832 = port 23432
- Channel 100 -> port 5700

### Backend Integration Example (Python)

```python
import subprocess

def trigger_fingerprint(channel_id, username, duration=300):
    """Trigger fingerprint from your panel backend."""
    subprocess.Popen([
        "python3", "python/xtream_fingerprint.py",
        "trigger-async",
        "--channel", str(channel_id),
        "--username", username,
        "--duration", str(duration),
    ])

def get_stream_stats(channel_id):
    """Get stream stats for panel dashboard."""
    import zmq, json
    port = 5600 + (channel_id % 20000)
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, 3000)
    sock.connect(f"tcp://127.0.0.1:{port}")
    sock.send_string("STATS_JSON")
    stats = json.loads(sock.recv_string())
    sock.close()
    ctx.term()
    return stats

# Usage from your panel's fingerprint button handler:
trigger_fingerprint(17832, "user_john_123", 300)

# Usage for stream dashboard:
stats = get_stream_stats(17832)
print(f"Bitrate: {stats['video_bitrate_kbps']}kbps, FPS: {stats['fps']}")
```

### Backend Integration Example (PHP)

```php
<?php
// Trigger fingerprint from PHP backend
function trigger_fingerprint($channel_id, $username, $duration = 300) {
    $cmd = sprintf(
        'python3 python/xtream_fingerprint.py trigger-async --channel %d --username %s --duration %d',
        intval($channel_id),
        escapeshellarg($username),
        intval($duration)
    );
    exec($cmd . ' > /dev/null 2>&1 &');
}

// Get stream stats
function get_stream_stats($channel_id) {
    $port = 5600 + ($channel_id % 20000);
    $cmd = sprintf(
        'python3 python/xtream_fingerprint.py stats --channel %d --json',
        intval($channel_id)
    );
    $output = shell_exec($cmd);
    return json_decode($output, true);
}

// When fingerprint button is pressed:
trigger_fingerprint(17832, $_GET['username'], 300);
?>
```

---

## Stream Health Monitoring

### Dashboard Mode

```bash
# Auto-discover and monitor all running channels
python3 python/stream_monitor.py --auto

# Continuous refresh every 5 seconds
python3 python/stream_monitor.py --auto --loop 5

# JSON output for API/web dashboard
python3 python/stream_monitor.py --auto --json
```

### Health Status Levels

| Status | Meaning |
|--------|---------|
| HEALTHY | Stream flowing, good FPS and bitrate |
| DEGRADED | Stream flowing but low FPS, bitrate, or CC errors |
| DOWN | No data flowing (idle > 10 seconds) |
| OFFLINE | ts_fingerprint not responding on ZMQ |

### Integration with Failover

The source_failover.py script uses the same STATS mechanism internally.
When it detects degraded/down status, it automatically switches to backup sources.

---

## Output Formats

ts_fingerprint outputs MPEG-TS to stdout. Pipe it to FFmpeg for any output format:

```bash
# MPEG-TS file
... | ./bin/ts_fingerprint ... > output.ts

# HLS
... | ./bin/ts_fingerprint ... | \
  ffmpeg -i pipe:0 -c copy -f hls -hls_time 4 /path/stream.m3u8

# RTMP
... | ./bin/ts_fingerprint ... | \
  ffmpeg -i pipe:0 -c copy -f flv rtmp://server/live/key

# HTTP push
... | ./bin/ts_fingerprint ... | \
  ffmpeg -i pipe:0 -c copy -f mpegts http://server:8080/stream

# With VLC auto-display (add disposition flag)
... | ./bin/ts_fingerprint --forced ... | \
  ffmpeg -i pipe:0 -c copy -disposition:s:0 default -f mpegts output.ts
```

---

## Building

```bash
# Install dependencies
sudo apt-get install build-essential libzmq3-dev pkg-config
pip3 install pyzmq

# Build all tools
make

# Install to /usr/local/bin (optional)
sudo make install

# Clean build
make clean
```

---

## Full Pipeline Example (Xtream Codes)

```
┌─────────────────────────────────────────────────────────────────────┐
│ Source Provider                                                       │
│ http://provider.com/live/channel1                                    │
└──────────────────────────┬──────────────────────────────────────────┘
                           │
                    ┌──────▼���─────┐
                    │   FFmpeg    │  -c:v copy -c:a copy (zero CPU)
                    │ (no encode) │
                    └──────┬──────┘
                           │ pipe (MPEG-TS)
                    ┌──────▼──────────────┐
                    │  ts_fingerprint     │  DVB subtitle injection
                    │  --zmq :5556        │  + real-time stream stats
                    │  --forced --lang eng│
                    └──────┬──────────────┘
                           │ pipe (MPEG-TS + subtitles)
                    ┌──────▼──────┐
                    │   Output    │  HLS / RTMP / MPEG-TS / ...
                    │  (FFmpeg)   │  -disposition:s:0 default
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼────┐ ┌─���──▼────┐ ┌────▼────┐
         │  VLC    │ │  MAG    │ │ Enigma2 │
         │ (auto)  │ │ (auto)  │ │ (native)│
         └───���─────┘ └─────────┘ └─────────┘
```

---

## Troubleshooting

### Fingerprint not visible
1. Check subtitles are enabled in player
2. Try `--forced --lang eng` flags
3. For VLC: add `-disposition:s:0 default` to output FFmpeg
4. For MAG: set portal subtitle language to match --lang
5. Run `./bin/ts_fingerprint --stats 5` to verify stream is flowing

### Black bar when no fingerprint
This was fixed - subtitle PID only added to PMT after first SHOW command.
If you see it, make sure you're using the latest build.

### MAG STB shows text in center only
Fixed with full-width bitmap rendering. Position is baked into the bitmap.
Verify you have the latest code (`git pull && make`).

### Stream stats show 0 FPS / 0 bitrate
Wait a few seconds after starting. Stats need initial data to calculate.
Check `STATS` via ZMQ or `--stats 5` flag.

### Enigma2 not showing subtitles
Go to Menu > Setup > Subtitles and enable DVB subtitles.
Set the subtitle language to match your --lang setting.
