# A/B Audio Watermark System

Inaudible per-user audio fingerprint using A/B tone selection.
Works alongside burn-in mode — since the channel is already being transcoded,
audio watermarking adds almost zero extra CPU cost.

## How it works

1. FFmpeg transcodes the channel with TWO audio tracks:
   - Track A: original audio + inaudible 18.5kHz tone
   - Track B: original audio + inaudible 19.5kHz tone

2. ts_fingerprint receives the stream with both audio PIDs

3. For each user, a unique binary pattern is generated from their username/MAC
   (e.g., "01101001001011...")

4. Every N seconds (segment duration), ts_fingerprint selects either track A or B
   based on the current bit in the pattern

5. With 20 bits = 1,048,576 unique user combinations

## Detection

When someone records the stream (phone recording, screen capture, etc.):

```bash
# Analyze a recording
python3 detect.py recording.mp4 --verbose

# Match against a user database
python3 detect.py recording.mp4 --users userlist.txt
```

The detection tool extracts which tone (A or B) is present per segment,
rebuilds the binary pattern, and matches it against the user database.

## Files

- `generate_pattern.py` — Generate binary pattern from username/MAC
- `detect.py` — Analyze recordings and identify users

## Usage

### Generate pattern for a user

```bash
python3 generate_pattern.py "john123"
# Output: 01101001001011001110

python3 generate_pattern.py "AA:BB:CC:DD:EE:FF" --bits 24
# Output: 011010010010110011101001
```

### Set pattern via ZMQ

```bash
PATTERN=$(python3 generate_pattern.py "john123")
echo "AB_PATTERN $PATTERN" | zmq_send tcp://127.0.0.1:5556
```

### Set pattern via CLI

```bash
ffmpeg ... | ts_fingerprint --ab-audio --ab-pattern "01101001001011001110" | ...
```

### Full pipeline with burn-in + audio watermark

```bash
./ffmpeg_fingerprint.sh \
  -i "udp://239.1.1.1:1234" \
  --burn-in --text "USER_123" \
  --audio-watermark --ab-pattern "01101001001011001110" \
  -f mpegts pipe:1
```

## Integration with your panel

When a user connects:
1. Generate their pattern: `python3 generate_pattern.py "$USERNAME"`
2. Send to ts_fingerprint via ZMQ: `AB_PATTERN <pattern>`
3. (Also send `SHOW $USERNAME` for visual fingerprint)

When analyzing a leak:
1. Get the recording
2. Run: `python3 detect.py recording.mp4 --users /path/to/userlist.txt`
3. Tool returns the matched username and confidence score

## Configuration

- `--tone-a 18500` — Frequency for variant A (Hz, default: 18500)
- `--tone-b 19500` — Frequency for variant B (Hz, default: 19500)
- `--ab-segment-duration 4` — Seconds per A/B segment (default: 4)
- Tone volume: 0.003 (-50dB below main audio, inaudible)
