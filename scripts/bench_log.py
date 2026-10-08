#!/usr/bin/env python3
"""Record a bench session from a serial port into hardware-evidence/ (local only, git-ignored).

This tool only READS and sends the short diagnostic commands you name with --send. It never flashes,
never opens a port unless you pass --port explicitly, and never lists or guesses ports. Use it only
after the USB port has been confirmed. Needs pyserial (pip install pyserial==3.5) for a real port.

  python3 scripts/bench_log.py --port <CONFIRMED_PORT> --label stage1 --send info --send scan \\
      --send "imu 10" --send "button 20" --seconds 60
  python3 scripts/bench_log.py --analyze hardware-evidence/<session>/raw.log     # no port needed
"""

import argparse
import datetime
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "hardware-evidence"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_analyze  # noqa: E402

SAFE_COMMAND = re.compile(
    r"^(help|info|scan|all|status|ping|nudge|up|adv|down confirm|imu( \d{1,3})?|button( \d{1,3})?)$"
)


def session_dir(label, now=None):
    now = now or datetime.datetime.now()
    clean = re.sub(r"[^a-zA-Z0-9_-]", "-", label)[:40] or "session"
    target = EVIDENCE / f"{now:%Y%m%d-%H%M%S}-{clean}"
    return target


def run_session(
    link,
    commands,
    seconds,
    out_dir,
    clock=time.monotonic,
    sleep=time.sleep,
    wall=datetime.datetime.now,
):
    """`link` needs read(), write(bytes). Returns the raw log text. Lines get host timestamps."""
    out_dir.mkdir(parents=True, exist_ok=False)
    raw = out_dir / "raw.log"
    buffer = b""
    lines = []
    sent = 0
    deadline = clock() + seconds
    next_command = clock() + 1.0
    with raw.open("w") as handle:
        while clock() < deadline:
            if sent < len(commands) and clock() >= next_command:
                link.write((commands[sent] + "\n").encode())
                handle.write(f"# {wall():%H:%M:%S.%f} host sent: {commands[sent]}\n")
                sent += 1
                next_command = clock() + 2.0
            chunk = link.read()
            if chunk:
                buffer += chunk
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    text = line.decode(errors="replace").rstrip("\r")
                    handle.write(f"{wall():%H:%M:%S.%f} {text}\n")
                    lines.append(text)
            else:
                sleep(0.01)
        handle.flush()
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--port", help="the CONFIRMED serial port (never guessed)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--label", default="session")
    parser.add_argument(
        "--send", action="append", default=[], help="a diagnostic command, e.g. 'imu 10'"
    )
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--analyze", help="analyze an existing log; no port is opened")
    args = parser.parse_args(argv)
    if args.analyze:
        result = bench_analyze.analyze_text(Path(args.analyze).read_text(errors="replace"))
        print(result["report"])
        return 0
    if not args.port:
        parser.error("--port is required (or use --analyze); this tool never guesses a port")
    for command in args.send:
        if not SAFE_COMMAND.fullmatch(command):
            parser.error(f"refusing unexpected command {command!r}")
    if not 1 <= args.seconds <= 3600:
        parser.error("--seconds must be 1..3600")
    try:
        import serial  # pyserial, imported only when a real port is used
    except ImportError:
        print("pyserial is not installed: pip install pyserial==3.5", file=sys.stderr)
        return 2

    class Link:
        def __init__(self):
            self.port = serial.Serial(args.port, args.baud, timeout=0.05)

        def read(self):
            return self.port.read(512)

        def write(self, data):
            self.port.write(data)

    out_dir = session_dir(args.label)
    text = run_session(Link(), args.send, args.seconds, out_dir)
    result = bench_analyze.analyze_text(text)
    (out_dir / "summary.txt").write_text(result["report"] + "\n")
    (out_dir / "summary.json").write_text(json.dumps(result, indent=1))
    print(result["report"])
    print(
        f"Saved locally in {out_dir.relative_to(ROOT)} (git-ignored; do not publish without review)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
