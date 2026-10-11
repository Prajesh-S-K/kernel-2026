"""Failure paths of the overlay: stale commands and replies after Stop, timeout and reconnect, and keyboard
recovery. Pure Python plus a real simulated companion bridge for the gated end-to-end scenarios."""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_overlay_bridge import FakeBridge, FakeKeyboard, FakeWorker, drain  # noqa: E402
from test_overlay_logic import MAIN, Harness, status  # noqa: E402

from overlay import keyboard as kb  # noqa: E402
from overlay import logic as L  # noqa: E402
from overlay.bridge import Bridge, Worker  # noqa: E402
from overlay.controller import Controller  # noqa: E402

EXE = Path(os.environ.get("NODX_SIM", ROOT / "build/nodx_sim"))
TILE = L.default_tile_frame(MAIN, "right")


def layout_for(model):
    if model.state == "menu":
        return L.expanded_layout(TILE, [MAIN])
    return L.collapsed_layout(TILE)


class QueuedCommands(unittest.TestCase):
    def test_commands_queued_under_an_old_epoch_are_dropped_not_sent(self):
        bridge = FakeBridge()
        worker = Worker(bridge)
        worker.set_epoch(10)
        worker.send(
            "overlay", True, 3, 10
        )  # a claim, a menu report, a selection, a keyboard command
        worker.send("menu", True, 3, 10)
        worker.send("select", "right", 3, 10)
        worker.send("keyboard", True, 3, 10)
        worker.set_epoch(
            11
        )  # Stop, timeout or reconnect: a new epoch begins before the thread sends anything
        worker.start()
        drain(worker, 0.5)
        worker.stop()
        self.assertEqual(bridge.bodies, [], "stale commands were sent")
        self.assertEqual(worker.dropped, 4)

    def test_only_the_current_epoch_is_sent_and_order_is_kept(self):
        bridge = FakeBridge()
        worker = Worker(bridge)
        worker.set_epoch(10)
        worker.send("menu", True, 3, 10)  # stale
        worker.set_epoch(11)
        worker.send("overlay", True, 3, 11)
        worker.send("select", "left", 3, 11)
        worker.start()
        drain(worker)
        worker.stop()
        self.assertEqual(
            [(b["op"], b["epoch"]) for b in bridge.bodies], [("overlay", 11), ("select", 11)]
        )

    def test_menu_reports_of_different_epochs_are_never_coalesced(self):
        bridge = FakeBridge()
        worker = Worker(bridge)
        worker.set_epoch(10)
        worker.send("menu", True, 3, 10)
        worker.send("menu", False, 3, 10)  # same epoch at the tail: coalesced
        worker.send("menu", True, 3, 10)
        self.assertEqual(len(worker._queue), 1)
        worker.set_epoch(11)
        worker.send("menu", True, 3, 11)
        self.assertEqual(len(worker._queue), 2, "a new epoch's report replaced an old one")


class EpochsInTheModel(unittest.TestCase):
    def test_the_epoch_advances_on_loss_stop_and_never_while_idle(self):
        h = Harness()
        h.step()
        start = h.model.epoch
        h.run(1.0)
        self.assertEqual(h.model.epoch, start, "the epoch moved while nothing happened")
        h.model.on_reply(status(active=False), h.now)  # Stop: the session ended
        h.step()
        self.assertEqual(h.model.epoch, start + 1)
        h.run(2.0)
        self.assertEqual(h.model.epoch, start + 1, "the epoch moved again while hidden")
        h.set_status()
        h.step()
        self.assertEqual(h.model.state, "unclaimed")
        # a loss starts another epoch
        h.run(3.5, reply=False)
        self.assertEqual(h.model.state, "lost")
        self.assertEqual(h.model.epoch, start + 2)

    def test_a_claim_goes_above_everything_the_device_already_accepted(self):
        h = Harness()
        h.model.epoch = 5000
        h.model.on_reply(
            {**status(), "actions": {**status()["actions"], "epochFloor": 9000, "session": 4}},
            h.now,
        )
        h.step()
        h.on_tile()
        cmds = h.run(1.5)
        claim = next(c for c in cmds if c.kind == "overlay")
        self.assertGreater(claim.epoch, 9000)
        self.assertEqual(claim.session, 4)
        self.assertEqual(h.model.epoch, claim.epoch)
        for c in cmds:
            if c.kind in ("menu", "select"):
                self.assertEqual(c.epoch, claim.epoch)
                self.assertEqual(c.session, 4)

    def test_a_restarted_overlay_starts_above_an_older_one(self):
        first = L.OverlayModel()
        time.sleep(0.01)
        self.assertGreaterEqual(L.OverlayModel().epoch, first.epoch)


