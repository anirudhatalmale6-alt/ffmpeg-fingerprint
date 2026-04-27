#!/usr/bin/env python3
"""
Audio Watermark Detection Tool

Analyzes a recording (video/audio file, phone capture, screen recording)
to extract the A/B audio watermark pattern and identify the user.

Usage:
    python3 detect.py recording.mp4
    python3 detect.py recording.mp3 --users users.txt
    python3 detect.py recording.wav --tone-a 18500 --tone-b 19500

The tool:
1. Extracts audio from the input file (uses ffmpeg)
2. Runs FFT analysis to detect watermark tones per segment
3. Builds binary pattern from A/B tone presence
4. Matches pattern against user database (if provided)
"""

import sys
import os
import json
import hashlib
import struct
import argparse
import subprocess
import tempfile

try:
    import numpy as np
except ImportError:
    print("Error: numpy required. Install with: pip3 install numpy")
    sys.exit(1)

TONE_A_HZ = 18500
TONE_B_HZ = 19500
SEGMENT_DURATION = 4.0
SAMPLE_RATE = 48000
NUM_BITS = 20
DETECTION_BANDWIDTH = 200


def extract_audio(input_file, sample_rate=48000):
    """Extract audio as raw PCM float32 mono using ffmpeg."""
    with tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as tmp:
        tmp_path = tmp.name

    cmd = [
        'ffmpeg', '-hide_banner', '-loglevel', 'error',
        '-i', input_file,
        '-ac', '1',
        '-ar', str(sample_rate),
        '-f', 'f32le',
        '-y', tmp_path
    ]

    try:
        subprocess.run(cmd, check=True, capture_output=True)
        data = np.fromfile(tmp_path, dtype=np.float32)
        return data
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


def detect_tone(samples, target_freq, sample_rate, bandwidth=DETECTION_BANDWIDTH):
    """Detect presence and strength of a specific frequency using Goertzel algorithm."""
    if len(samples) < 256:
        return 0.0

    n = len(samples)
    k = int(0.5 + n * target_freq / sample_rate)
    w = 2.0 * np.pi * k / n
    coeff = 2.0 * np.cos(w)

    s0 = 0.0
    s1 = 0.0
    s2 = 0.0

    for sample in samples:
        s0 = sample + coeff * s1 - s2
        s2 = s1
        s1 = s0

    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    magnitude = np.sqrt(abs(power)) / n

    total_power = np.sqrt(np.mean(samples ** 2))
    if total_power < 1e-10:
        return 0.0

    return magnitude / total_power


def detect_tone_fft(samples, target_freq, sample_rate, bandwidth=DETECTION_BANDWIDTH):
    """Detect tone presence using FFT (more robust for noisy recordings)."""
    if len(samples) < 1024:
        return 0.0

    window = np.hanning(len(samples))
    windowed = samples * window

    fft_result = np.fft.rfft(windowed)
    fft_magnitude = np.abs(fft_result)

    freqs = np.fft.rfftfreq(len(samples), 1.0 / sample_rate)

    target_mask = (freqs >= target_freq - bandwidth / 2) & (freqs <= target_freq + bandwidth / 2)
    if not np.any(target_mask):
        return 0.0

    target_power = np.max(fft_magnitude[target_mask])

    noise_mask = (freqs >= target_freq - bandwidth * 3) & (freqs <= target_freq + bandwidth * 3) & ~target_mask
    if np.any(noise_mask):
        noise_floor = np.median(fft_magnitude[noise_mask])
    else:
        noise_floor = np.median(fft_magnitude)

    if noise_floor < 1e-10:
        return 0.0

    return target_power / noise_floor


def analyze_recording(audio_data, sample_rate, tone_a_hz, tone_b_hz,
                      segment_duration, num_bits):
    """Analyze recording to extract A/B pattern per segment."""
    samples_per_segment = int(segment_duration * sample_rate)
    total_segments = len(audio_data) // samples_per_segment

    if total_segments < num_bits:
        print(f"Warning: recording has {total_segments} segments, need {num_bits} for full pattern")
        print(f"         Recording length: {len(audio_data)/sample_rate:.1f}s, need {num_bits * segment_duration:.0f}s")

    results = []

    for i in range(min(total_segments, num_bits * 3)):
        start = i * samples_per_segment
        end = start + samples_per_segment
        segment = audio_data[start:end]

        strength_a = detect_tone_fft(segment, tone_a_hz, sample_rate)
        strength_b = detect_tone_fft(segment, tone_b_hz, sample_rate)

        if strength_a > 2.0 or strength_b > 2.0:
            bit = 0 if strength_a > strength_b else 1
            confidence = max(strength_a, strength_b) / (min(strength_a, strength_b) + 0.001)
        else:
            bit = -1
            confidence = 0.0

        results.append({
            'segment': i,
            'tone_a': round(strength_a, 2),
            'tone_b': round(strength_b, 2),
            'bit': bit,
            'confidence': round(confidence, 2)
        })

    return results


