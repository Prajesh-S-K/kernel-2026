"""Overlay bridge client, worker ordering, controller dispatch, keyboard helper, and a real end-to-end run
against the simulated companion bridge over HTTP. No AppKit needed."""

import importlib.util
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from overlay import keyboard as kb  # noqa: E402
from overlay import logic as L  # noqa: E402
from overlay.bridge import Bridge, Worker, body_for  # noqa: E402
from overlay.controller import Controller  # noqa: E402

SPEC = importlib.util.spec_from_file_location("server", ROOT / "desktop/server.py")
SERVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SERVER)
EXE = Path(os.environ.get("NODX_SIM", ROOT / "build/nodx_sim"))


class FakeBridge:
    def __init__(self, fail=False):
        self.bodies = []
        self.fail = fail

    def post(self, body):
        self.bodies.append(body)
        return None if self.fail else {"ok": True, "state": "ACTIVE", "actions": {}}


def drain(worker, seconds=1.0):
    deadline = time.monotonic() + seconds
    got = []
    while time.monotonic() < deadline:
        got += worker.drain()
        if not worker._queue and got:
            time.sleep(0.05)
            got += worker.drain()
            break
        time.sleep(0.01)
    return got


class BodyMapping(unittest.TestCase):
    def test_every_overlay_body_is_accepted_by_the_server_and_maps_to_the_firmware_command(self):
        session, epoch = 7, 1234
        expected = {
            ("status", None): "status",
            ("overlay", True): f"actions overlay on {session} {epoch}",
            ("overlay", False): f"actions overlay off {epoch}",
            ("menu", True): f"actions menu open {epoch}",
            ("menu", False): f"actions menu close {epoch}",
            ("keyboard", True): f"actions keyboard on {epoch}",
            ("keyboard", False): f"actions keyboard off {epoch}",
        }
        for target in ("left", "right", "double", "drag", "drop", "scroll", "cancel", "stop"):
            expected[("select", target)] = f"actions select {target} {epoch}"
        for (kind, value), command in expected.items():
            self.assertEqual(SERVER.command_for(body_for(kind, value, session, epoch)), command)
        with self.assertRaises(ValueError):
            body_for("click", True, session, epoch)
        # a command without its token cannot even be built
        for kind, value in (
            ("menu", True),
            ("select", "left"),
            ("keyboard", True),
            ("overlay", False),
        ):
            with self.assertRaises(ValueError):
                body_for(kind, value, session, None)
        with self.assertRaises(ValueError):
            body_for("overlay", True, None, epoch)  # a claim needs the session serial
        # nothing the overlay can build is a mouse or serial primitive
        for kind in ("status", "overlay", "menu", "select", "keyboard"):
            self.assertIn(body_for(kind, "left", 1, 1).get("action"), ("status", "actions"))

    def test_menu_items_the_overlay_selects_are_all_valid_targets(self):
        for ident, _label in L.MENU_ITEMS:
            if ident in L.SELECT_TARGETS:
                SERVER.command_for(body_for("select", ident, 1, 1))


class WorkerOrdering(unittest.TestCase):
    def test_a_selection_is_never_reordered_behind_a_later_menu_report(self):
        bridge = FakeBridge()
        worker = Worker(bridge)
        worker.set_epoch(5)
        # queue before the thread runs so everything is pending together
        worker.send("menu", True, 1, 5)
        worker.send("select", "right", 1, 5)
        worker.send("menu", False, 1, 5)
        worker.send("menu", False, 1, 5)  # a repeat of the tail: coalesced
        worker.start()
        drain(worker)
        worker.stop()
        kinds = [(b["op"], b.get("open", b.get("target"))) for b in bridge.bodies]
        self.assertEqual(kinds, [("menu", True), ("select", "right"), ("menu", False)])
        self.assertTrue(all(b["epoch"] == 5 for b in bridge.bodies))

    def test_failures_are_reported_as_none_and_counted(self):
        worker = Worker(FakeBridge(fail=True))
        worker.start()
        worker.send("status")
        got = drain(worker)
        worker.stop()
        self.assertEqual(len(got), 1)
        self.assertIsNone(got[0][1])
        self.assertEqual(worker.failed, 1)

    def test_traffic_is_kept_flowing(self):
        bridge = FakeBridge()
        worker = Worker(bridge)
        worker.start()
        now = time.monotonic()
        worker.ensure_traffic(now)  # nothing sent for a long time: a status poll goes out
        drain(worker)
        worker.ensure_traffic(time.monotonic())  # just sent: no new poll yet
        time.sleep(0.1)
        worker.stop()
        self.assertEqual([b["action"] for b in bridge.bodies], ["status"])