class StaleReplies(unittest.TestCase):
    def make(self):
        model = L.OverlayModel("macos", epoch0=100)
        worker = FakeWorker()
        controller = Controller(worker, model, keyboard=FakeKeyboard(False))
        return controller, worker, model

    def test_a_delayed_active_reply_after_stop_does_not_bring_the_tile_back(self):
        controller, worker, model = self.make()
        model.on_reply(status(), 1.0)
        controller.tick(1.0, (0, 0), layout_for(model))
        self.assertEqual(model.state, "unclaimed")
        model.on_reply(status(active=False), 1.1)  # the Stop is seen
        controller.tick(1.1, (0, 0), layout_for(model))
        self.assertEqual(model.state, "hidden")
        stale_epoch = 100  # the epoch that was valid before the Stop
        worker.replies = [(1.2, status(menu=True, ready=True), {}, stale_epoch)]
        controller.tick(1.2, (0, 0), layout_for(model))
        self.assertEqual(
            model.state, "hidden", "a delayed reply from the old session showed the tile"
        )
        self.assertEqual(controller.stale_replies, 1)
        self.assertFalse(model.claimed)

    def test_a_delayed_reply_cannot_reopen_the_menu_or_reclaim_after_a_loss(self):
        h = Harness()
        h.step()
        h.on_tile()
        h.run(1.5)
        old_epoch = h.model.epoch
        h.set_status(menu=True, ready=True)
        h.away()
        h.run(3.5, reply=False)  # contact lost: a new epoch
        self.assertEqual(h.model.state, "lost")
        worker = FakeWorker()
        controller = Controller(worker, h.model, keyboard=FakeKeyboard(False))
        worker.replies = [
            (h.now, status(menu=True, ready=True, controller="OVERLAY"), {}, old_epoch)
        ]
        before = list(h.commands)
        controller.tick(h.now + 0.1, (0, 0), layout_for(h.model))
        self.assertEqual(h.model.state, "lost", "a delayed reply revived the overlay")
        self.assertFalse(h.model.claimed)
        self.assertEqual(h.commands, before)
        self.assertEqual(controller.stale_replies, 1)
        # a current-epoch reply is accepted, but it still does not claim or reopen anything by itself
        worker.replies = [(h.now, status(), {}, h.model.epoch)]
        controller.tick(h.now + 0.2, (0, 0), layout_for(h.model))
        self.assertEqual(h.model.state, "lost")
        self.assertFalse(h.model.claimed)

    def test_the_worker_epoch_follows_the_model_every_tick(self):
        controller, worker, model = self.make()
        model.on_reply(status(), 1.0)
        controller.tick(1.0, (0, 0), layout_for(model))
        self.assertEqual(worker.epoch, model.epoch)
        model.on_reply(status(active=False), 1.1)
        controller.tick(1.1, (0, 0), layout_for(model))
        self.assertEqual(worker.epoch, model.epoch)