def extract_pattern(results, num_bits):
    """Extract binary pattern from analysis results."""
    pattern = []
    for r in results[:num_bits]:
        if r['bit'] >= 0:
            pattern.append(r['bit'])
        else:
            pattern.append(-1)
    return pattern


def pattern_to_string(pattern):
    """Convert pattern to binary string (? for unknown bits)."""
    return ''.join(str(b) if b >= 0 else '?' for b in pattern)


def user_to_pattern(username, num_bits=NUM_BITS):
    """Generate deterministic A/B pattern from username/MAC (same algo as C code)."""
    h = hashlib.sha256(username.encode('utf-8')).digest()
    bits = []
    for i in range(num_bits):
        byte_idx = i // 8
        bit_idx = i % 8
        if byte_idx < len(h):
            bits.append((h[byte_idx] >> bit_idx) & 1)
        else:
            bits.append(0)
    return bits


def match_user(detected_pattern, users_file=None, users_list=None):
    """Match detected pattern against user database."""
    users = []

    if users_file:
        with open(users_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    users.append(line)
    elif users_list:
        users = users_list

    if not users:
        return None

    best_match = None
    best_score = -1

    for username in users:
        expected = user_to_pattern(username, len(detected_pattern))
        matches = 0
        total = 0

        for d, e in zip(detected_pattern, expected):
            if d >= 0:
                total += 1
                if d == e:
                    matches += 1

        if total > 0:
            score = matches / total
            if score > best_score:
                best_score = score
                best_match = username

    return {'username': best_match, 'confidence': round(best_score * 100, 1)} if best_match else None


def main():
    parser = argparse.ArgumentParser(description='Audio Watermark Detection Tool')
    parser.add_argument('input', help='Input recording file (video/audio)')
    parser.add_argument('--users', help='User list file (one username/MAC per line)')
    parser.add_argument('--tone-a', type=float, default=TONE_A_HZ,
                        help=f'Frequency of tone A in Hz (default: {TONE_A_HZ})')
    parser.add_argument('--tone-b', type=float, default=TONE_B_HZ,
                        help=f'Frequency of tone B in Hz (default: {TONE_B_HZ})')
    parser.add_argument('--segment', type=float, default=SEGMENT_DURATION,
                        help=f'Segment duration in seconds (default: {SEGMENT_DURATION})')
    parser.add_argument('--bits', type=int, default=NUM_BITS,
                        help=f'Number of bits in pattern (default: {NUM_BITS})')
    parser.add_argument('--json', action='store_true', help='Output results as JSON')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show per-segment analysis')

    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: file not found: {args.input}")
        sys.exit(1)

    print(f"Extracting audio from: {args.input}")
    audio_data = extract_audio(args.input, SAMPLE_RATE)
    duration = len(audio_data) / SAMPLE_RATE
    print(f"Audio duration: {duration:.1f}s ({len(audio_data)} samples at {SAMPLE_RATE}Hz)")

    print(f"Analyzing watermark (tone A={args.tone_a}Hz, tone B={args.tone_b}Hz)...")
    results = analyze_recording(audio_data, SAMPLE_RATE, args.tone_a, args.tone_b,
                                args.segment, args.bits)

    pattern = extract_pattern(results, args.bits)
    pattern_str = pattern_to_string(pattern)

    detected_bits = sum(1 for b in pattern if b >= 0)
    total_bits = len(pattern)

    if args.verbose:
        print("\nPer-segment analysis:")
        print(f"{'Seg':>4} {'Tone A':>8} {'Tone B':>8} {'Bit':>5} {'Conf':>8}")
        print("-" * 40)
        for r in results:
            bit_str = str(r['bit']) if r['bit'] >= 0 else '?'
            print(f"{r['segment']:>4} {r['tone_a']:>8.2f} {r['tone_b']:>8.2f} {bit_str:>5} {r['confidence']:>8.2f}")

    print(f"\nDetected pattern: {pattern_str}")
    print(f"Bits detected: {detected_bits}/{total_bits}")

    match = None
    if args.users:
        match = match_user(pattern, users_file=args.users)
        if match:
            print(f"\nMATCH FOUND: {match['username']} (confidence: {match['confidence']}%)")
        else:
            print("\nNo match found in user database")

    if args.json:
        output = {
            'input_file': args.input,
            'duration': round(duration, 1),
            'pattern': pattern_str,
            'bits_detected': detected_bits,
            'total_bits': total_bits,
            'segments': results,
            'match': match
        }
        print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
