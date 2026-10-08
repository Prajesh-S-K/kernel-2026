"""Integration tests for desktop calibration, saved files, recording and replay."""

import importlib.util
import json
import math
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("server", ROOT / "desktop/server.py")
SERVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SERVER)
EXE = Path(os.environ.get("NODX_SIM", ROOT / "build/nodx_sim"))


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)

    def tearDown(self):
        self.device.close()
        self.tmp.cleanup()

    def send(self, command):
        return self.device.request(command)

    def calibrate(self):
        self.send("calibrate")
        for _ in range(20):
            result = self.send("step 50 0 0 0 0 1 1 0")
        self.assertEqual(result["calibration"], "COMPLETE")
        return result

    def test_end_to_end_calibration_saved_reloaded_after_restart(self):
        result = self.calibrate()
        self.assertGreater(result["profile"]["gain"][0], result["profile"]["gain"][1])
        self.device.close()
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)
        loaded = self.send("status")
        self.assertTrue(loaded["hasProfile"])
        self.assertEqual(loaded["profile"], result["profile"])

    def test_corrupt_both_slots_rejects_and_recalibration_repairs(self):
        self.calibrate()
        result = self.send("corrupt")
        self.assertEqual(result["state"], "SAFE_STATE")
        self.assertFalse(self.send("load")["ok"])
        self.calibrate()
        self.assertTrue(self.send("load")["ok"])

    def test_fault_recovery_needs_resume(self):
        self.calibrate()
        self.assertTrue(self.send("resume")["ok"])
        result = self.send("step 5 20 0 0 1 1 0 1")
        self.assertEqual(result["state"], "SAFE_STATE")
        self.assertTrue(all(r == [0, 0, 0, 0] for r in result["reports"]))
        result = self.send("step 20 20 0 0 1 1 0 0")
        self.assertEqual(result["state"], "READY")
        self.assertTrue(self.send("resume")["ok"])

    def test_recording_can_be_replayed(self):
        self.send("record on")
        self.send("step 50 10 0 0 0 1 0 0")
        self.send("record off")
        path = Path(self.tmp.name) / "samples.csv"
        self.assertEqual(len(path.read_text().splitlines()), 51)
        replay = subprocess.run(
            [str(EXE), self.tmp.name, "--replay", str(path)], capture_output=True, text=True
        )
        self.assertEqual(replay.returncode, 0, replay.stderr)
        rows = [json.loads(line) for line in replay.stdout.splitlines()]
        self.assertEqual(len(rows), 50)
        self.assertEqual(rows[-1]["state"], "ACTIVE")

    def test_nan_replay_enters_safe_and_stays_ready_after_recovery(self):
        path = Path(self.tmp.name) / "nan.csv"
        samples = ["timestampMs,gyroX,gyroY,gyroZ,accelX,accelY,accelZ,valid"]
        for i in range(1, 101):
            gyro = "nan" if i == 40 else "10"
            samples.append(f"{i * 10},{gyro},0,0,0,0,1,1")
        path.write_text("\n".join(samples) + "\n")
        replay = subprocess.run(
            [str(EXE), self.tmp.name, "--replay", str(path)], capture_output=True, text=True
        )
        self.assertEqual(replay.returncode, 0, replay.stderr)
        rows = [json.loads(line) for line in replay.stdout.splitlines()]
        self.assertEqual(rows[39]["state"], "SAFE_STATE")
        self.assertEqual(rows[-1]["state"], "READY")

    def test_native_protocol_refuses_unknown_or_oversized_batch(self):
        self.assertFalse(self.send("unknown")["ok"])
        self.assertFalse(self.send("step 51 0 0 0 0 1 0 0")["ok"])

    def test_api_rejects_nonfinite_and_out_of_range(self):
        for data in (
            {"action": "step", "yaw": math.nan},
            {"action": "step", "count": 1000},
            {"action": "step", "fault": 7},
            {"action": "other"},
        ):
            with self.assertRaises(ValueError):
                SERVER.command_for(data)

    def test_baseline_never_overwrites_saved_adaptive(self):
        adaptive = self.calibrate()["profile"]
        generic = self.send("generic")["profile"]
        self.assertNotEqual(generic, adaptive)
        self.assertEqual(self.send("load")["profile"], adaptive)

    def test_serial_adapter_correlates_ack_not_unsolicited_telemetry(self):
        class FakeSerial:
            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.written = data
                self.lines = [
                    b"[NODX] boot\n",
                    b'{"protocol":1,"requestId":0,"ok":true}\n',
                    b'{"protocol":1,"requestId":1,"ok":false,"source":"HARDWARE"}\n',
                ]

            def readline(self):
                return self.lines.pop(0)

        adapter = SERVER.SerialDevice.__new__(SERVER.SerialDevice)
        adapter.serial = FakeSerial()
        adapter.sequence = 0
        result = adapter.request("resume")
        self.assertFalse(result["ok"])
        self.assertEqual(adapter.serial.written, b"@1 resume\n")

    def test_serial_bridge_never_injects_virtual_motion_and_refuses_corruption(self):
        class FakeSerial:
            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.written = data
                self.sequence = int(data.decode().split()[0][1:])

            def readline(self):
                return json.dumps({"protocol": 1, "requestId": self.sequence, "ok": True}).encode()

        adapter = SERVER.SerialDevice.__new__(SERVER.SerialDevice)
        adapter.serial = FakeSerial()
        adapter.sequence = 0
        adapter.request("step 5 20 30 40 1 1 0 0")
        self.assertEqual(adapter.serial.written, b"@1 status\n")
        adapter.request("dwell 1")
        self.assertEqual(adapter.serial.written, b"@2 dwell on\n")
        with self.assertRaises(ValueError):
            adapter.request("corrupt")

    def test_strict_counts_booleans_and_trailing_native_fields(self):
        for command in (
            "step 1.5 0 0 0 1 1 0",
            "step 1 0 0 0 2 1 0 0",
            "dwell",
            "dwell 2",
            "status extra",
            "record maybe",
            "x" * 257,
        ):
            self.assertFalse(self.send(command)["ok"], command)
        for data in (
            {"action": "step", "count": 1.5},
            {"action": "step", "fault": 1.5},
            {"action": "step", "pressed": "false"},
            {"action": "dwell", "enabled": 1},
        ):
            with self.assertRaises(ValueError):
                SERVER.command_for(data)

    def test_pause_report_without_step_and_finite_invalid_profile_telemetry(self):
        self.calibrate()
        self.send("resume")
        self.send("step 5 20 0 0 1 1 0 0")
        result = self.send("pause")
        self.assertEqual(result["reports"][-1], [0, 0, 0, 0])
        result = self.send("corrupt")
        json.dumps(result, allow_nan=False)
        self.assertEqual(result["faultCode"], "PROFILE")

    def test_temporary_settings_preserve_saved_profile(self):
        saved = self.calibrate()["profile"]
        changed = self.send("settings 1 0")
        self.assertTrue(changed["profile"]["dwellEnabled"])
        self.assertEqual(self.send("load")["profile"], saved)

    def test_oversized_profile_file_is_rejected_without_loading(self):
        self.device.close()
        (Path(self.tmp.name) / "profile0.bin").write_bytes(b"x" * 4096)
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)
        result = self.send("status")
        self.assertFalse(result["hasProfile"])
        self.assertEqual(result["state"], "CALIBRATION_REQUIRED")
        self.assertEqual(result["softwareVersion"], (ROOT / "VERSION").read_text().strip())

    def test_partial_serial_acknowledgement(self):
        class FakeSerial:
            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.lines = [b'{"protocol":1,', b'"requestId":1,"ok":', b"true}\n"]

            def readline(self):
                return self.lines.pop(0)

        adapter = SERVER.SerialDevice.__new__(SERVER.SerialDevice)
        adapter.serial = FakeSerial()
        adapter.sequence = 0
        self.assertTrue(adapter.request("status")["ok"])


