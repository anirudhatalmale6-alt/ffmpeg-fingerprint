#!/usr/bin/env python3
"""
Drop-in replacement for the original db_trigger.py

Works exactly like the old script:
  python3 db_trigger.py <USERNAME> <DURATION_SECONDS>

But instead of sending drawtext reinit commands (which require re-encoding),
it sends SHOW/HIDE commands to the fingerprint_inject BSF (which works with -c:v copy).

The ZMQ address and behavior are identical to the original setup.
"""

import zmq
import random
import sys
import time

# --- CONFIGURATION ---
# Default ZMQ address - override with 3rd argument for multi-stream setups
ZMQ_ADDRESS = "tcp://127.0.0.1:5555"

# Predefined 9 Strategic Positions (Resolution Agnostic: 720p, 1080p, 4K)
POSITIONS = {
    "top_left":      0,
    "top_center":    1,
    "top_right":     2,
    "mid_left":      3,
    "center":        4,
    "mid_right":     5,
    "bottom_left":   6,
    "bottom_center": 7,
    "bottom_right":  8,
}


def trigger_ffmpeg_zmq(text, duration_sec):
    # 1. Pick a random position
    pos_name = random.choice(list(POSITIONS.keys()))
    pos_id = POSITIONS[pos_name]

    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.RCVTIMEO, 5000)
    socket.setsockopt(zmq.SNDTIMEO, 5000)
    socket.connect(ZMQ_ADDRESS)

    # --- SHOW FINGERPRINT ---
    show_cmd = f"SHOW {text}"
    print(f"Showing [{pos_name}] for {duration_sec}s for user: {text}")
    socket.send_string(show_cmd)
    reply = socket.recv_string()
    print(f"  -> {reply}")

    time.sleep(duration_sec)

    # --- HIDE FINGERPRINT ---
    hide_cmd = "HIDE"
    socket.send_string(hide_cmd)
    reply = socket.recv_string()
    print(f"Hidden -> {reply}")

    socket.close()
    context.term()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 db_trigger.py <USERNAME> <DURATION_SECONDS> [ZMQ_ADDRESS]")
        print("")
        print("Examples:")
        print("  python3 db_trigger.py USER1 300                              # Stream on port 5555")
        print("  python3 db_trigger.py USER1 300 tcp://127.0.0.1:5556        # Stream on port 5556")
        sys.exit(1)

    username = sys.argv[1]
    duration = int(sys.argv[2])

    # Optional: override ZMQ address for multi-stream setups
    if len(sys.argv) >= 4:
        ZMQ_ADDRESS = sys.argv[3]

    trigger_ffmpeg_zmq(username, duration)
