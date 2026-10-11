"""Capture script and offline replay. Synthetic recordings only; nothing here is a hardware result."""

import importlib.util
import json
import math
import random
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "capture_session", ROOT / "scripts/capture_session.py"
)
CAPTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAPTURE)
REPLAY = ROOT / "build" / "nodx_replay"
BIAS = (-1.2, -0.9, 0.1)


def sample_rows(count, start=1000):
    return [[start + 10 * i, i, -i, 2 * i, 3 * i, 4 * i, 16384] for i in range(count)]


class FakeBoard:
    """Answers capture commands the way the firmware does, from a list of rows."""

    def __init__(self, rows, refuse=False):
        self.rows = rows
        self.refuse = refuse
        self.log = []

    def __call__(self, body):
        self.log.append(body)
        if body["action"] == "capture" and body["op"] == "start":
            return {"ok": not self.refuse, "reason": "sensor not streaming" if self.refuse else ""}
        if body["action"] == "status":
            return {"ok": True, "capture": {"active": False, "count": len(self.rows)}}
        if body["action"] == "capture" and body["op"] == "get":
            offset = body["offset"]
            page = self.rows[offset : offset + CAPTURE.PAGE]
            return {
                "ok": True,
                "capture": {"offset": offset, "count": len(self.rows), "rows": page},
            }
        raise AssertionError(body)


class CaptureScript(unittest.TestCase):
    def test_pages_are_assembled_in_order(self):
        rows = sample_rows(70)
        self.assertEqual(CAPTURE.read_capture(FakeBoard(rows), 70), rows)

    def test_a_missing_or_misplaced_page_is_an_error(self):
        rows = sample_rows(40)

        def wrong_offset(body):
            reply = FakeBoard(rows)(body)
            reply["capture"]["offset"] += 1
            return reply

        with self.assertRaises(RuntimeError):
            CAPTURE.read_capture(wrong_offset, 40)
        with self.assertRaises(RuntimeError):
            CAPTURE.read_capture(FakeBoard(rows[:20]), 40)

    def test_summary_reports_rate_and_gaps(self):
        rows = sample_rows(100)
        rows[50][0] += 40  # one late sample
        summary = CAPTURE.summarize(rows)
        self.assertEqual(summary["rows"], 100)
        self.assertGreaterEqual(summary["maxGapMs"], 40)
        self.assertAlmostEqual(summary["meanIntervalMs"], 10.4, delta=0.5)

    def test_end_to_end_writes_csv_and_metadata_without_identifiers(self):
        rows = sample_rows(48)
        board = FakeBoard(rows)
        original = CAPTURE.post
        CAPTURE.post = lambda port, body, timeout=3.0: board(body)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                code = CAPTURE.main(
                    ["--label", "pointing test", "--seconds", "2", "--note", "n", "--out", tmp]
                )
                self.assertEqual(code, 0)
                csvs = list(Path(tmp).glob("*.csv"))
                self.assertEqual(len(csvs), 1)
                lines = csvs[0].read_text().splitlines()
                self.assertEqual(lines[0], "t_ms,gx,gy,gz,ax,ay,az")
                self.assertEqual(len(lines), 49)
                self.assertEqual(lines[1], "1000,0,0,0,0,0,16384")
                meta = json.loads(csvs[0].with_suffix(".json").read_text())
                self.assertEqual(meta["rows"], 48)
                self.assertEqual(meta["label"], "pointing test")
                self.assertNotIn("mac", json.dumps(meta).lower())
                self.assertIn("pointing-test", csvs[0].name)
        finally:
            CAPTURE.post = original

    def test_a_refused_capture_exits_nonzero_and_writes_nothing(self):
        board = FakeBoard([], refuse=True)
        original = CAPTURE.post
        CAPTURE.post = lambda port, body, timeout=3.0: board(body)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(CAPTURE.main(["--label", "x", "--seconds", "2", "--out", tmp]), 1)
                self.assertEqual(list(Path(tmp).iterdir()), [])
        finally:
            CAPTURE.post = original

    def test_only_capture_commands_are_sent(self):
        board = FakeBoard(sample_rows(32))
        original = CAPTURE.post
        CAPTURE.post = lambda port, body, timeout=3.0: board(body)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                CAPTURE.main(["--label", "x", "--seconds", "1", "--out", tmp])
        finally:
            CAPTURE.post = original
        for body in board.log:
            self.assertIn(body["action"], ("capture", "status"))
        with self.assertRaises(SystemExit):
            CAPTURE.main(["--label", "x", "--seconds", "21"])


