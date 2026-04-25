#!/usr/bin/env python3
"""
Xtream Codes Panel Integration for ts_fingerprint.

Manages fingerprint injection for Xtream Codes style IPTV panels:
- Per-user fingerprint triggering
- Channel-based stream management
- Multi-stream ZMQ port allocation
- Real-time stream monitoring via STATS

Usage:
    # Start fingerprint manager for a channel
    python3 xtream_fingerprint.py start --channel 17832 --source "http://source/stream"

    # Trigger fingerprint for a user on a channel
    python3 xtream_fingerprint.py trigger --channel 17832 --username "test12345" --duration 300

    # Get stream stats for a channel
    python3 xtream_fingerprint.py stats --channel 17832

    # Stop a channel
    python3 xtream_fingerprint.py stop --channel 17832
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time

try:
    import zmq
except ImportError:
    print("Error: pyzmq required. Install: pip3 install pyzmq")
    sys.exit(1)

ZMQ_BASE_PORT = 5600
PID_DIR = "/tmp/ts_fingerprint_pids"
BIN_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "bin")


def zmq_port_for_channel(channel_id):
    """Deterministic ZMQ port from channel ID."""
    return ZMQ_BASE_PORT + (int(channel_id) % 20000)


def zmq_addr_for_channel(channel_id):
    return f"tcp://127.0.0.1:{zmq_port_for_channel(channel_id)}"


def pid_file(channel_id):
    os.makedirs(PID_DIR, exist_ok=True)
    return os.path.join(PID_DIR, f"ch_{channel_id}.json")


def save_process_info(channel_id, ffmpeg_pid, tsfp_pid, source_url):
    info = {
        "channel_id": channel_id,
        "ffmpeg_pid": ffmpeg_pid,
        "tsfp_pid": tsfp_pid,
        "source_url": source_url,
        "zmq_port": zmq_port_for_channel(channel_id),
        "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    with open(pid_file(channel_id), "w") as f:
        json.dump(info, f)


def load_process_info(channel_id):
    pf = pid_file(channel_id)
    if os.path.exists(pf):
        with open(pf) as f:
            return json.load(f)
    return None


def is_process_running(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ProcessLookupError):
        return False


def cmd_start(args):
    """Start FFmpeg + ts_fingerprint pipeline for a channel."""
    channel_id = args.channel
    source_url = args.source
    zmq_addr = zmq_addr_for_channel(channel_id)

    info = load_process_info(channel_id)
    if info and is_process_running(info.get("tsfp_pid", 0)):
        print(f"Channel {channel_id} already running (ts_fp PID: {info['tsfp_pid']})")
        return

    ts_fp_bin = os.path.join(BIN_DIR, "ts_fingerprint")
    if not os.path.exists(ts_fp_bin):
        print(f"Error: ts_fingerprint not found at {ts_fp_bin}")
        print("Run 'make' first to build the tools.")
        sys.exit(1)

    ffmpeg_cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-reconnect", "1", "-reconnect_streamed", "1",
        "-reconnect_delay_max", "5",
        "-i", source_url,
        "-c:v", "copy", "-c:a", "copy",
        "-f", "mpegts", "pipe:1"
    ]

    tsfp_cmd = [ts_fp_bin, "--zmq", zmq_addr, "--stats", "0"]

    if args.lang:
        tsfp_cmd.extend(["--lang", args.lang])
    if args.display:
        tsfp_cmd.extend(["--display", args.display])
    if args.forced:
        tsfp_cmd.append("--forced")

    output = args.output or f"/tmp/ts_fingerprint_ch_{channel_id}.ts"

    if output == "pipe:1":
        out_file = sys.stdout.buffer
    else:
        out_file = open(output, "wb")

    ffmpeg_proc = subprocess.Popen(
        ffmpeg_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    tsfp_proc = subprocess.Popen(
        tsfp_cmd,
        stdin=ffmpeg_proc.stdout,
        stdout=out_file,
        stderr=subprocess.PIPE
    )

    ffmpeg_proc.stdout.close()

    save_process_info(channel_id, ffmpeg_proc.pid, tsfp_proc.pid, source_url)

    print(f"Channel {channel_id} started:")
    print(f"  Source: {source_url}")
    print(f"  ZMQ: {zmq_addr}")
    print(f"  FFmpeg PID: {ffmpeg_proc.pid}")
    print(f"  ts_fingerprint PID: {tsfp_proc.pid}")
    print(f"  Output: {output}")
    print(f"\nTrigger fingerprint:")
    print(f"  python3 xtream_fingerprint.py trigger --channel {channel_id} --username USER --duration 300")


def cmd_stop(args):
    """Stop a channel's pipeline."""
    channel_id = args.channel
    info = load_process_info(channel_id)
    if not info:
        print(f"Channel {channel_id} not found")
        return

    for key in ["tsfp_pid", "ffmpeg_pid"]:
        pid = info.get(key, 0)
        if pid and is_process_running(pid):
            try:
                os.kill(pid, signal.SIGTERM)
                print(f"Stopped {key.replace('_pid', '')} (PID {pid})")
            except OSError:
                pass

    pf = pid_file(channel_id)
    if os.path.exists(pf):
        os.remove(pf)
    print(f"Channel {channel_id} stopped")


