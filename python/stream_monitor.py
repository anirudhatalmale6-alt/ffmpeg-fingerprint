#!/usr/bin/env python3
"""
Stream Health Monitor for ts_fingerprint channels.

Monitors multiple channels via ZMQ STATS and reports health status.
Can be used as a standalone dashboard or integrated into panel backends.

Usage:
    # Monitor all running channels (auto-discover from PID files)
    python3 stream_monitor.py --auto

    # Monitor specific channels
    python3 stream_monitor.py --channels 17832,17833,17834

    # Monitor specific ZMQ addresses
    python3 stream_monitor.py --zmq "tcp://127.0.0.1:5600,tcp://127.0.0.1:5601"

    # JSON output for API integration
    python3 stream_monitor.py --auto --json

    # Continuous monitoring (refresh every 5 seconds)
    python3 stream_monitor.py --auto --loop 5
"""

import argparse
import json
import os
import sys
import time

try:
    import zmq
except ImportError:
    print("Error: pyzmq required. Install: pip3 install pyzmq")
    sys.exit(1)

PID_DIR = "/tmp/ts_fingerprint_pids"
ZMQ_BASE_PORT = 5600


def zmq_port_for_channel(channel_id):
    return ZMQ_BASE_PORT + (int(channel_id) % 20000)


def get_stats(zmq_addr, as_json=True, timeout=2000):
    """Get stats from a ts_fingerprint instance."""
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.RCVTIMEO, timeout)
    sock.setsockopt(zmq.SNDTIMEO, timeout)
    sock.setsockopt(zmq.LINGER, 0)

    try:
        sock.connect(zmq_addr)
        sock.send_string("STATS_JSON" if as_json else "STATS")
        reply = sock.recv_string()
        if as_json:
            return json.loads(reply)
        return reply
    except Exception:
        return None
    finally:
        sock.close()
        ctx.term()


def discover_channels():
    """Auto-discover running channels from PID directory."""
    channels = []
    if not os.path.exists(PID_DIR):
        return channels

    for f in os.listdir(PID_DIR):
        if f.startswith("ch_") and f.endswith(".json"):
            try:
                with open(os.path.join(PID_DIR, f)) as fh:
                    info = json.load(fh)
                channels.append({
                    "channel_id": info["channel_id"],
                    "zmq_addr": f"tcp://127.0.0.1:{info['zmq_port']}",
                    "source_url": info.get("source_url", ""),
                })
            except Exception:
                continue
    return channels


def health_status(stats):
    """Determine health status from stats."""
    if not stats:
        return "OFFLINE", "No response from ts_fingerprint"

    idle = stats.get("idle_seconds", 999)
    fps = stats.get("fps", 0)
    video_br = stats.get("video_bitrate_kbps", 0)
    cc_err = stats.get("cc_errors", 0)
    packets = stats.get("total_packets", 0)

    issues = []

    if idle > 10:
        return "DOWN", f"No data for {idle:.0f}s"

    if packets > 1000:
        if fps < 1.0:
            issues.append(f"low FPS ({fps:.1f})")
        if video_br < 10:
            issues.append(f"low bitrate ({video_br:.0f}kbps)")

    if cc_err > 100:
        issues.append(f"{cc_err} CC errors")

    if issues:
        return "DEGRADED", ", ".join(issues)

    return "HEALTHY", ""


def print_table(results):
    """Print results as a formatted table."""
    print(f"\n{'Channel':>10} {'Status':>10} {'Video':>8} {'Audio':>8} {'Bitrate':>12} "
          f"{'FPS':>6} {'CC Err':>8} {'Uptime':>12} {'Fingerprint':>15} {'Details':>30}")
    print("-" * 140)

    for r in results:
        stats = r.get("stats")
        status, detail = r.get("health", ("UNKNOWN", ""))

        if stats:
            print(f"{r['channel_id']:>10} "
                  f"{status:>10} "
                  f"{stats.get('video_codec', '-'):>8} "
                  f"{stats.get('audio_codec', '-'):>8} "
                  f"{stats.get('video_bitrate_kbps', 0):>8.0f}kbps "
                  f"{stats.get('fps', 0):>6.1f} "
                  f"{stats.get('cc_errors', 0):>8} "
                  f"{stats.get('uptime_seconds', 0)//3600}h{(stats.get('uptime_seconds', 0)%3600)//60:02d}m "
                  f"{'ON' if stats.get('fingerprint_active') else 'OFF':>15} "
                  f"{detail[:30]:>30}")
        else:
            print(f"{r['channel_id']:>10} "
                  f"{'OFFLINE':>10} "
                  f"{'-':>8} {'-':>8} {'-':>12} {'-':>6} {'-':>8} {'-':>12} {'-':>15} "
                  f"{'No response':>30}")


def main():
    parser = argparse.ArgumentParser(description="Stream Health Monitor")
    parser.add_argument("--auto", action="store_true", help="Auto-discover channels")
    parser.add_argument("--channels", help="Comma-separated channel IDs")
    parser.add_argument("--zmq", help="Comma-separated ZMQ addresses")
    parser.add_argument("--json", action="store_true", help="JSON output")
    parser.add_argument("--loop", type=int, default=0, help="Refresh interval (0=once)")
    args = parser.parse_args()

    targets = []

    if args.auto:
        targets = discover_channels()
        if not targets:
            print("No running channels found. Start channels first with xtream_fingerprint.py")
            sys.exit(0)
    elif args.channels:
        for ch_id in args.channels.split(","):
            ch_id = int(ch_id.strip())
            targets.append({
                "channel_id": ch_id,
                "zmq_addr": f"tcp://127.0.0.1:{zmq_port_for_channel(ch_id)}",
            })
    elif args.zmq:
        for i, addr in enumerate(args.zmq.split(",")):
            targets.append({
                "channel_id": i,
                "zmq_addr": addr.strip(),
            })
    else:
        parser.print_help()
        sys.exit(1)

    while True:
        results = []
        for target in targets:
            stats = get_stats(target["zmq_addr"])
            status = health_status(stats)
            results.append({
                "channel_id": target["channel_id"],
                "zmq_addr": target["zmq_addr"],
                "stats": stats,
                "health": status,
            })

        if args.json:
            output = []
            for r in results:
                entry = {
                    "channel_id": r["channel_id"],
                    "zmq_addr": r["zmq_addr"],
                    "status": r["health"][0],
                    "detail": r["health"][1],
                }
                if r["stats"]:
                    entry["stats"] = r["stats"]
                output.append(entry)
            print(json.dumps(output, indent=2))
        else:
            if args.loop:
                os.system("clear" if os.name != "nt" else "cls")
                print(f"Stream Monitor - {time.strftime('%H:%M:%S')} (refresh: {args.loop}s)")
            print_table(results)

        if args.loop <= 0:
            break
        time.sleep(args.loop)


if __name__ == "__main__":
    main()