class DeadlineTests(unittest.TestCase):
    def test_native_timeout_dead_process_and_malformed_reply(self):
        import sys
        import time
        from unittest.mock import patch

        from desktop import transport

        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "device.py"
            for body in ("import time;time.sleep(10)", "print('broken',flush=True)", "pass"):
                script.write_text(body)
                process = subprocess.Popen(
                    [sys.executable, str(script)],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    bufsize=0,
                )
                with patch.object(transport.subprocess, "Popen", return_value=process):
                    device = transport.NativeDevice("unused", directory)
                started = time.monotonic()
                with patch.object(transport, "REQUEST_TIMEOUT_SECONDS", 0.05):
                    with self.assertRaises((ValueError, RuntimeError, OSError)):
                        device.request("status")
                self.assertLess(time.monotonic() - started, 0.8)
                device.close()

    def test_reply_json_rejects_nonfinite_values_and_invalid_ids(self):
        from desktop.transport import decode_reply

        for reply in (
            b'{"ok":true,"motion":[1e999]}',
            b'{"ok":true,"requestId":true}',
            b'{"ok":true,"requestId":1.5}',
            b'{"ok":true,"protocol":true}',
            b'{"ok":true,"requestId":4294967296}',
            b'{"ok":1}',
            b"[]",
        ):
            with self.assertRaises(ValueError):
                decode_reply(reply)

    def test_missing_serial_acknowledgement_deadline(self):
        import time
        from unittest.mock import patch

        from desktop import transport

        class FakeSerial:
            def reset_input_buffer(self):
                pass

            def write(self, data):
                pass

            def readline(self):
                time.sleep(0.005)
                return b'{"protocol":1,"requestId":0,"ok":true}\n'

        adapter = transport.SerialDevice.__new__(transport.SerialDevice)
        adapter.serial = FakeSerial()
        adapter.sequence = 0
        with patch.object(transport, "REQUEST_TIMEOUT_SECONDS", 0.02):
            with self.assertRaises(RuntimeError):
                adapter.request("pause")


if __name__ == "__main__":
    unittest.main(verbosity=2)