def cmd_trigger(args):
    """Trigger fingerprint for a user on a channel."""
    channel_id = args.channel
    username = args.username
    duration = args.duration
    zmq_addr = zmq_addr_for_channel(channel_id)

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, 5000)
    sock.setsockopt(zmq.SNDTIMEO, 5000)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(zmq_addr)

    show_cmd = f"SHOW {username}"
    print(f"Channel {channel_id}: Showing '{username}' for {duration}s")
    sock.send_string(show_cmd)
    reply = sock.recv_string()
    print(f"  -> {reply}")

    if duration > 0:
        time.sleep(duration)
        sock.send_string("HIDE")
        reply = sock.recv_string()
        print(f"  Hidden -> {reply}")

    sock.close()
    ctx.term()


def cmd_trigger_async(args):
    """Trigger fingerprint without blocking (fire and forget SHOW, schedule HIDE)."""
    channel_id = args.channel
    username = args.username
    duration = args.duration
    zmq_addr = zmq_addr_for_channel(channel_id)

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, 5000)
    sock.setsockopt(zmq.SNDTIMEO, 5000)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(zmq_addr)

    sock.send_string(f"SHOW {username}")
    reply = sock.recv_string()
    print(f"Channel {channel_id}: SHOW '{username}' -> {reply}")

    sock.close()
    ctx.term()

    if duration > 0:
        subprocess.Popen(
            [sys.executable, __file__, "hide-delayed",
             "--channel", str(channel_id), "--delay", str(duration)],
            start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        print(f"  Auto-hide scheduled in {duration}s")


def cmd_hide_delayed(args):
    """Internal: delayed hide (called by trigger_async)."""
    time.sleep(args.delay)
    zmq_addr = zmq_addr_for_channel(args.channel)
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, 5000)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(zmq_addr)
    sock.send_string("HIDE")
    try:
        sock.recv_string()
    except Exception:
        pass
    sock.close()
    ctx.term()


def cmd_stats(args):
    """Get stream stats for a channel."""
    channel_id = args.channel
    zmq_addr = zmq_addr_for_channel(channel_id)

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, 5000)
    sock.setsockopt(zmq.LINGER, 0)
    sock.connect(zmq_addr)

    cmd = "STATS_JSON" if args.json else "STATS"
    sock.send_string(cmd)
    reply = sock.recv_string()

    if args.json:
        data = json.loads(reply)
        print(json.dumps(data, indent=2))
    else:
        print(f"Channel {channel_id} stats:")
        for pair in reply.split(" "):
            if "=" in pair:
                key, val = pair.split("=", 1)
                print(f"  {key}: {val}")

    sock.close()
    ctx.term()


def cmd_list(args):
    """List all running channels."""
    if not os.path.exists(PID_DIR):
        print("No channels running")
        return

    channels = []
    for f in os.listdir(PID_DIR):
        if f.startswith("ch_") and f.endswith(".json"):
            with open(os.path.join(PID_DIR, f)) as fh:
                info = json.load(fh)
            tsfp_alive = is_process_running(info.get("tsfp_pid", 0))
            ffmpeg_alive = is_process_running(info.get("ffmpeg_pid", 0))
            info["status"] = "running" if (tsfp_alive and ffmpeg_alive) else "dead"
            channels.append(info)

    if not channels:
        print("No channels found")
        return

    print(f"{'Channel':>10} {'Status':>10} {'ZMQ Port':>10} {'Source':>50} {'Started':>20}")
    print("-" * 110)
    for ch in channels:
        src = ch.get("source_url", "")[:50]
        print(f"{ch['channel_id']:>10} {ch['status']:>10} {ch['zmq_port']:>10} {src:>50} {ch.get('started_at', ''):>20}")


