"""Hands-free integration tests: native engine over its line protocol, the HTTP bridge's
validation and the serial adapter. Synthetic input only; nothing here measures real users."""

import http.server
import importlib.util
import json
import math
import os
import subprocess
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("server", ROOT / "desktop/server.py")
SERVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SERVER)
EXE = Path(os.environ.get("NODX_SIM", ROOT / "build/nodx_sim"))
NEUTRAL = "step 50 0 0 0 0 1 0 0"


class HandsFreeCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.runtime = Path(self.tmp.name)
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)

    def tearDown(self):
        self.device.close()
        self.tmp.cleanup()

    def send(self, command):
        return self.device.request(command)

    def restart(self):
        self.device.close()
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)

    def quiet(self, samples=50):
        remaining, result = samples, None
        while remaining > 0:
            count = min(50, remaining)
            result = self.send(f"step {count} 0 0 0 0 1 0 0")
            remaining -= count
        return result

    def calibrate(self):
        self.send("calibrate")
        for _ in range(20):
            result = self.send("step 50 0 0 0 0 1 1 0")
        self.assertEqual(result["calibration"], "COMPLETE")

    def train(self, gesture, pattern):
        self.assertTrue(self.send(f"train start {gesture}")["ok"])
        self.quiet(110)
        for _ in range(4):
            self.assertTrue(self.send(f"gesture {pattern}")["ok"])
        result = self.send(f"gesture {pattern}")  # validation repeat
        self.assertEqual(result["handsFree"]["training"]["phase"], "READY", result)
        self.assertTrue(self.send("train accept")["ok"])

    def setup_hands_free(self, kind="maintained"):
        """Helper setup. Most workflow tests use the maintained-switch compatibility
        configuration (switch ON from the start); the push-button tests pass kind="momentary"."""
        self.calibrate()
        self.train("pause", "nod2")
        self.train("drag", "tilt2")
        self.assertTrue(self.send(f"handsfree enable {kind}")["ok"])
        result = self.send("handsfree commit")
        self.assertTrue(result["ok"], result)
        if kind == "maintained":
            result = self.send("enable 1")
            self.quiet(10)
        else:
            self.quiet(60)  # a new input kind starts disabled: let the button be seen released
        return result

    def activate_by_gesture(self):
        self.quiet(60)
        result = self.send("gesture nod2")
        self.assertEqual(result["state"], "ACTIVE", result["handsFree"])
        return result


