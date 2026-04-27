#!/usr/bin/env python3
"""
Generate A/B audio watermark pattern from username/MAC.

Usage:
    python3 generate_pattern.py "USERNAME_123"
    python3 generate_pattern.py "AA:BB:CC:DD:EE:FF" --bits 20

Outputs binary pattern string (e.g., 01101001001011...) that can be passed to:
    - ts_fingerprint --ab-pattern <pattern>
    - ZMQ command: AB_PATTERN <pattern>
"""

import hashlib
import sys
import argparse


def user_to_pattern(username, num_bits=20):
    """Generate deterministic binary pattern from username/MAC using SHA-256."""
    h = hashlib.sha256(username.encode('utf-8')).digest()
    bits = []
    for i in range(num_bits):
        byte_idx = i // 8
        bit_idx = i % 8
        if byte_idx < len(h):
            bits.append((h[byte_idx] >> bit_idx) & 1)
        else:
            bits.append(0)
    return ''.join(str(b) for b in bits)


def main():
    parser = argparse.ArgumentParser(description='Generate A/B watermark pattern')
    parser.add_argument('username', help='Username or MAC address')
    parser.add_argument('--bits', type=int, default=20,
                        help='Number of bits in pattern (default: 20)')

    args = parser.parse_args()
    pattern = user_to_pattern(args.username, args.bits)
    print(pattern)


if __name__ == '__main__':
    main()
