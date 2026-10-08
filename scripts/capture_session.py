#!/usr/bin/env python3
"""Capture a raw 100 Hz sensor recording from the running companion for offline replay.

The board records up to 10 seconds of RAW sensor frames in RAM (`capture start`); this tool starts it,
waits, reads the pages back (`capture get`) and writes a CSV (t_ms,gx,gy,gz,ax,ay,az in sensor register
units: gyro 131 LSB per deg/s, accel 16384 LSB per g) plus a small metadata file, under
hardware-evidence/captures/ (git-ignored, local only; no device identifiers are written).

The capture is passive: it sends only capture commands and never changes control. For a practice
recording, run this at the moment you start the practice in the companion.

    python3 scripts/capture_session.py --label practice --seconds 10
    python3 scripts/capture_session.py --label pointing --seconds 10 --note "normal pointing, no gestures"
    python3 scripts/capture_session.py --label gestures --seconds 10 --note "gestures at about 2 s and 6 s"

Replay: build/nodx_replay quick --practice <practice.csv> --eval <session.csv>[:2000,6000]
"""

import argparse
import json
import sys
import time
import urllib.request
from pathlib import Path

PAGE = 16
ROOT = Path(__file__).resolve().parent.parent


def post(port, body, timeout=3.0):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/api/device",
        json.dumps(body).encode(),
        {"Content-Type": "application/json"},
    )
    return json.load(urllib.request.urlopen(request, timeout=timeout))


def read_capture(call, count):
    """Read `count` rows with `call(body) -> reply`; returns the list of [t, gx, gy, gz, ax, ay, az]."""
    rows = []
    for offset in range(0, count, PAGE):
        reply = call({"action": "capture", "op": "get", "offset": offset})
        capture = reply.get("capture", {})
        page = capture.get("rows")
        if not reply.get("ok") or capture.get("offset") != offset or not page:
            raise RuntimeError(f"capture page at {offset} was not returned")
        rows.extend(page)
    if len(rows) != count:
        raise RuntimeError(f"expected {count} rows, got {len(rows)}")
    return rows


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as out:
        out.write("t_ms,gx,gy,gz,ax,ay,az\n")
        for row in rows:
            out.write(",".join(str(int(v)) for v in row) + "\n")


def summarize(rows):
    """Basic sanity numbers: duration, mean sample interval, and the worst gap."""
    times = [r[0] for r in rows]
    gaps = [b - a for a, b in zip(times, times[1:])]
    return {
        "rows": len(rows),
        "seconds": round((times[-1] - times[0]) / 1000.0, 2) if len(times) > 1 else 0,
        "meanIntervalMs": round(sum(gaps) / len(gaps), 2) if gaps else 0,
        "maxGapMs": max(gaps) if gaps else 0,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("--port", type=int, default=8791, help="companion port")
    parser.add_argument("--seconds", type=int, default=10, help="1 to 10")
    parser.add_argument("--label", required=True, help="short name for the recording")
    parser.add_argument("--note", default="", help="what you will do during the capture")
    parser.add_argument("--out", type=Path, default=ROOT / "hardware-evidence" / "captures")
    args = parser.parse_args(argv)
    if not 1 <= args.seconds <= 10:
        parser.error("--seconds must be 1 to 10")
    label = "".join(c if c.isalnum() or c in "-_" else "-" for c in args.label)

    def call(body):
        return post(args.port, body)

    start = call({"action": "capture", "op": "start", "seconds": args.seconds})
    if not start.get("ok"):
        print("The board refused the capture:", start.get("reason"), file=sys.stderr)
        return 1
    print(f"Recording {args.seconds} s: do your {args.label} now.")
    deadline = time.time() + args.seconds + 3
    status = start
    while time.time() < deadline:
        time.sleep(0.25)
        status = call({"action": "status"})
        if not status.get("capture", {}).get("active"):
            break
    count = status.get("capture", {}).get("count", 0)
    if count < 2:
        print("Nothing was captured (is the sensor streaming?)", file=sys.stderr)
        return 1
    rows = read_capture(call, count)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    csv_path = args.out / f"{stamp}-{label}.csv"
    write_csv(csv_path, rows)
    meta = {"label": args.label, "note": args.note, "seconds": args.seconds, **summarize(rows)}
    csv_path.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n")
    print(
        f"Saved {csv_path} ({meta['rows']} rows, mean interval {meta['meanIntervalMs']} ms, "
        f"worst gap {meta['maxGapMs']} ms)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