class SetupAndDailyWorkflow(HandsFreeCase):
    def test_helper_setup_then_daily_use_needs_no_buttons(self):
        committed = self.setup_hands_free()
        self.assertEqual(committed["state"], "READY")
        self.assertEqual(committed["handsFree"]["mode"], "HANDS_FREE")
        self.assertTrue(committed["profile"]["dwellEnabled"])
        self.assertEqual(committed["protocolRevision"], 4)
        self.assertEqual(committed["protocol"], 1)
        # Daily workflow: only gestures and the maintained switch, no resume/pause/calibrate.
        active = self.activate_by_gesture()
        self.assertTrue(active["handsFree"]["switch"]["permitted"])
        dragging = self.send("gesture tilt2")
        self.assertTrue(dragging["handsFree"]["drag"])
        self.assertEqual(dragging["cursor"], "DRAG")
        self.assertTrue(dragging["reports"][-1][3])
        released = self.send("gesture tilt2")
        self.assertFalse(released["handsFree"]["drag"])
        self.assertEqual(released["reports"][-1][3], 0)
        paused = self.send("gesture nod2")
        self.assertEqual(paused["state"], "PAUSED")
        self.assertEqual(self.send("gesture nod2")["state"], "ACTIVE")

    def test_dwell_click_works_while_hands_free(self):
        self.setup_hands_free()
        self.activate_by_gesture()
        self.send("step 30 30 0 0 0 1 0 0")  # move away so the post-resume lockout clears
        clicks = 0
        for _ in range(6):
            result = self.send(NEUTRAL)
            downs = [r[3] for r in result["reports"]]
            clicks += sum(1 for a, b in zip([0] + downs, downs) if b and not a)
        self.assertEqual(clicks, 1)

    def test_conversion_is_explicit(self):
        self.calibrate()
        before = self.send("status")
        self.assertEqual(before["handsFree"]["mode"], "LEGACY_SWITCH")
        self.assertFalse(before["profile"]["dwellEnabled"])
        self.send("generic")
        self.send("load")
        self.quiet(200)
        self.train("pause", "nod2")
        self.restart()
        after = self.send("status")
        self.assertEqual(after["handsFree"]["mode"], "LEGACY_SWITCH")
        self.assertEqual(after["handsFree"]["config"], "MISSING")
        self.assertEqual(after["profile"], before["profile"])
        self.assertFalse((self.runtime / "hf0.bin").exists())

    def test_existing_clients_and_commands_still_work(self):
        self.calibrate()
        result = self.send("status")
        for key in (
            "protocol",
            "protocolRevision",
            "faultCode",
            "softwareVersion",
            "profile",
            "reports",
            "dwell",
            "calibration",
        ):
            self.assertIn(key, result)
        self.assertEqual(result["protocol"], 1)
        self.assertTrue(self.send("resume")["ok"])
        stepped = self.send("step 5 20 0 0 1 1 0 0")  # the original eight fields
        self.assertTrue(stepped["ok"])
        self.assertTrue(self.send("pause")["ok"])
        self.assertTrue(self.send("settings 1 0")["ok"])
        self.assertTrue(self.send("dwell 1")["ok"])

    def test_telemetry_is_finite_valid_json_in_every_state(self):
        states = [self.send("status")]
        self.calibrate()
        states.append(self.send("step 5 0 0 0 0 1 0 1"))  # NaN gyro fault
        states.append(self.send("step 5 0 0 0 0 1 0 2"))  # extreme gyro
        self.setup_hands_free()
        (self.runtime / "hf0.bin").write_bytes(b"x" * 600)
        (self.runtime / "hf1.bin").write_bytes(b"y")
        self.restart()
        states.append(self.send("status"))
        states.append(self.send("corrupt"))
        for state in states:
            encoded = json.dumps(state, allow_nan=False, separators=(",", ":"))
            self.assertIn("handsFree", state)
            # Firmware frames are bounded at 2048 bytes; keep real headroom for longer reasons.
            self.assertLess(len(encoded), 1900)


