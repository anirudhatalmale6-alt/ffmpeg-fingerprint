# Dynamic FFmpeg Fingerprinting Setup Guide (Zero Re-encoding Version)

Drop-in replacement for the original setup. Same workflow, same ZMQ control,
same Python trigger - but with ZERO re-encoding/transcoding.

---

## 1. Prerequisites (Compiling FFmpeg with Fingerprint BSF)

The custom FFmpeg binary includes the `fingerprint_inject` bitstream filter.
It must be compiled from source with our BSF module.

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get install build-essential yasm nasm pkg-config \
  libzmq3-dev libx264-dev libx265-dev libfdk-aac-dev \
  libmp3lame-dev libopus-dev libvpx-dev

# Python dependencies (same as before)
pip3 install pyzmq

# Build FFmpeg with fingerprint BSF
chmod +x build_ffmpeg.sh
./build_ffmpeg.sh

# Verify the BSF is available
./ffmpeg-dist/bin/ffmpeg -bsfs 2>/dev/null | grep fingerprint
# Should output: fingerprint_inject
```

---

## 2. Running the FFmpeg Listener (The Stream)

**OLD command (re-encodes video, uses CPU):**
```bash
ffmpeg -i "SOURCE_URL" \
  -vf "zmq=bind_address=tcp\://127.0.0.1\:5555,drawtext=fontfile='...':text='':fontsize=22:fontcolor=white:box=1:boxcolor=black@0.3:boxborderw=6:x=10:y=10" \
  -c:v libx264 -preset ultrafast -tune zerolatency -crf 28 \
  -c:a copy -f mpegts pipe:1
```

**NEW command (zero re-encoding, zero CPU):**
```bash
ffmpeg -hide_banner -loglevel error \
  -reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5 \
  -user_agent "Mozilla/5.0" \
  -i "SOURCE_URL" \
  -c:v copy -c:a copy \
  -bsf:v fingerprint_inject=zmq_addr=tcp\://127.0.0.1\:5555 \
  -map 0:v:0? -map 0:a:0? \
  -f mpegts -mpegts_copyts 1 pipe:1
```

Key differences:
- `-c:v copy` instead of `-c:v libx264` (no video encoding!)
- `-bsf:v fingerprint_inject=...` instead of `-vf "zmq,drawtext=..."`
- No `-preset`, `-tune`, `-crf` needed (nothing to encode)
- Same ZMQ address, same pipe output

Works with ALL output formats:
```bash
# MPEG-TS
-f mpegts pipe:1

# HLS
-f hls -hls_time 4 -hls_list_size 5 /path/to/stream.m3u8

# DASH
-f dash -seg_duration 4 /path/to/manifest.mpd

# RTMP
-f flv rtmp://server/live/key

# RTSP (via RTSP server)
-f rtsp rtsp://server:8554/stream

# HTTP (via pipe or direct)
-f mpegts http://server:8080/stream
```

---

## 3. The Backend Python Trigger (Drop-in Replacement)

Same usage as the original `db_trigger.py`:

```python
import zmq
import random
import sys
import time

# --- CONFIGURATION ---
ZMQ_ADDRESS = "tcp://127.0.0.1:5555"

POSITIONS = {
    "top_left": 0, "top_center": 1, "top_right": 2,
    "mid_left": 3, "center": 4, "mid_right": 5,
    "bottom_left": 6, "bottom_center": 7, "bottom_right": 8,
}

def trigger_ffmpeg_zmq(text, duration_sec):
    # 1. Pick a random position
    pos_name = random.choice(list(POSITIONS.keys()))

    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    socket.setsockopt(zmq.SNDTIMEO, 5000)
    socket.connect(ZMQ_ADDRESS)

    # --- SHOW FINGERPRINT ---
    show_cmd = f"SHOW {text}"
    print(f"Showing [{pos_name}] for {duration_sec}s for user: {text}")
    socket.send_string(show_cmd)
    socket.recv_string()

    time.sleep(duration_sec)

    # --- HIDE FINGERPRINT ---
    socket.send_string("HIDE")
    socket.recv_string()

    socket.close()
    context.term()

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 db_trigger.py <USERNAME> <DURATION_SECONDS>")
        sys.exit(1)

    username = sys.argv[1]
    duration = int(sys.argv[2])
    trigger_ffmpeg_zmq(username, duration)
```

---

## 4. How to Test Your Integration

1. Start the stream with the NEW FFmpeg command (copy codecs + BSF):
   ```bash
   ffmpeg -i "SOURCE_URL" -c:v copy -c:a copy \
     -bsf:v fingerprint_inject=zmq_addr=tcp\://127.0.0.1\:5555 \
     -f mpegts pipe:1 > output.ts
   ```

2. In another terminal, trigger the fingerprint (same as before):
   ```bash
   python3 db_trigger.py "TEST_USER_123" 300
   ```

3. The fingerprint data is now embedded in the H.264/H.265 stream as SEI data.
   The stream does NOT re-encode! Zero CPU usage for video processing.

4. Run the trigger again with a different user - it updates instantly via ZMQ.

5. After 300 seconds, the fingerprint automatically hides.

6. To verify the fingerprint was injected, use the sei_reader tool:
   ```bash
   ffmpeg -i output.ts -c:v copy -f h264 pipe:1 | ./bin/sei_reader
   # Output: [FINGERPRINT #1] TEST_USER_123
   ```

---

## 5. ZMQ Command Protocol

Same REQ/REP pattern as the original. Commands:

| Command | Description |
|---------|-------------|
| SHOW text | Activate fingerprint with text |
| HIDE | Deactivate fingerprint |
| TEXT text | Update text without changing state |
| STATUS | Get current state |

---

## 6. Static Mode (No ZMQ)

For fixed fingerprint text without dynamic control:
```bash
ffmpeg -i "SOURCE_URL" -c:v copy -c:a copy \
  -bsf:v fingerprint_inject=text=FIXED_USER_ID \
  -f mpegts pipe:1
```

---

## Comparison

| Feature | OLD (drawtext) | NEW (fingerprint_inject) |
|---------|---------------|-------------------------|
| Re-encoding | Yes (libx264) | No (-c:v copy) |
| CPU usage | High | Zero (video) |
| Visible text | Yes (burned in) | Forensic (SEI data) |
| ZMQ control | Yes | Yes (same) |
| Python trigger | Same | Same (drop-in) |
| H.264 support | Yes | Yes |
| H.265 support | Yes | Yes |
| All outputs | Yes | Yes |
| Tamper-proof | Can be cropped | Cannot be removed |
