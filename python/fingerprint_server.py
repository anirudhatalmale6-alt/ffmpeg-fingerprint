#!/usr/bin/env python3
"""
FFmpeg Fingerprint Server

Universal fingerprint management server that works with all output formats.
Handles both BSF (SEI injection) and subtitle overlay modes.

Runs as a background service that manages fingerprint state and
communicates with FFmpeg processes via ZeroMQ.

Usage:
  python3 fingerprint_server.py --bsf-port 5555 --sub-port 5556

API Endpoints (ZMQ REQ/REP):
  SHOW <user_id> [duration_sec] [position]  - Show fingerprint
  HIDE                                       - Hide fingerprint
  STATUS                                     - Get current state
  SHOW_TIMED <user_id> <duration_sec>       - Show with auto-hide

The server manages timing internally so the caller doesn't need to sleep.
"""

import zmq
import threading
import time
import random
import argparse
import json
import sys
import signal
from datetime import datetime

POSITIONS = {
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


class FingerprintState:
    """Thread-safe fingerprint state."""

    def __init__(self):
        self.lock = threading.Lock()
        self.active = False
        self.text = ""
        self.position = -1  # -1 = random
        self.expire_time = 0  # Unix timestamp when to auto-hide (0=never)
        self.show_count = 0

    def show(self, text, duration=0, position=-1):
        with self.lock:
            self.text = text
            self.active = True
            self.position = position if 0 <= position <= 8 else random.randint(0, 8)
            self.expire_time = time.time() + duration if duration > 0 else 0
            self.show_count += 1
            return self.position

    def hide(self):
        with self.lock:
            self.active = False
            self.text = ""
            self.expire_time = 0

    def check_expiry(self):
        with self.lock:
            if self.active and self.expire_time > 0 and time.time() >= self.expire_time:
                self.active = False
                self.text = ""
                self.expire_time = 0
                return True
            return False

    def get_state(self):
        with self.lock:
            remaining = 0
            if self.active and self.expire_time > 0:
                remaining = max(0, int(self.expire_time - time.time()))
            return {
                "active": self.active,
                "text": self.text,
                "position": self.position,
                "position_name": POSITIONS.get(self.position, "unknown"),
                "remaining_seconds": remaining,
                "show_count": self.show_count,
            }


class FingerprintForwarder:
    """Forwards fingerprint state changes to BSF and subtitle ZMQ endpoints."""

    def __init__(self, state, bsf_port, sub_port):
        self.state = state
        self.bsf_addr = f"tcp://127.0.0.1:{bsf_port}" if bsf_port else None
        self.sub_addr = f"tcp://127.0.0.1:{sub_port}" if sub_port else None
        self.context = zmq.Context()

    def forward_show(self, text, position):
        """Forward SHOW to BSF and subtitle endpoints."""
        # Forward to BSF (SEI injection)
        if self.bsf_addr:
            try:
                sock = self.context.socket(zmq.REQ)
                sock.setsockopt(zmq.RCVTIMEO, 1000)
                sock.setsockopt(zmq.SNDTIMEO, 1000)
                sock.connect(self.bsf_addr)
                sock.send_string(f"SHOW {text}")
                sock.recv_string()
                sock.close()
            except Exception as e:
                print(f"[WARN] BSF forward failed: {e}", file=sys.stderr)

        # Forward to subtitle injector
        if self.sub_addr:
            try:
                sock = self.context.socket(zmq.REQ)
                sock.setsockopt(zmq.RCVTIMEO, 1000)
                sock.setsockopt(zmq.SNDTIMEO, 1000)
                sock.connect(self.sub_addr)
                sock.send_string(f"SHOW {text} {position}")
                sock.recv_string()
                sock.close()
            except Exception as e:
                print(f"[WARN] Subtitle forward failed: {e}", file=sys.stderr)

    def forward_hide(self):
        """Forward HIDE to all endpoints."""
        for addr in [self.bsf_addr, self.sub_addr]:
            if not addr:
                continue
            try:
                sock = self.context.socket(zmq.REQ)
                sock.setsockopt(zmq.RCVTIMEO, 1000)
                sock.setsockopt(zmq.SNDTIMEO, 1000)
                sock.connect(addr)
                sock.send_string("HIDE")
                sock.recv_string()
                sock.close()
            except Exception as e:
                print(f"[WARN] Forward HIDE failed: {e}", file=sys.stderr)


def expiry_checker(state, forwarder):
    """Background thread that checks for expired fingerprints."""
    while True:
        if state.check_expiry():
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Auto-hide (expired)",
                  file=sys.stderr)
            forwarder.forward_hide()
        time.sleep(1)