class CommandValidation(HandsFreeCase):
    def test_native_commands_reject_malformed_input(self):
        for command in (
            "enable",
            "enable 2",
            "enable 1 extra",
            "enable 0.5",
            "gesture",
            "gesture nod9",
            "gesture nod2 9",
            "gesture nod2 nan",
            "gesture nod2 inf",
            "gesture nod2 1 extra",
            "gesture spin2",
            "train",
            "train start",
            "train start both",
            "train start pause extra",
            "train cancel now",
            "train bogus",
            "handsfree",
            "handsfree bogus",
            "handsfree commit extra",
            "handsfree switchless maybe",
            "handsfree switchless",
            "step 5 0 0 0 0 1 0 0 2",
            "step 5 0 0 0 0 1 0 0 1 extra",
            "step 5 0 0 0 0 1 0 0 1.5",
            "step 5 0 0 0 0 1 0 0 -1",
        ):
            self.assertFalse(self.send(command)["ok"], command)

    def test_server_validates_new_actions(self):
        for data in (
            {"action": "enable"},
            {"action": "enable", "enabled": 1},
            {"action": "gesture"},
            {"action": "gesture", "name": "nod4"},
            {"action": "gesture", "name": "nod2", "scale": math.nan},
            {"action": "gesture", "name": "nod2", "scale": 10},
            {"action": "gesture", "name": "nod2\nstatus"},
            {"action": "train"},
            {"action": "train", "op": "start"},
            {"action": "train", "op": "start", "gesture": "both"},
            {"action": "train", "op": "wipe"},
            {"action": "handsfree"},
            {"action": "handsfree", "op": "switchless"},
            {"action": "handsfree", "op": "switchless", "enabled": "yes"},
            {"action": "step", "enabled": 1},
            {"action": "step", "enabled": "true"},
        ):
            with self.assertRaises(ValueError, msg=str(data)):
                SERVER.command_for(data)
        self.assertEqual(SERVER.command_for({"action": "enable", "enabled": False}), "enable 0")
        self.assertEqual(
            SERVER.command_for({"action": "calibrate", "guided": True}), "calibrate guided"
        )
        self.assertEqual(SERVER.command_for({"action": "calibrate"}), "calibrate")
        self.assertEqual(
            SERVER.command_for({"action": "handsfree", "op": "uncal", "enabled": True}),
            "handsfree uncal start",
        )
        self.assertEqual(
            SERVER.command_for({"action": "handsfree", "op": "uncal", "enabled": False}),
            "handsfree uncal stop",
        )
        with self.assertRaises(ValueError):
            SERVER.command_for({"action": "handsfree", "op": "uncal"})
        self.assertEqual(
            SERVER.command_for({"action": "gesture", "name": "tilt2"}), "gesture tilt2 1"
        )
        self.assertEqual(
            SERVER.command_for({"action": "train", "op": "start", "gesture": "drag"}),
            "train start drag",
        )
        self.assertEqual(
            SERVER.command_for({"action": "handsfree", "op": "switchless", "enabled": True}),
            "handsfree switchless on",
        )
        self.assertTrue(SERVER.command_for({"action": "step", "enabled": False}).endswith(" 0"))
        self.assertEqual(
            SERVER.command_for({"action": "step"}).split()[-1],
            "0",
            "an omitted enabled field must not add a ninth token",
        )

    def test_training_requires_a_profile_and_accept_requires_validation(self):
        self.assertFalse(self.send("train start pause")["ok"])
        self.calibrate()
        started = self.send("train start pause")
        self.assertTrue(started["ok"])
        self.assertEqual(started["state"], "TRAINING")
        self.assertFalse(self.send("train accept")["ok"])
        cancelled = self.send("train cancel")
        self.assertTrue(cancelled["ok"])
        self.assertEqual(cancelled["state"], "READY")
        self.assertEqual(cancelled["handsFree"]["training"]["phase"], "IDLE")
        self.assertEqual(self.send("handsfree commit")["handsFree"]["mode"], "LEGACY_SWITCH")
        self.assertFalse(self.send("handsfree commit")["ok"])


class SaveFailureAndRecovery(HandsFreeCase):
    def block(self, names):
        for name in names:  # a directory in the way makes the temp-file write fail
            (self.runtime / f"{name}.tmp").mkdir()

    def unblock(self, names):
        for name in names:
            (self.runtime / f"{name}.tmp").rmdir()

    def staged(self):
        self.calibrate()
        self.train("pause", "nod2")
        self.train("drag", "tilt2")

    def test_profile_write_failure_is_reported_and_nothing_changes(self):
        self.staged()
        names = ["profile0.bin", "profile1.bin"]
        self.block(names)
        result = self.send("handsfree commit")
        self.assertFalse(result["ok"])
        self.assertEqual(result["faultCode"], "STORAGE")
        self.assertEqual(result["handsFree"]["mode"], "LEGACY_SWITCH")
        self.assertEqual(result["handsFree"]["config"], "MISSING")
        self.assertFalse(result["profile"]["dwellEnabled"])
        self.assertFalse((self.runtime / "hf0.bin").exists())
        self.unblock(names)
        self.assertTrue(self.send("handsfree commit")["ok"])

    def test_configuration_write_failure_restores_the_profile(self):
        self.staged()
        names = ["hf0.bin", "hf1.bin"]
        self.block(names)
        result = self.send("handsfree commit")
        self.assertFalse(result["ok"])
        self.assertEqual(result["faultCode"], "STORAGE")
        self.assertEqual(result["handsFree"]["mode"], "LEGACY_SWITCH")
        self.restart()
        reloaded = self.send("status")
        self.assertFalse(reloaded["profile"]["dwellEnabled"], "profile left converted")
        self.assertEqual(reloaded["handsFree"]["config"], "MISSING")
        self.unblock(names)

    def test_failed_replacement_keeps_the_prior_configuration(self):
        self.setup_hands_free()
        prior = self.send("status")["handsFree"]["configId"]
        self.train("drag", "turn2")
        names = ["hf0.bin", "hf1.bin"]
        self.block(names)
        self.assertFalse(self.send("handsfree commit")["ok"])
        self.unblock(names)
        self.restart()
        self.assertEqual(self.send("status")["handsFree"]["configId"], prior)

    def test_corrupt_and_oversized_configuration_inhibit_until_helper_repair(self):
        self.setup_hands_free()
        for payload in (b"x" * 4096, b"short"):
            self.device.close()
            for name in ("hf0.bin", "hf1.bin"):
                (self.runtime / name).write_bytes(payload)
            self.device = SERVER.NativeDevice(EXE, self.tmp.name)
            status = self.quiet(60)
            self.assertEqual(status["handsFree"]["mode"], "CONFIG_INVALID")
            self.assertEqual(status["handsFree"]["config"], "CORRUPT")
            self.assertFalse(self.send("resume")["ok"])
            self.assertEqual(self.send("gesture nod2")["state"], "READY")
        repaired = self.send("handsfree legacy")
        self.assertTrue(repaired["ok"])
        self.assertEqual(repaired["handsFree"]["mode"], "LEGACY_SWITCH")
        self.restart()
        self.assertEqual(self.send("status")["handsFree"]["mode"], "LEGACY_SWITCH")

    def test_fault_recovery_never_resumes_hands_free_control(self):
        self.setup_hands_free()
        self.activate_by_gesture()
        faulted = self.send("step 5 0 0 0 0 1 0 1")
        self.assertEqual(faulted["state"], "SAFE_STATE")
        self.assertTrue(all(r == [0, 0, 0, 0] for r in faulted["reports"]))
        recovered = self.quiet(200)
        self.assertEqual(recovered["state"], "READY")
        self.assertFalse(self.send("gesture tilt2")["handsFree"]["drag"])