def cmd_bulk_trigger(args):
    """Trigger fingerprint on multiple channels at once."""
    channel_ids = [int(x.strip()) for x in args.channels.split(",")]
    username = args.username
    duration = args.duration

    for ch_id in channel_ids:
        try:
            zmq_addr = zmq_addr_for_channel(ch_id)
            ctx = zmq.Context()
            sock = ctx.socket(zmq.REQ)
            sock.setsockopt(zmq.RCVTIMEO, 2000)
            sock.setsockopt(zmq.LINGER, 0)
            sock.connect(zmq_addr)
            sock.send_string(f"SHOW {username}")
            reply = sock.recv_string()
            print(f"Channel {ch_id}: {reply}")
            sock.close()
            ctx.term()
        except Exception as e:
            print(f"Channel {ch_id}: FAILED - {e}")

    if duration > 0:
        print(f"\nWaiting {duration}s before hiding...")
        time.sleep(duration)
        for ch_id in channel_ids:
            try:
                zmq_addr = zmq_addr_for_channel(ch_id)
                ctx = zmq.Context()
                sock = ctx.socket(zmq.REQ)
                sock.setsockopt(zmq.RCVTIMEO, 2000)
                sock.setsockopt(zmq.LINGER, 0)
                sock.connect(zmq_addr)
                sock.send_string("HIDE")
                sock.recv_string()
                sock.close()
                ctx.term()
            except Exception:
                pass
        print("All hidden")


def main():
    parser = argparse.ArgumentParser(description="Xtream Codes Fingerprint Manager")
    sub = parser.add_subparsers(dest="command")

    # start
    p_start = sub.add_parser("start", help="Start channel pipeline")
    p_start.add_argument("--channel", type=int, required=True, help="Channel ID")
    p_start.add_argument("--source", required=True, help="Source URL")
    p_start.add_argument("--output", help="Output path (default: /tmp/ts_fingerprint_ch_ID.ts)")
    p_start.add_argument("--lang", help="Subtitle language code")
    p_start.add_argument("--display", help="Display resolution WxH")
    p_start.add_argument("--forced", action="store_true", help="Force subtitle display")

    # stop
    p_stop = sub.add_parser("stop", help="Stop channel pipeline")
    p_stop.add_argument("--channel", type=int, required=True)

    # trigger (blocking)
    p_trig = sub.add_parser("trigger", help="Trigger fingerprint (blocking)")
    p_trig.add_argument("--channel", type=int, required=True)
    p_trig.add_argument("--username", required=True)
    p_trig.add_argument("--duration", type=int, default=300, help="Duration in seconds")

    # trigger-async (non-blocking)
    p_async = sub.add_parser("trigger-async", help="Trigger fingerprint (non-blocking)")
    p_async.add_argument("--channel", type=int, required=True)
    p_async.add_argument("--username", required=True)
    p_async.add_argument("--duration", type=int, default=300)

    # hide-delayed (internal)
    p_hide = sub.add_parser("hide-delayed", help=argparse.SUPPRESS)
    p_hide.add_argument("--channel", type=int, required=True)
    p_hide.add_argument("--delay", type=int, required=True)

    # stats
    p_stats = sub.add_parser("stats", help="Get stream statistics")
    p_stats.add_argument("--channel", type=int, required=True)
    p_stats.add_argument("--json", action="store_true", help="JSON output")

    # list
    sub.add_parser("list", help="List running channels")

    # bulk-trigger
    p_bulk = sub.add_parser("bulk-trigger", help="Trigger on multiple channels")
    p_bulk.add_argument("--channels", required=True, help="Comma-separated channel IDs")
    p_bulk.add_argument("--username", required=True)
    p_bulk.add_argument("--duration", type=int, default=300)

    args = parser.parse_args()

    commands = {
        "start": cmd_start,
        "stop": cmd_stop,
        "trigger": cmd_trigger,
        "trigger-async": cmd_trigger_async,
        "hide-delayed": cmd_hide_delayed,
        "stats": cmd_stats,
        "list": cmd_list,
        "bulk-trigger": cmd_bulk_trigger,
    }

    if args.command in commands:
        commands[args.command](args)
    else:
        parser.print_help()
        print("\nExamples:")
        print("  python3 xtream_fingerprint.py start --channel 17832 --source 'http://source/stream'")
        print("  python3 xtream_fingerprint.py trigger --channel 17832 --username test12345 --duration 300")
        print("  python3 xtream_fingerprint.py trigger-async --channel 17832 --username test12345 --duration 300")
        print("  python3 xtream_fingerprint.py stats --channel 17832 --json")
        print("  python3 xtream_fingerprint.py list")
        print("  python3 xtream_fingerprint.py stop --channel 17832")


if __name__ == "__main__":
    main()