def handle_command(msg, state, forwarder):
    """Process a command and return reply string."""
    parts = msg.strip().split()
    if not parts:
        return "ERR empty command"

    cmd = parts[0].upper()

    if cmd == "SHOW" and len(parts) >= 2:
        text = parts[1]
        duration = int(parts[2]) if len(parts) > 2 else 0
        position = int(parts[3]) if len(parts) > 3 else -1

        pos = state.show(text, duration, position)
        forwarder.forward_show(text, pos)

        pos_name = POSITIONS.get(pos, f"pos_{pos}")
        dur_str = f" for {duration}s" if duration > 0 else " (indefinite)"
        print(f"[{datetime.now().strftime('%H:%M:%S')}] SHOW '{text}' "
              f"at {pos_name}{dur_str}", file=sys.stderr)
        return f"OK pos={pos} pos_name={pos_name}"

    elif cmd == "SHOW_TIMED" and len(parts) >= 3:
        text = parts[1]
        duration = int(parts[2])
        position = int(parts[3]) if len(parts) > 3 else -1

        pos = state.show(text, duration, position)
        forwarder.forward_show(text, pos)

        pos_name = POSITIONS.get(pos, f"pos_{pos}")
        print(f"[{datetime.now().strftime('%H:%M:%S')}] SHOW_TIMED '{text}' "
              f"at {pos_name} for {duration}s", file=sys.stderr)
        return f"OK pos={pos} duration={duration}"

    elif cmd == "HIDE":
        state.hide()
        forwarder.forward_hide()
        print(f"[{datetime.now().strftime('%H:%M:%S')}] HIDE", file=sys.stderr)
        return "OK hidden"

    elif cmd == "STATUS":
        s = state.get_state()
        return json.dumps(s)

    else:
        return f"ERR unknown command: {cmd}"


def main():
    parser = argparse.ArgumentParser(description="FFmpeg Fingerprint Server")
    parser.add_argument("--port", type=int, default=5557,
                        help="Server listen port (default: 5557)")
    parser.add_argument("--bsf-port", type=int, default=5555,
                        help="BSF ZMQ port to forward to (default: 5555)")
    parser.add_argument("--sub-port", type=int, default=5556,
                        help="Subtitle injector ZMQ port (default: 5556)")
    args = parser.parse_args()

    state = FingerprintState()
    forwarder = FingerprintForwarder(state, args.bsf_port, args.sub_port)

    # Start expiry checker thread
    expiry_thread = threading.Thread(target=expiry_checker,
                                      args=(state, forwarder), daemon=True)
    expiry_thread.start()

    # Main ZMQ server
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    bind_addr = f"tcp://127.0.0.1:{args.port}"
    socket.bind(bind_addr)

    print(f"Fingerprint Server listening on {bind_addr}", file=sys.stderr)
    print(f"  Forwarding to BSF: tcp://127.0.0.1:{args.bsf_port}",
          file=sys.stderr)
    print(f"  Forwarding to SUB: tcp://127.0.0.1:{args.sub_port}",
          file=sys.stderr)

    def signal_handler(sig, frame):
        print("\nShutting down...", file=sys.stderr)
        socket.close()
        context.term()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    while True:
        try:
            msg = socket.recv_string()
            reply = handle_command(msg, state, forwarder)
            socket.send_string(reply)
        except zmq.error.ContextTerminated:
            break
        except Exception as e:
            print(f"[ERR] {e}", file=sys.stderr)
            try:
                socket.send_string(f"ERR {str(e)}")
            except Exception:
                pass


if __name__ == "__main__":
    main()