class MomentaryEnableButton(HandsFreeCase):
    """The enable input is one momentary push button: a debounced press toggles a latch that is
    never persisted. Raw pressed state and latched permission are reported separately."""

    def press(self, samples=6):
        self.assertTrue(self.send("enable 1")["ok"])
        self.send(f"step {samples} 0 0 0 0 1 0 0")
        result = self.send("enable 0")
        self.send(f"step {samples} 0 0 0 0 1 0 0")
        return result

    def switch(self):
        return self.send("status")["handsFree"]["switch"]

    def test_new_setup_assumes_the_button_and_starts_disabled(self):
        self.setup_hands_free("momentary")
        status = self.send("status")
        switch = status["handsFree"]["switch"]
        self.assertEqual(switch["kind"], "MOMENTARY")
        self.assertEqual(switch["kindStaged"], "MOMENTARY")
        self.assertFalse(switch["pressed"])
        self.assertFalse(switch["latched"])
        self.assertFalse(switch["permitted"])
        self.assertEqual(status["protocolRevision"], 4)
        self.assertFalse(self.send("resume")["ok"], "helper resume while disabled")
        self.quiet(60)
        self.assertNotEqual(self.send("gesture nod2")["state"], "ACTIVE")

    def test_press_enables_without_resuming_and_the_gesture_resumes(self):
        self.setup_hands_free("momentary")
        self.quiet(60)
        self.assertTrue(self.send("enable 1")["ok"])
        held = self.send("step 6 0 0 0 0 1 0 0")["handsFree"]["switch"]
        self.assertTrue(held["pressed"] and held["latched"] and held["permitted"])
        released = self.send("enable 0")
        self.send("step 6 0 0 0 0 1 0 0")
        switch = self.switch()
        self.assertFalse(switch["pressed"], "raw pressed state follows the button")
        self.assertTrue(switch["latched"], "latched permission outlives the press")
        self.assertNotEqual(released["state"], "ACTIVE", "enabling resumed control")
        self.assertEqual(self.send("gesture nod2")["state"], "ACTIVE")

    def test_second_press_disables_and_releases_a_drag_at_once(self):
        self.setup_hands_free("momentary")
        self.press()
        self.quiet(60)
        self.assertEqual(self.send("gesture nod2")["state"], "ACTIVE")
        dragging = self.send("gesture tilt2")
        self.assertTrue(dragging["handsFree"]["drag"])
        self.assertTrue(dragging["reports"][-1][3])
        disabled = self.send("enable 1")  # the press edge itself, no sample in between
        self.assertEqual(disabled["state"], "PAUSED")
        self.assertFalse(disabled["handsFree"]["drag"])
        self.assertFalse(disabled["handsFree"]["switch"]["latched"])
        self.assertEqual(disabled["reports"][-1][3], 0, "button still down after the press")

    def test_holding_the_button_toggles_only_once(self):
        self.setup_hands_free("momentary")
        self.quiet(60)
        self.send("enable 1")
        for _ in range(20):  # a long hold
            result = self.send("step 50 0 0 0 0 1 0 0")
        self.assertTrue(result["handsFree"]["switch"]["latched"])
        self.send("enable 0")
        self.send("step 6 0 0 0 0 1 0 0")
        self.send("enable 1")
        result = self.send("step 50 0 0 0 0 1 0 0")
        self.assertFalse(result["handsFree"]["switch"]["latched"], "second press disables")
        for _ in range(10):
            result = self.send("step 50 0 0 0 0 1 0 0")
        self.assertFalse(result["handsFree"]["switch"]["latched"], "a hold toggled twice")

    def test_reboot_resets_permission_and_boot_held_enables_nothing(self):
        self.setup_hands_free("momentary")
        self.press()
        self.assertTrue(self.switch()["latched"])
        self.restart()
        switch = self.switch()
        self.assertEqual(switch["kind"], "MOMENTARY", "the kind is stored")
        self.assertFalse(switch["latched"], "the latch was persisted")
        self.assertEqual(self.send("status")["handsFree"]["mode"], "HANDS_FREE")
        self.restart()
        self.assertTrue(self.send("enable 1")["ok"])  # held from power-up
        for _ in range(10):
            result = self.send("step 50 0 0 0 0 1 0 0")
        self.assertFalse(result["handsFree"]["switch"]["permitted"], "held button enabled control")
        self.send("enable 0")
        self.send("step 6 0 0 0 0 1 0 0")
        self.assertFalse(self.switch()["permitted"], "release alone enabled control")
        self.press()
        self.assertTrue(self.switch()["permitted"])
        self.assertNotEqual(self.send("status")["state"], "ACTIVE")

    def test_fault_drops_the_permission_and_presses_do_not_recover(self):
        self.setup_hands_free("momentary")
        self.press()
        self.quiet(60)
        self.assertEqual(self.send("gesture nod2")["state"], "ACTIVE")
        fault = self.send("step 5 0 0 0 0 1 0 1")  # NaN gyro
        self.assertEqual(fault["state"], "SAFE_STATE")
        self.assertFalse(fault["handsFree"]["switch"]["latched"])
        for _ in range(3):
            self.send("enable 1")
            self.send("step 6 0 0 0 0 1 0 1")
            self.send("enable 0")
            result = self.send("step 6 0 0 0 0 1 0 1")
            self.assertEqual(result["state"], "SAFE_STATE", "a press bypassed the fault")
        self.assertFalse(self.send("resume")["ok"])

    def test_maintained_configuration_keeps_working_and_is_never_reinterpreted(self):
        self.setup_hands_free("maintained")
        self.restart()
        switch = self.send("status")["handsFree"]["switch"]
        self.assertEqual(switch["kind"], "MAINTAINED")
        self.send("enable 1")
        result = self.send("step 10 0 0 0 0 1 0 0")
        self.assertTrue(result["handsFree"]["switch"]["permitted"])
        self.assertFalse(result["handsFree"]["switch"]["latched"], "maintained has no latch")
        self.assertNotEqual(result["state"], "ACTIVE")
        off = self.send("enable 0")
        self.assertFalse(off["handsFree"]["switch"]["permitted"], "OFF is immediate")
        # Retraining and saving must not silently turn it into the button.
        self.train("pause", "nod2")
        self.assertTrue(self.send("handsfree commit")["ok"])
        self.restart()
        self.assertEqual(self.switch()["kind"], "MAINTAINED")

    def test_kind_commands_are_validated(self):
        self.assertFalse(self.send("handsfree enable")["ok"])
        self.assertFalse(self.send("handsfree enable bogus")["ok"])
        self.assertFalse(self.send("handsfree enable momentary extra")["ok"])
        self.assertTrue(self.send("handsfree enable momentary")["ok"])
        self.assertEqual(self.switch()["kindStaged"], "MOMENTARY")
        self.assertEqual(
            SERVER.command_for({"action": "handsfree", "op": "enable", "kind": "momentary"}),
            "handsfree enable momentary",
        )
        for bad in ("toggle", None, 3):
            with self.assertRaises(ValueError):
                SERVER.command_for({"action": "handsfree", "op": "enable", "kind": bad})


