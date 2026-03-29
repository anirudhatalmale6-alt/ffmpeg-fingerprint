#!/usr/bin/env python3
"""
Fingerprint Trigger Script

Compatible with both:
  1. FFmpeg BSF (fingerprint_inject) - SEI injection via ZMQ
  2. ts_fingerprint standalone tool - DVB subtitle injection via ZMQ

Usage:
  python3 trigger.py show "USERNAME_123" --duration 300
  python3 trigger.py show "USERNAME_123" --duration 300 --position 4
  python3 trigger.py hide
  python3 trigger.py status

For BSF mode (default port 5555):
  python3 trigger.py show "USER" --duration 300 --addr tcp://127.0.0.1:5555 --mode bsf

For ts_fingerprint mode (default port 5556):
  python3 trigger.py show "USER" --duration 300 --addr tcp://127.0.0.1:5556 --mode subtitle
"""

import zmq
import sys
import time
import random
import argparse

# 9 position names matching the position indices
POSITION_NAMES = {
    0: "top_left",
    1: "top_center",
    2: "top_right",
    3: "mid_left",
    4: "center",
    5: "mid_right",
    6: "bottom_left",
    7: "bottom_center",
    8: "bottom_right",
}


def send_command(addr, command, timeout=5000):
    """Send a ZMQ command and return the reply."""
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.RCVTIMEO, timeout)
    socket.setsockopt(zmq.SNDTIMEO, timeout)
    socket.connect(addr)

    try:
        socket.send_string(command)
        reply = socket.recv_string()
        return reply
    except zmq.error.Again:
        return "ERROR: timeout"
    finally:
        socket.close()
        context.term()


def cmd_show(args):
    """Show fingerprint on stream."""
    text = args.text
    duration = args.duration
    position = args.position

    if position is None:
        position = random.randint(0, 8)

    pos_name = POSITION_NAMES.get(position, f"pos_{position}")

    if args.mode == "bsf":
        # BSF mode: SHOW command
        command = f"SHOW {text}"
    else:
        # Subtitle mode: SHOW text position
        command = f"SHOW {text} {position}"

    print(f"Showing fingerprint [{pos_name}] for {duration}s: {text}")
    reply = send_command(args.addr, command)
    print(f"  -> {reply}")

    if duration > 0:
        print(f"Waiting {duration} seconds...")
        time.sleep(duration)

        # Hide after duration
        reply = send_command(args.addr, "HIDE")
        print(f"Hidden -> {reply}")


def cmd_hide(args):
    """Hide fingerprint."""
    reply = send_command(args.addr, "HIDE")
    print(f"Hidden -> {reply}")


def cmd_status(args):
    """Get current status."""
    reply = send_command(args.addr, "STATUS")
    print(f"Status: {reply}")


def main():
    parser = argparse.ArgumentParser(
        description="FFmpeg Fingerprint Trigger",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Show fingerprint for 5 minutes (random position)
  python3 trigger.py show "USER_123" --duration 300

  # Show at specific position (center)
  python3 trigger.py show "USER_123" --duration 300 --position 4

  # Show indefinitely (no auto-hide)
  python3 trigger.py show "USER_123" --duration 0

  # Hide manually
  python3 trigger.py hide

  # Check status
  python3 trigger.py status

  # Use with BSF mode
  python3 trigger.py show "USER" --duration 300 --mode bsf --addr tcp://127.0.0.1:5555

  # Use with subtitle injector
  python3 trigger.py show "USER" --duration 300 --mode subtitle --addr tcp://127.0.0.1:5556
""")

    parser.add_argument("--addr", default="tcp://127.0.0.1:5555",
                        help="ZMQ address (default: tcp://127.0.0.1:5555)")
    parser.add_argument("--mode", choices=["bsf", "subtitle"], default="subtitle",
                        help="Mode: bsf (SEI injection) or subtitle (DVB subtitle)")

    subparsers = parser.add_subparsers(dest="command", help="Command")
    subparsers.required = True

    # show command
    show_parser = subparsers.add_parser("show", help="Show fingerprint")
    show_parser.add_argument("text", help="Fingerprint text (username)")
    show_parser.add_argument("--duration", type=int, default=300,
                             help="Duration in seconds (0=indefinite, default=300)")
    show_parser.add_argument("--position", type=int, choices=range(9),
                             default=None, help="Position 0-8 (default=random)")
    show_parser.set_defaults(func=cmd_show)

    # hide command
    hide_parser = subparsers.add_parser("hide", help="Hide fingerprint")
    hide_parser.set_defaults(func=cmd_hide)

    # status command
    status_parser = subparsers.add_parser("status", help="Get status")
    status_parser.set_defaults(func=cmd_status)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