class FakeWorker:
    def __init__(self):
        self.sent = []
        self.replies = []

    def drain(self):
        out, self.replies = self.replies, []
        return out

    def set_epoch(self, epoch):
        self.epoch = epoch

    def send(self, kind, value=None, session=None, epoch=None):
        self.sent.append((kind, value))

    def ensure_traffic(self, now):
        self.sent.append(("poll", None))


class FakeKeyboard:
    SETUP = kb.SETUP_TEXT

    def __init__(self, running):
        self.running = running
        self.opened = 0

    def keyboard_running(self):
        return self.running

    def open_settings(self):
        self.opened += 1
        return True

    switched = 0
    switch_works = True

    def switch_on(self):
        self.switched += 1
        return self.switch_works

    def __getattr__(self, name):  # the message helpers live in the real module
        return getattr(kb, name)


def session_status(**kw):
    a = {"controller": "OVERLAY", "keyboard": False, "mode": "LEFT", "dwellMs": 1200}
    a.update(kw)
    return {"state": "ACTIVE", "handsFree": {"uncalDemo": {"active": True}}, "actions": a}


class ControllerKeyboard(unittest.TestCase):
    def make(self, running, clicks="macos", **kw):
        model = L.OverlayModel(clicks)
        model.on_reply(session_status(**kw), 10.0)
        worker = FakeWorker()
        fake = FakeKeyboard(running)
        return Controller(worker, model, keyboard=fake), worker, fake, model

    def test_keyboard_running_enters_keyboard_mode_and_pauses_nodx_clicks(self):
        c, w, k, m = self.make(True)
        c._keyboard(10.0)
        self.assertIn(("keyboard", True), w.sent)
        self.assertEqual(k.opened, 0)
        self.assertIn("NodX clicks are paused", m.notice)

    def test_keyboard_not_running_opens_settings_and_never_claims_it_opened(self):
        c, w, k, m = self.make(False)
        c._keyboard(10.0)
        self.assertEqual(k.opened, 1)
        self.assertNotIn(("keyboard", True), w.sent)
        self.assertIn("not running", m.notice)
        self.assertIn("One-time setup", m.notice)

    def switching(self, running=False, clicks="macos"):
        c, w, k, m = self.make(running, clicks)
        c.opener = "switch"
        m.claimed = True
        return c, w, k, m

    def test_switch_asks_macos_then_enters_keyboard_mode_once_the_host_runs(self):
        c, w, k, m = self.switching()
        c._keyboard(10.0)
        self.assertEqual((k.switched, k.opened), (1, 0))
        self.assertNotIn(("keyboard", True), w.sent, "asked, not yet opened")
        self.assertIn("asked macOS", m.notice)
        k.running = True
        c._watch_keyboard(11.0)
        self.assertIn(("keyboard", True), w.sent)
        self.assertIn("cannot see whether a keyboard is on screen", m.notice)

    def test_switch_falls_back_to_settings_when_the_host_never_starts(self):
        c, w, k, m = self.switching()
        c._keyboard(10.0)
        c._watch_keyboard(12.0)
        self.assertEqual(k.opened, 0, "still waiting")
        c._watch_keyboard(15.0)
        self.assertEqual(k.opened, 1)
        self.assertNotIn(("keyboard", True), w.sent)
        self.assertIn("did not start", m.notice)
        self.assertIn("One-time setup", m.notice)

    def test_a_failed_preference_write_opens_settings_at_once(self):
        c, w, k, m = self.switching()
        k.switch_works = False
        c._keyboard(10.0)
        self.assertEqual(k.opened, 1)
        self.assertIsNone(c._opening_until)

    def test_a_lost_claim_cancels_a_pending_keyboard_open(self):
        c, w, k, m = self.switching()
        c._keyboard(10.0)
        m.claimed = False
        k.running = True
        c._watch_keyboard(11.0)
        self.assertNotIn(("keyboard", True), w.sent)
        self.assertIsNone(c._opening_until)

    def test_settings_opener_never_writes_a_preference(self):
        c, w, k, m = self.make(False)
        c._keyboard(10.0)
        self.assertEqual((k.switched, k.opened), (0, 1))

    def test_the_preference_command_sets_only_the_keyboard_switch(self):
        seen = []

        class Done:
            returncode = 0

        def runner(cmd, **kw):
            seen.append(cmd)
            return Done()

        self.assertTrue(kb.switch_on(runner))
        self.assertEqual(
            seen, [["defaults", "write", "com.apple.universalaccess", "virtualKeyboardOnOff", "-bool", "true"]]
        )

    def test_selecting_keyboard_again_is_the_way_back_to_nodx_clicks(self):
        c, w, k, m = self.make(True, keyboard=True)
        c._keyboard(10.0)
        self.assertEqual([x for x in w.sent if x[0] == "keyboard"], [("keyboard", False)])

    def test_nodx_click_mode_never_pauses_clicks(self):
        c, w, k, m = self.make(True, clicks="nodx")
        c._keyboard(10.0)
        self.assertNotIn(("keyboard", True), w.sent)
        self.assertEqual(k.opened, 0)

    def test_replies_are_fed_to_the_model_and_failures_are_ignored(self):
        c, w, k, m = self.make(True)
        w.replies = [(1.0, None, {}, None), (1.1, {"error": "x"}, {}, None)]
        c.tick(11.0, (0, 0), L.collapsed_layout(L.Rect(0, 0, L.TILE_W, L.TILE_H)))
        self.assertEqual(m.last_ok, 10.0, "an unusable reply must not count as contact")