def raw(rate_dps, rng, noise=0.8):
    return [int(round((rate_dps[i] + BIAS[i] + rng.gauss(0, noise)) * 131)) for i in range(3)]


def tilt_rate(t_ms, excursion=13.0, duration=600):
    if t_ms < 0 or t_ms > duration:
        return 0.0
    half = duration / 2
    amp = excursion * math.pi / (half / 1000) / 2
    return amp * math.sin(math.pi * t_ms / half)  # one full sine period: out, then back


def write(path, frames):
    with open(path, "w") as out:
        out.write("t_ms,gx,gy,gz,ax,ay,az\n")
        for t, g in frames:
            out.write(f"{t},{g[0]},{g[1]},{g[2]},0,0,16384\n")


@unittest.skipUnless(REPLAY.exists(), "build nodx_replay first")
class Replay(unittest.TestCase):
    def run_replay(self, *args):
        return subprocess.run(
            [str(REPLAY), "quick", "--axes", "0,1,2", *args], capture_output=True, text=True
        )

    def make(self, tmp):
        rng = random.Random(5)
        practice = []
        for i in range(1500):  # 15 s: still, countdown, one tilt-and-return, ordinary pointing
            t = 10 * i
            yaw = pitch = 0.0
            if t >= 6800:  # varied pointing in the yaw/pitch plane
                phase = (t - 6800) / 1000.0
                yaw = 45 * math.sin(2 * math.pi * phase * 0.8)
                pitch = 40 * math.sin(2 * math.pi * phase * 0.55 + 1)
            practice.append((t, raw((yaw, pitch, tilt_rate(t - 5300)), rng)))
        write(Path(tmp) / "practice.csv", practice)
        evalrows = []
        for i in range(1000):  # gestures at 1.5 s and 6.0 s; pointing in between
            t = 10 * i
            yaw = 40 * math.sin(math.pi * (t - 3000) / 1500) if 3000 <= t <= 4500 else 0
            z = tilt_rate(t - 1500) + tilt_rate(t - 6000)
            evalrows.append((t, raw((yaw, 0, z), rng)))
        write(Path(tmp) / "eval.csv", evalrows)

    def test_replay_learns_the_practice_and_counts_hits_and_misses(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.make(tmp)
            done = self.run_replay(
                "--practice", f"{tmp}/practice.csv", "--eval", f"{tmp}/eval.csv:1500,6000"
            )
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn("phase PREVIEW", done.stdout)
            summary = json.loads(done.stdout.strip().splitlines()[-1])
            self.assertEqual(summary["clicks"], 2)
            self.assertEqual(summary["hits"], 2)
            self.assertEqual(summary["missed"], 0)
            self.assertEqual(summary["falseClicks"], 0)
            self.assertGreater(summary["candidates"], 1)

    def test_an_unlabelled_extra_click_is_reported_false_and_a_missed_one_is_missed(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.make(tmp)
            done = self.run_replay(
                "--practice", f"{tmp}/practice.csv", "--eval", f"{tmp}/eval.csv:1500,3500"
            )
            summary = json.loads(done.stdout.strip().splitlines()[-1])
            self.assertEqual(summary["hits"], 1)
            self.assertEqual(summary["missed"], 1)
            self.assertEqual(summary["falseClicks"], 1)

    def test_a_practice_without_a_tilt_fails_and_bad_settings_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            rng = random.Random(1)
            write(Path(tmp) / "still.csv", [(10 * i, raw((0, 0, 0), rng)) for i in range(1000)])
            write(Path(tmp) / "eval.csv", [(10 * i, raw((0, 0, 0), rng)) for i in range(300)])
            done = self.run_replay("--practice", f"{tmp}/still.csv", "--eval", f"{tmp}/eval.csv")
            self.assertEqual(done.returncode, 1)
            self.assertIn('ok":false', done.stdout)
            bad = self.run_replay("--practice", f"{tmp}/still.csv", "--sens", "9")
            self.assertEqual(bad.returncode, 2)


if __name__ == "__main__":
    unittest.main()