class MovementOnlyDemo(HandsFreeCase):
    """Temporary movement-only demo: no dwell click, drag or wheel, bounded steps; RAM only."""

    def test_the_demo_is_a_validated_temporary_mode(self):
        self.setup_hands_free("maintained")
        self.quiet(60)
        self.assertFalse(self.send("status")["handsFree"]["demoMovementOnly"])
        for bad in ("handsfree demo", "handsfree demo maybe", "handsfree demo on extra"):
            self.assertFalse(self.send(bad)["ok"], bad)

        def saved():
            return tuple(
                (self.runtime / name).read_bytes() if (self.runtime / name).exists() else None
                for name in ("hf0.bin", "hf1.bin")
            )

        before = saved()
        on = self.send("handsfree demo on")
        self.assertTrue(on["ok"])
        self.assertTrue(on["handsFree"]["demoMovementOnly"])
        after = saved()
        self.assertTrue(any(item is not None for item in before), "a configuration was saved")
        self.assertEqual(before, after, "the demo must not touch the saved configuration")
        self.assertTrue(self.send("handsfree demo off")["ok"])
        self.assertFalse(self.send("status")["handsFree"]["demoMovementOnly"])
        self.assertEqual(
            SERVER.command_for({"action": "handsfree", "op": "demo", "enabled": True}),
            "handsfree demo on",
        )
        with self.assertRaises(ValueError):
            SERVER.command_for({"action": "handsfree", "op": "demo"})

    def test_no_drag_no_click_and_bounded_steps_in_the_demo(self):
        self.setup_hands_free("maintained")
        self.quiet(60)
        self.assertTrue(self.send("handsfree demo on")["ok"])
        self.quiet(60)
        self.assertTrue(self.send("resume")["ok"])
        refused_before = self.send("status")["handsFree"]["gesture"]["refused"]
        dragging = self.send("gesture tilt2")
        self.assertFalse(dragging["handsFree"]["drag"], "a drag started in the demo")
        self.assertEqual(dragging["handsFree"]["gesture"]["refused"], refused_before + 1)
        self.assertEqual(dragging["state"], "ACTIVE")
        self.send("step 30 40 0 0 0 1 0 0")  # fast yaw
        clicks = 0
        biggest = 0
        for _ in range(8):
            result = self.send("step 50 0 0 0 0 1 0 0")  # hold still: a dwell would click
            for dx, dy, wheel, down in result["reports"]:
                clicks += bool(down)
                biggest = max(biggest, abs(dx), abs(dy))
        self.assertEqual(clicks, 0, "a dwell click in the demo")
        self.assertLessEqual(biggest, 6)

    def test_the_demo_is_off_after_a_restart_and_the_profile_keeps_dwell(self):
        self.setup_hands_free("maintained")
        self.assertTrue(self.send("handsfree demo on")["ok"])
        self.restart()
        status = self.send("status")
        self.assertFalse(status["handsFree"]["demoMovementOnly"])
        self.assertTrue(status["profile"]["dwellEnabled"])
        self.assertEqual(status["handsFree"]["mode"], "HANDS_FREE")