class GatedBridge(Bridge):
    """A real bridge client whose requests can be held in flight (a slow link, a queue that drains late)."""

    def __init__(self, url):
        super().__init__(url, timeout=8.0)
        self.gate = threading.Event()
        self.gate.set()
        self.log = []

    def post(self, body):
        if not self.gate.is_set():
            self.gate.wait(8.0)
        reply = super().post(body)
        self.log.append((body, reply))
        return reply


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class GatedEndToEnd(unittest.TestCase):
    """Commands the overlay had queued or in flight arrive AFTER a Stop and an explicit restart. The device must
    refuse every one of them, and the overlay must not reclaim, reopen the menu or run an action by itself."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.port = free_port()
        cls.server = subprocess.Popen(
            [
                sys.executable,
                str(ROOT / "desktop/server.py"),
                "--port",
                str(cls.port),
                "--runtime",
                cls.tmp.name,
            ],
            env=dict(os.environ, NODX_SIM=str(EXE)),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.url = f"http://127.0.0.1:{cls.port}/api/device"
        probe = Bridge(cls.url)
        for _ in range(60):
            if probe.post({"action": "status"}):
                break
            time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        cls.server.wait(5)
        cls.tmp.cleanup()

    def step(self, n=12):
        for _ in range(n):
            self.plain.post({"action": "step"})

    def setUp(self):
        self.plain = Bridge(self.url)
        self.plain.post({"action": "handsfree", "op": "uncal", "enabled": False})
        self.step()

    def start_session(self):
        reply = self.plain.post({"action": "handsfree", "op": "uncal", "enabled": True})
        self.assertTrue(reply["ok"], reply)
        return reply

    def actions(self):
        return self.plain.post({"action": "status"})["actions"]

    def test_commands_in_flight_during_stop_and_restart_are_all_refused(self):
        self.start_session()
        session = self.actions()["session"]
        gated = GatedBridge(self.url)
        worker = Worker(gated)
        worker.start()
        model = L.OverlayModel("macos", epoch0=7000)
        controller = Controller(worker, model, keyboard=FakeKeyboard(False))
        pointer = [10.0, 10.0]

        def run(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                controller.tick(time.monotonic(), tuple(pointer), layout_for(model))
                time.sleep(0.02)

        run(1.0)
        pointer[:] = [TILE.x + 30, TILE.y + 20]
        run(1.9)
        self.assertEqual(model.state, "menu")
        self.assertEqual(self.actions()["controller"], "OVERLAY")
        # the link now stalls: everything the overlay sends from here on is held in flight
        gated.gate.clear()
        layout = L.expanded_layout(TILE, [MAIN])
        item = layout.items["right"]
        pointer[:] = [
            layout.frame.x + item.x + item.w / 2,
            layout.frame.top - (item.y + item.h / 2),
        ]
        run(1.9)  # the selection is queued/held
        # meanwhile the user presses the website Stop and then starts a new session explicitly
        self.plain.post({"action": "handsfree", "op": "uncal", "enabled": False})
        self.step()
        self.start_session()
        self.assertEqual(self.actions()["controller"], "NONE")
        self.assertNotEqual(self.actions()["session"], session)
        # the old overlay's held commands drain now
        gated.gate.set()
        run(2.5)
        worker.stop()
        refused = [
            (b["op"], b.get("target"), r.get("ok"))
            for b, r in gated.log
            if b.get("action") == "actions" and b["epoch"] >= 7000
        ]
        self.assertTrue(refused, "nothing was held in flight; the scenario did not run")
        late = self.actions()
        self.assertEqual(late["controller"], "NONE", "a late command reclaimed the controls")
        self.assertFalse(late["enabled"])
        self.assertEqual(late["mode"], "LEFT", "a late selection executed")
        self.assertFalse(late["menu"], "a late menu report reopened the menu")
        self.assertEqual(late["counts"]["right"] + late["counts"]["left"], 0)
        # nothing the device accepted AFTER the new session began came from the old overlay
        after = [
            (b["op"], r["ok"])
            for b, r in gated.log
            if b.get("action") == "actions" and b["op"] in ("overlay", "select", "menu", "keyboard")
        ]
        self.assertTrue(all(ok is False for op, ok in after[-3:]), after[-3:])
        # and the overlay itself did not claim again on its own
        self.assertFalse(model.claimed)
        self.assertNotEqual(model.state, "menu")

    def test_a_delayed_claim_from_before_a_timeout_cannot_reclaim_in_the_same_session(self):
        self.start_session()
        status0 = self.actions()
        session = status0["session"]
        reply = self.plain.post(
            {
                "action": "actions",
                "op": "overlay",
                "enabled": True,
                "session": session,
                "epoch": 9001,
            }
        )
        self.assertTrue(reply["ok"])
        # the overlay stops reporting and the device times it out: each simulator step is 50 ms, so 140 steps are 7 s
        for _ in range(140):
            self.plain.post({"action": "step"})
        self.assertEqual(self.actions()["controller"], "NONE")
        again = self.plain.post(
            {
                "action": "actions",
                "op": "overlay",
                "enabled": True,
                "session": session,
                "epoch": 9001,
            }
        )
        self.assertFalse(again["ok"], "a delayed claim (same epoch) reclaimed after the timeout")
        for body in (
            {"action": "actions", "op": "menu", "open": True, "epoch": 9001},
            {"action": "actions", "op": "select", "target": "right", "epoch": 9001},
            {"action": "actions", "op": "keyboard", "enabled": True, "epoch": 9001},
        ):
            self.assertFalse(self.plain.post(body)["ok"], body)
        self.assertEqual(self.actions()["controller"], "NONE")
        fresh = self.plain.post(
            {
                "action": "actions",
                "op": "overlay",
                "enabled": True,
                "session": session,
                "epoch": 9002,
            }
        )
        self.assertTrue(fresh["ok"], "the explicit new claim was refused")
        self.plain.post({"action": "actions", "op": "overlay", "enabled": False, "epoch": 9002})


class KeyboardRecovery(unittest.TestCase):
    def test_process_detection_never_claims_a_keyboard_is_visible(self):
        for text in (kb.OPENED_TEXT, L.KEYBOARD_HINT):
            low = text.lower()
            self.assertTrue("cannot see" in low or "only knows" in low, text)
            self.assertIn(
                "not that a keyboard is on screen" if "only knows" in low else "on screen", low
            )
        # the wording never says the keyboard "opened" or "is open"
        for text in (kb.OPENED_TEXT, L.KEYBOARD_HINT, kb.CLOSED_TEXT, kb.GONE_TEXT):
            self.assertNotRegex(
                text.lower(), r"keyboard (is )?(now )?(open|opened|visible|shown)\b"
            )

    def test_the_recovery_instruction_names_every_way_back(self):
        for text in (kb.OPENED_TEXT, L.KEYBOARD_HINT):
            self.assertIn("Keyboard again", text)
            self.assertIn("Cancel", text)
            self.assertIn("NodX clicks back", text)
        self.assertIn("Pause / Stop", L.KEYBOARD_HINT)
        self.assertIn("physical button", L.KEYBOARD_HINT)

    def test_the_hint_stays_visible_for_as_long_as_keyboard_mode_lasts(self):
        h = Harness()
        h.model.claimed, h.model.state = True, "tile"
        h.set_status(keyboard=True)
        self.assertEqual(h.model.view(h.now).notice, L.KEYBOARD_HINT)
        self.assertEqual(h.model.view(h.now + 10_000).notice, L.KEYBOARD_HINT, "the hint expired")
        h.set_status(keyboard=False)
        self.assertEqual(h.model.view(h.now).notice, "")

    def test_cancel_and_stop_stay_reachable_in_keyboard_mode(self):
        for item in ("cancel", "stop", "left", "keyboard"):
            h = Harness()
            h.set_status(keyboard=True, controller="OVERLAY")
            h.model.claimed, h.model.state = True, "tile"
            h.model.claim_sent_at = -1e9
            h.step()
            h.on_tile()
            h.run(1.5)
            self.assertEqual(h.model.state, "menu", "the menu did not open in keyboard mode")
            h.set_status(keyboard=True, controller="OVERLAY", menu=True, ready=True)
            h.on_item(item)
            h.run(1.6)
            expected = ("keyboard_toggle", None) if item == "keyboard" else ("select", item)
            picked = [
                (c.kind, c.value) for c in h.commands if c.kind in ("select", "keyboard_toggle")
            ]
            self.assertEqual(picked, [expected], item)

    def test_keyboard_again_and_a_vanished_host_both_give_clicks_back(self):
        model = L.OverlayModel("macos", epoch0=1)
        model.on_reply(status(keyboard=True, session=2), 10.0)
        model.claimed, model.state = True, "tile"
        worker = FakeWorker()
        controller = Controller(worker, model, keyboard=FakeKeyboard(True))
        controller._keyboard(10.0)  # Keyboard chosen again while keyboard mode is on
        self.assertIn(("keyboard", False), worker.sent)
        self.assertIn("does NOT switch off", model.notice)
        # the host process disappears while keyboard mode lasts
        worker2 = FakeWorker()
        gone = FakeKeyboard(False)
        c2 = Controller(worker2, model, keyboard=gone)
        model.notice, model.notice_until = "", 0
        model.on_reply(status(keyboard=True, session=2), 20.0)
        c2.tick(20.0, (0, 0), layout_for(model))
        self.assertIn(("keyboard", False), worker2.sent)
        self.assertIn("stopped running", model.notice)

    def test_the_keyboard_command_carries_the_current_epoch(self):
        model = L.OverlayModel("macos", epoch0=4242)
        model.on_reply(status(session=9), 10.0)
        worker = FakeWorker()
        controller = Controller(worker, model, keyboard=FakeKeyboard(True))
        calls = []
        worker.send = lambda kind, value=None, session=None, epoch=None: calls.append(
            (kind, value, session, epoch)
        )
        controller._keyboard(10.0)
        self.assertEqual(calls, [("keyboard", True, 9, 4242)])


class DwellClickGenerators(unittest.TestCase):
    def test_the_documentation_explains_two_click_generators_and_how_to_avoid_them(self):
        raw = (ROOT / "docs" / "OVERLAY.md").read_text()
        text = re.sub(
            r"[>*\s]+", " ", raw
        ).lower()  # ignore markdown emphasis, quoting and line breaks
        for needle in (
            "does not switch off",
            "two click generators",
            "host process is running",
            "not that a keyboard is on screen",
            "dwell control",
            "exactly one click",
            "choose one",
        ):
            self.assertIn(needle, text, needle)


if __name__ == "__main__":
    unittest.main(verbosity=2)