class KeyboardHelper(unittest.TestCase):
    def test_detection_uses_only_the_host_process_and_needs_no_permission(self):
        self.assertTrue(kb.keyboard_running([("Assistive Control", "com.apple.AssistiveControl")]))
        self.assertTrue(kb.keyboard_running([("Something", "com.apple.assistivecontrol.helper")]))
        self.assertFalse(
            kb.keyboard_running([("Finder", "com.apple.finder"), ("Safari", "com.apple.Safari")])
        )
        self.assertFalse(kb.keyboard_running([]))
        self.assertFalse(
            kb.keyboard_running([("Accessibility", "com.apple.AccessibilityUIServer")])
        )

    def test_settings_deep_link_is_the_only_thing_opened(self):
        calls = []

        class Done:
            returncode = 0

        def runner(cmd, **kw):
            calls.append(cmd)
            return Done()

        self.assertTrue(kb.open_settings(runner))
        self.assertEqual(calls, [["open", kb.SETTINGS_URL]])
        self.assertTrue(kb.SETTINGS_URL.startswith("x-apple.systempreferences:"))

        def boom(cmd, **kw):
            raise OSError("no open")

        self.assertFalse(kb.open_settings(boom))

    def test_texts_are_honest_about_setup(self):
        self.assertIn("One-time setup", kb.SETUP_TEXT)
        self.assertIn("Accessibility Keyboard", kb.SETUP_TEXT)
        self.assertIn("not running", kb.MISSING_TEXT)

    def test_with_nodx_clicking_the_setup_says_to_leave_macos_dwell_off(self):
        nodx = kb.missing_text("nodx")
        self.assertIn("not running", nodx)
        self.assertIn("OFF", nodx)
        self.assertNotIn("turn on its dwell", nodx)
        self.assertIn("turn on its dwell", kb.missing_text("macos"))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class EndToEnd(unittest.TestCase):
    """The real overlay path (model + controller + worker + HTTP bridge) against the simulated companion."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.port = free_port()
        env = dict(os.environ, NODX_SIM=str(EXE))
        cls.server = subprocess.Popen(
            [
                sys.executable,
                str(ROOT / "desktop/server.py"),
                "--port",
                str(cls.port),
                "--runtime",
                cls.tmp.name,
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.url = f"http://127.0.0.1:{cls.port}/api/device"
        bridge = Bridge(cls.url)
        for _ in range(50):
            if bridge.post({"action": "status"}):
                break
            time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(5)
        cls.tmp.cleanup()

    def setUp(self):
        self.bridge = Bridge(self.url)

    def test_the_overlay_claims_selects_and_stops_through_the_bridge_only(self):
        post = self.bridge.post
        for _ in range(12):  # the simulator needs healthy sensor samples before a session can start
            post({"action": "step"})
        self.assertTrue(post({"action": "handsfree", "op": "uncal", "enabled": True})["ok"])
        worker = Worker(self.bridge)
        worker.start()
        model = L.OverlayModel("macos")
        controller = Controller(worker, model, keyboard=FakeKeyboard(False))
        tile = L.default_tile_frame(L.Rect(0, 0, 1440, 870), "right")
        pointer = [10.0, 10.0]

        def run(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                now = time.monotonic()
                if model.state == "menu":
                    layout = L.expanded_layout(tile, [L.Rect(0, 0, 1440, 870)])
                else:
                    layout = L.collapsed_layout(tile)
                controller.tick(now, tuple(pointer), layout)
                time.sleep(0.02)

        run(1.0)
        self.assertEqual(model.state, "unclaimed", "the tile did not appear for an active session")
        pointer[:] = [tile.x + 30, tile.y + 20]  # the pointer rests on the tile
        run(1.9)
        self.assertEqual(model.state, "menu")
        status = post({"action": "status"})["actions"]
        self.assertEqual(status["controller"], "OVERLAY")
        self.assertTrue(
            status["menu"] and status["ready"], "the device did not acknowledge the menu"
        )
        layout = L.expanded_layout(tile, [L.Rect(0, 0, 1440, 870)])
        r = layout.items["right"]
        pointer[:] = [layout.frame.x + r.x + r.w / 2, layout.frame.top - (r.y + r.h / 2)]
        run(1.9)  # a validated selection through the bridge
        self.assertEqual(post({"action": "status"})["actions"]["mode"], "RIGHT")
        self.assertEqual(model.state, "tile", "the menu did not collapse after the selection")
        # the bridge refuses a browser takeover while the overlay controls
        self.assertFalse(post({"action": "actions", "op": "enable", "enabled": True})["ok"])
        # choose Stop from the menu
        pointer[:] = [tile.x + 30, tile.y + 20]
        run(0.4)
        pointer[:] = [tile.x - 300, tile.y]
        run(0.5)
        pointer[:] = [tile.x + 30, tile.y + 20]
        run(1.9)
        self.assertEqual(model.state, "menu")
        layout = L.expanded_layout(tile, [L.Rect(0, 0, 1440, 870)])
        r = layout.items["stop"]
        pointer[:] = [layout.frame.x + r.x + r.w / 2, layout.frame.top - (r.y + r.h / 2)]
        run(1.9)
        worker.stop()
        final = post({"action": "status"})
        self.assertNotEqual(final["state"], "ACTIVE", "Pause / Stop did not stop the session")
        self.assertEqual(final["actions"]["controller"], "NONE")
        self.assertEqual(worker.failed, 0)
        self.assertEqual(L.session_active(final), False)

    def test_the_overlay_never_uses_a_serial_port_or_a_second_mouse_path(self):
        files = {p.name: p.read_text() for p in (ROOT / "overlay").glob("*.py")}
        text = "\n".join(files.values())
        # Key events are posted in exactly one place (the NodX keyboard's typer), and never mouse events.
        for name, content in files.items():
            if name != "typing.py":
                self.assertNotIn("CGEventPost", content, name)
        self.assertNotIn("Mouse", files["typing.py"])
        for forbidden in (
            "import serial",
            "serial.Serial",
            "pyserial",
            "/dev/cu.",
            "CGEventCreateMouseEvent",
            "CGPostMouseEvent",
            "NSEventTypeLeftMouseDown",
            "pyautogui",
            "AXUIElementPerformAction",
        ):
            self.assertNotIn(forbidden, text, forbidden)


if __name__ == "__main__":
    unittest.main(verbosity=2)