class SwitchAndTransport(HandsFreeCase):
    def test_switch_off_releases_immediately_and_on_never_resumes(self):
        self.setup_hands_free()
        self.activate_by_gesture()
        self.assertTrue(self.send("gesture tilt2")["handsFree"]["drag"])
        off = self.send("enable 0")
        self.assertTrue(off["ok"])
        self.assertEqual(off["state"], "PAUSED")
        self.assertEqual(off["reports"][-1], [0, 0, 0, 0], "no immediate release report")
        self.assertFalse(off["handsFree"]["drag"])
        blocked = self.quiet(100)
        self.assertEqual(blocked["handsFree"]["blocked"], "control switch is OFF")
        self.assertFalse(self.send("resume")["ok"])
        self.assertEqual(self.send("gesture nod2")["state"], "PAUSED")
        on = self.send("enable 1")
        self.assertTrue(on["ok"])
        settled = self.quiet(100)
        self.assertTrue(settled["handsFree"]["switch"]["permitted"])
        self.assertEqual(settled["state"], "PAUSED")
        self.assertEqual(self.send("gesture nod2")["state"], "ACTIVE")

    def test_step_enable_field_drives_the_switch(self):
        self.setup_hands_free()
        self.activate_by_gesture()
        off = self.send("step 5 0 0 0 0 1 0 0 0")
        self.assertEqual(off["state"], "PAUSED")
        on = self.send("step 10 0 0 0 0 1 0 0 1")
        self.assertTrue(on["handsFree"]["switch"]["on"])
        self.assertEqual(on["state"], "PAUSED")

    def test_repeated_transport_failure_keeps_output_inhibited(self):
        self.setup_hands_free()
        self.activate_by_gesture()
        for _ in range(5):
            result = self.send("step 20 40 0 0 0 1 0 6")
            self.assertEqual(result["state"], "SAFE_STATE")
            self.assertTrue(all(r[3] == 0 for r in result["reports"]))
        self.assertEqual(self.quiet(200)["state"], "READY")

    def test_serial_link_reopens_after_repeated_timeouts_and_on_request(self):
        class DeadSerial:
            closed = False

            def reset_input_buffer(self):
                pass

            def write(self, data):
                pass

            def readline(self):
                return b""

            def close(self):
                self.closed = True

        class Adapter(SERVER.SerialDevice):
            reopened = 0

            def _open(self):
                Adapter.reopened += 1
                self.serial = DeadSerial()

        import sys

        transport = sys.modules[SERVER.SerialDevice.__module__]
        original_timeout = transport.REQUEST_TIMEOUT_SECONDS
        transport.REQUEST_TIMEOUT_SECONDS = 0.01
        try:
            adapter = Adapter("fake-port")
            self.assertEqual(Adapter.reopened, 1)
            for _ in range(adapter.AUTO_REOPEN_AFTER - 1):
                with self.assertRaises(RuntimeError):
                    adapter.request("status")
            self.assertEqual(Adapter.reopened, 1, "reopened too early")
            with self.assertRaises(RuntimeError):
                adapter.request("status")
            self.assertEqual(Adapter.reopened, 2, "no automatic reopen after repeated timeouts")
            for _ in range(adapter.AUTO_REOPEN_AFTER + 2):
                with self.assertRaises(RuntimeError):
                    adapter.request("status")
            self.assertEqual(Adapter.reopened, 2, "cooldown ignored")
            adapter.reopen()  # the manual button
            self.assertEqual(Adapter.reopened, 3)
            self.assertEqual(adapter.failures, 0)
        finally:
            transport.REQUEST_TIMEOUT_SECONDS = original_timeout

    def test_serial_adapter_passes_new_commands_and_still_refuses_native_only(self):
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
        for command in ("train start pause", "handsfree commit", "enable 0", "gesture nod2 1"):
            adapter.request(command)
            self.assertTrue(adapter.serial.written.decode().endswith(command + "\n"))
            self.assertLessEqual(len(adapter.serial.written), 80)
        with self.assertRaises(ValueError):
            adapter.request("corrupt")
        with self.assertRaises(ValueError):
            adapter.request("record on")
        adapter.request("step 5 0 0 0 0 1 0 0 1")
        self.assertEqual(adapter.serial.written.split(b" ", 1)[1], b"status\n")


