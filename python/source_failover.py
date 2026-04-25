#!/usr/bin/env python3
"""
Source Failover Manager for ts_fingerprint / FFmpeg streams.

Manages FFmpeg + ts_fingerprint pipeline with automatic source failover:
- Priority-based backup streams
- Audio/video loss detection via built-in STATS
- Auto-reconnect to main source when it comes back
- Health monitoring without extra connections (uses ts_fingerprint STATS)

Usage:
    python3 source_failover.py --config failover.json
    python3 source_failover.py --sources "main=URL1,backup1=URL2,backup2=URL3" \
        --zmq tcp://127.0.0.1:5556 --output "pipe:1"
"""

import argparse
import json
import logging
import os
import signal
import subprocess
import sys
import threading
import time

try:
    import zmq
except ImportError:
    print("Error: pyzmq required. Install: pip3 install pyzmq")
    sys.exit(1)

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("failover")


class StreamSource:
    def __init__(self, name, url, priority=0):
        self.name = name
        self.url = url
        self.priority = priority  # lower = higher priority (0 = main)
        self.fail_count = 0
        self.last_fail = 0
        self.last_success = 0

    def __repr__(self):
        return f"<Source {self.name} pri={self.priority} fails={self.fail_count}>"


class FailoverManager:
    def __init__(self, sources, zmq_addr, output_args, ts_fp_args=None,
                 check_interval=5, fail_threshold=3, main_retry_interval=30):
        self.sources = sorted(sources, key=lambda s: s.priority)
        self.zmq_addr = zmq_addr
        self.output_args = output_args
        self.ts_fp_args = ts_fp_args or []
        self.check_interval = check_interval
        self.fail_threshold = fail_threshold
        self.main_retry_interval = main_retry_interval

        self.current_source = None
        self.ffmpeg_proc = None
        self.tsfp_proc = None
        self.running = False
        self.lock = threading.Lock()

        self.zmq_ctx = zmq.Context()

    def _build_ffmpeg_cmd(self, source_url):
        cmd = [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-reconnect", "1", "-reconnect_streamed", "1",
            "-reconnect_delay_max", "3",
            "-rw_timeout", "5000000",
            "-i", source_url,
            "-c:v", "copy", "-c:a", "copy",
            "-f", "mpegts", "pipe:1"
        ]
        return cmd

    def _build_tsfp_cmd(self):
        cmd = ["./bin/ts_fingerprint", "--zmq", self.zmq_addr, "--stats", "0"]
        cmd.extend(self.ts_fp_args)
        return cmd

    def _start_pipeline(self, source):
        """Start FFmpeg | ts_fingerprint pipeline for a source."""
        with self.lock:
            self._stop_pipeline()

            ffmpeg_cmd = self._build_ffmpeg_cmd(source.url)
            tsfp_cmd = self._build_tsfp_cmd()

            log.info(f"Starting pipeline: source={source.name} url={source.url}")

            self.ffmpeg_proc = subprocess.Popen(
                ffmpeg_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )

            if self.output_args:
                out_dest = subprocess.PIPE if self.output_args == "pipe:1" else None
                self.tsfp_proc = subprocess.Popen(
                    tsfp_cmd,
                    stdin=self.ffmpeg_proc.stdout,
                    stdout=out_dest or sys.stdout.buffer,
                    stderr=subprocess.PIPE
                )
            else:
                self.tsfp_proc = subprocess.Popen(
                    tsfp_cmd,
                    stdin=self.ffmpeg_proc.stdout,
                    stdout=sys.stdout.buffer,
                    stderr=subprocess.PIPE
                )

            self.ffmpeg_proc.stdout.close()
            self.current_source = source
            source.last_success = time.time()
            log.info(f"Pipeline started: {source.name}")

    def _stop_pipeline(self):
        """Stop the current FFmpeg | ts_fingerprint pipeline."""
        for proc, name in [(self.tsfp_proc, "ts_fingerprint"), (self.ffmpeg_proc, "ffmpeg")]:
            if proc and proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                log.info(f"Stopped {name}")
        self.ffmpeg_proc = None
        self.tsfp_proc = None

    def _get_stats(self):
        """Get stream stats via ZMQ STATS_JSON command."""
        try:
            sock = self.zmq_ctx.socket(zmq.REQ)
            sock.setsockopt(zmq.RCVTIMEO, 2000)
            sock.setsockopt(zmq.SNDTIMEO, 2000)
            sock.setsockopt(zmq.LINGER, 0)
            sock.connect(self.zmq_addr)
            sock.send_string("STATS_JSON")
            reply = sock.recv_string()
            sock.close()
            return json.loads(reply)
        except Exception:
            return None

    def _check_stream_health(self):
        """Check if the current stream is healthy."""
        if self.ffmpeg_proc and self.ffmpeg_proc.poll() is not None:
            log.warning("FFmpeg process died")
            return False

        if self.tsfp_proc and self.tsfp_proc.poll() is not None:
            log.warning("ts_fingerprint process died")
            return False

        stats = self._get_stats()
        if not stats:
            return False

        idle = stats.get("idle_seconds", 999)
        if idle > 10:
            log.warning(f"Stream idle for {idle}s - no data flowing")
            return False

        fps = stats.get("fps", 0)
        video_bitrate = stats.get("video_bitrate_kbps", 0)

        if stats.get("total_packets", 0) > 1000:
            if fps < 1.0:
                log.warning(f"Low FPS: {fps}")
                return False
            if video_bitrate < 10:
                log.warning(f"Low video bitrate: {video_bitrate}kbps")
                return False

        return True

    def _check_source_alive(self, source, timeout=5):
        """Quick check if a source URL is reachable using ffprobe."""
        try:
            result = subprocess.run(
                ["ffprobe", "-v", "quiet", "-i", source.url,
                 "-show_entries", "stream=codec_type",
                 "-of", "csv=p=0"],
                timeout=timeout,
                capture_output=True, text=True
            )
            return result.returncode == 0 and len(result.stdout.strip()) > 0
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return False

    def _select_next_source(self):
        """Select the next best available source based on priority."""
        for source in self.sources:
            if source == self.current_source:
                continue
            if time.time() - source.last_fail < 10 and source.fail_count > 2:
                continue
            log.info(f"Trying source: {source.name} (priority={source.priority})")
            if self._check_source_alive(source):
                return source
            else:
                source.fail_count += 1
                source.last_fail = time.time()
                log.warning(f"Source {source.name} unreachable")
        return None

    def _main_source_check_thread(self):
        """Background thread to periodically check if main source is back."""
        main_source = self.sources[0] if self.sources else None
        if not main_source:
            return

        while self.running:
            time.sleep(self.main_retry_interval)
            if not self.running:
                break

            if self.current_source == main_source:
                continue

            log.info(f"Checking if main source '{main_source.name}' is back online...")
            if self._check_source_alive(main_source):
                log.info(f"Main source '{main_source.name}' is back! Switching...")
                main_source.fail_count = 0
                self._start_pipeline(main_source)

    def run(self):
        """Main failover loop."""
        self.running = True

        signal.signal(signal.SIGINT, lambda s, f: self.stop())
        signal.signal(signal.SIGTERM, lambda s, f: self.stop())

        main_source = self.sources[0]
        self._start_pipeline(main_source)

        retry_thread = threading.Thread(target=self._main_source_check_thread, daemon=True)
        retry_thread.start()

        consecutive_fails = 0

        while self.running:
            time.sleep(self.check_interval)
            if not self.running:
                break

            healthy = self._check_stream_health()

            if healthy:
                consecutive_fails = 0
                if self.current_source:
                    self.current_source.fail_count = 0
            else:
                consecutive_fails += 1
                log.warning(f"Health check failed ({consecutive_fails}/{self.fail_threshold})")

                if consecutive_fails >= self.fail_threshold:
                    if self.current_source:
                        self.current_source.fail_count += 1
                        self.current_source.last_fail = time.time()

                    next_src = self._select_next_source()
                    if next_src:
                        log.info(f"Failover: {self.current_source.name} -> {next_src.name}")
                        self._start_pipeline(next_src)
                        consecutive_fails = 0
                    else:
                        log.error("No available sources! Retrying current...")
                        self._start_pipeline(self.current_source or main_source)
                        consecutive_fails = 0

        self._stop_pipeline()
        self.zmq_ctx.term()

    def stop(self):
        log.info("Shutting down...")
        self.running = False