class ReplayAndTrials(HandsFreeCase):
    def replay(self, path, *flags):
        completed = subprocess.run(
            [str(EXE), self.tmp.name, "--replay", str(path), *flags],
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return completed.stdout

    def test_gesture_replay_is_reproducible_and_resumes_only_by_gesture(self):
        self.setup_hands_free()
        self.send("record on")
        self.send("gesture nod2")
        self.send("record off")
        path = self.runtime / "samples.csv"
        self.assertIn(",enable", path.read_text().splitlines()[0])
        self.device.close()
        first = self.replay(path, "--hands-free")
        second = self.replay(path, "--hands-free")
        self.assertEqual(first, second, "replay is not deterministic")
        rows = [json.loads(line) for line in first.splitlines()]
        self.assertEqual(rows[0]["state"], "READY")
        self.assertEqual(rows[-1]["state"], "ACTIVE")
        self.assertEqual(rows[-1]["handsFree"]["gesture"]["executed"], 1)
        self.assertEqual(len(rows), 150)
        # nothing executes during the 400 ms of stillness before the pattern
        self.assertTrue(all(r["handsFree"]["gesture"]["executed"] == 0 for r in rows[:60]))
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)

    def test_replay_with_the_switch_off_never_activates(self):
        self.setup_hands_free()
        self.send("record on")
        self.send("gesture nod2")
        self.send("record off")
        path = self.runtime / "samples.csv"
        lines = path.read_text().splitlines()
        disabled = [lines[0]] + [line.rsplit(",", 1)[0] + ",0" for line in lines[1:]]
        path.write_text("\n".join(disabled) + "\n")
        self.device.close()
        rows = [json.loads(line) for line in self.replay(path, "--hands-free").splitlines()]
        self.assertTrue(all(r["state"] != "ACTIVE" for r in rows))
        self.assertTrue(all(r["handsFree"]["gesture"]["executed"] == 0 for r in rows))
        self.device = SERVER.NativeDevice(EXE, self.tmp.name)

    def test_replay_rejects_a_bad_enable_flag(self):
        path = self.runtime / "bad.csv"
        path.write_text(
            "timestampMs,gyroX,gyroY,gyroZ,accelX,accelY,accelZ,valid,enable\n10,0,0,0,0,0,1,1,2\n"
        )
        completed = subprocess.run(
            [str(EXE), self.tmp.name, "--replay", str(path), "--hands-free"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)

    def test_trial_endpoint_enforces_interaction_context(self):
        class Dummy:
            def request(self, command):
                return {"ok": True}

            def close(self):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SERVER.Handler)
        server.runtime = self.runtime
        server.device = Dummy()
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        profile = {"gain": [30, 30, 30, 30], "dwellEnabled": True}
        context = {
            "deviceSource": "SIMULATED",
            "inputSource": "SIMULATED",
            "condition": "ADAPTIVE",
            "selectionMethod": "DWELL",
            "interactionMode": "HANDS_FREE",
            "gestureConfigId": "0badc0de",
            "profile": profile,
        }
        trial = {
            "trial": 1,
            "condition": "ADAPTIVE",
            "inputSource": "SIMULATED",
            "deviceSource": "SIMULATED",
            "selectionMethod": "DWELL",
            "interactionMode": "HANDS_FREE",
            "gestureConfigId": "0badc0de",
            "distance": 100,
            "width": 50,
            "movementTimeMs": 900,
            "hit": True,
            "aborted": False,
            "profile": profile,
            "blockContext": context,
            "gestureInterruptions": 2,
        }

        def post(data):
            request = urllib.request.Request(
                f"http://127.0.0.1:{server.server_address[1]}/api/trial",
                data=json.dumps(data).encode(),
                headers={"Content-Type": "application/json"},
            )
            try:
                with urllib.request.urlopen(request, timeout=5) as response:
                    return response.status
            except urllib.error.HTTPError as error:
                error.close()
                return error.code

        self.assertEqual(post(trial), 200)
        logged = json.loads((self.runtime / "trials.jsonl").read_text().splitlines()[-1])
        self.assertEqual(logged["interactionMode"], "HANDS_FREE")
        self.assertEqual(logged["gestureInterruptions"], 2)
        for key, value in (
            ("interactionMode", "LEGACY_SWITCH"),
            ("gestureConfigId", "00000000"),
        ):
            self.assertEqual(post({**trial, key: value}), 400, key)
        self.assertEqual(post({**trial, "interactionMode": "OTHER"}), 400)
        self.assertEqual(post({**trial, "gestureInterruptions": -1}), 400)
        self.assertEqual(post({**trial, "gestureInterruptions": 1.5}), 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