def parse_sources_string(sources_str):
    """Parse 'main=URL1,backup1=URL2,backup2=URL3' into StreamSource list."""
    sources = []
    for i, part in enumerate(sources_str.split(",")):
        part = part.strip()
        if "=" in part:
            name, url = part.split("=", 1)
        else:
            name = f"source_{i}"
            url = part
        sources.append(StreamSource(name=name.strip(), url=url.strip(), priority=i))
    return sources


def load_config(config_path):
    """Load configuration from JSON file."""
    with open(config_path) as f:
        cfg = json.load(f)

    sources = []
    for i, src in enumerate(cfg.get("sources", [])):
        sources.append(StreamSource(
            name=src.get("name", f"source_{i}"),
            url=src["url"],
            priority=src.get("priority", i)
        ))

    return {
        "sources": sources,
        "zmq_addr": cfg.get("zmq_addr", "tcp://127.0.0.1:5556"),
        "output": cfg.get("output", "pipe:1"),
        "check_interval": cfg.get("check_interval", 5),
        "fail_threshold": cfg.get("fail_threshold", 3),
        "main_retry_interval": cfg.get("main_retry_interval", 30),
        "ts_fp_args": cfg.get("ts_fp_args", []),
    }


def main():
    parser = argparse.ArgumentParser(description="FFmpeg Source Failover Manager")
    parser.add_argument("--config", help="JSON config file path")
    parser.add_argument("--sources", help="Comma-separated name=URL pairs")
    parser.add_argument("--zmq", default="tcp://127.0.0.1:5556", help="ZMQ address")
    parser.add_argument("--output", default="pipe:1", help="Output destination")
    parser.add_argument("--check-interval", type=int, default=5, help="Health check interval (seconds)")
    parser.add_argument("--fail-threshold", type=int, default=3, help="Failures before failover")
    parser.add_argument("--main-retry", type=int, default=30, help="Seconds between main source checks")
    parser.add_argument("--lang", default=None, help="Subtitle language code")
    parser.add_argument("--display", default=None, help="Display resolution WxH")
    parser.add_argument("--forced", action="store_true", help="Force subtitle display")
    args = parser.parse_args()

    if args.config:
        cfg = load_config(args.config)
    elif args.sources:
        ts_fp_args = []
        if args.lang:
            ts_fp_args.extend(["--lang", args.lang])
        if args.display:
            ts_fp_args.extend(["--display", args.display])
        if args.forced:
            ts_fp_args.append("--forced")

        cfg = {
            "sources": parse_sources_string(args.sources),
            "zmq_addr": args.zmq,
            "output": args.output,
            "check_interval": args.check_interval,
            "fail_threshold": args.fail_threshold,
            "main_retry_interval": args.main_retry,
            "ts_fp_args": ts_fp_args,
        }
    else:
        parser.print_help()
        print("\nExample:")
        print('  python3 source_failover.py --sources "main=http://src1/stream,backup=http://src2/stream"')
        print('  python3 source_failover.py --config failover.json')
        sys.exit(1)

    if not cfg["sources"]:
        print("Error: No sources specified")
        sys.exit(1)

    log.info(f"Sources: {[f'{s.name}(pri={s.priority})' for s in cfg['sources']]}")
    log.info(f"ZMQ: {cfg['zmq_addr']}")
    log.info(f"Health check every {cfg['check_interval']}s, failover after {cfg['fail_threshold']} fails")
    log.info(f"Main source retry every {cfg['main_retry_interval']}s")

    manager = FailoverManager(
        sources=cfg["sources"],
        zmq_addr=cfg["zmq_addr"],
        output_args=cfg["output"],
        ts_fp_args=cfg.get("ts_fp_args", []),
        check_interval=cfg["check_interval"],
        fail_threshold=cfg["fail_threshold"],
        main_retry_interval=cfg["main_retry_interval"],
    )
    manager.run()


if __name__ == "__main__":
    main()
