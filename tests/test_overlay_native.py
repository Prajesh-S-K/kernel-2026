"""Native overlay properties. Needs PyObjC (the overlay's own environment): skipped everywhere else.

.venv-overlay/bin/python -m unittest tests.test_overlay_native
"""

import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

try:
    from overlay import app
except Exception:  # PyObjC (or macOS) missing
    app = None


@unittest.skipIf(app is None, "PyObjC is not installed in this environment")
class NativePanel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.result = app.selftest(cls.directory.name)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_the_panel_floats_never_takes_focus_and_absorbs_clicks(self):
        r = self.result
        self.assertGreaterEqual(r["level"], 25, "must float above ordinary windows")
        self.assertTrue(r["floating"])
        self.assertTrue(r["nonactivating"])
        self.assertFalse(r["canBecomeKey"])
        self.assertFalse(r["canBecomeMain"])
        self.assertFalse(
            r["ignoresMouseEvents"], "clicks must be absorbed by the overlay, not passed through"
        )
        self.assertFalse(r["hidesOnDeactivate"])

    def test_it_follows_spaces_and_may_sit_above_full_screen_apps(self):
        self.assertTrue(self.result["canJoinAllSpaces"])
        self.assertTrue(self.result["fullScreenAuxiliary"])

    def test_a_clean_quit_releases_the_claim_with_the_epoch_in_force(self):
        class Recorder:
            def __init__(self):
                self.bodies = []

            def post(self, body):
                self.bodies.append(body)
                return {"ok": True}

        class W:
            def __init__(self, bridge):
                self.bridge = bridge

            def stop(self):
                pass

            def drain(self):
                return []

            def send(self, *a, **k):
                pass

            def ensure_traffic(self, *a):
                pass

            def set_epoch(self, *a):
                pass

        recorder = Recorder()
        model = app.L.OverlayModel("macos", epoch0=555)
        model.on_reply({"state": "ACTIVE", "actions": {"session": 4}}, 1.0)
        panel_app = app.OverlayApp(app.Controller(W(recorder), model), "right")
        panel_app.panel.setFrameOrigin_((-20000.0, -20000.0))
        panel_app.release_controls()
        self.assertEqual(
            [(b["op"], b.get("open", b.get("enabled")), b["epoch"]) for b in recorder.bodies],
            [("menu", False, 555), ("overlay", False, 555)],
        )

    def test_every_state_renders(self):
        self.assertEqual(len(self.result["png"]), 8)
        for path in self.result["png"]:
            self.assertGreater(Path(path).stat().st_size, 2000, path)

    def test_the_real_displays_are_reported_in_points(self):
        self.assertGreaterEqual(len(self.result["screens"]), 1)
        for screen in self.result["screens"]:
            self.assertGreater(screen["visible"][2], 300)
        for scale in self.result["scale"]:
            self.assertIn(scale, (1.0, 2.0, 3.0))


class FakeDevice:
    """Just enough of the firmware's overlay behaviour: a claim, fresh menu reports, acknowledged selections."""

    def __init__(self, ready=True):
        self.controller, self.reporting, self.reported_at = "NONE", False, -9.0
        self.floor, self.selected, self.ready_flag, self.clock = 0, [], ready, None

    def post(self, body):
        now = self.clock()
        if body.get("action") == "actions":
            op = body["op"]
            if op == "overlay":
                self.controller = "OVERLAY" if body["enabled"] else "NONE"
                self.floor = max(self.floor, body["epoch"])
            elif op == "menu":
                self.reporting, self.reported_at = body["open"], now
            elif op == "select":
                self.selected.append(body["target"])
        menu = self.controller == "OVERLAY" and self.reporting and now - self.reported_at <= 2.5
        if body.get("action") != "status":
            menu = False  # the firmware builds a command's reply before its next tick: not yet 'fresh'
        return {
            "state": "ACTIVE",
            "handsFree": {"uncalDemo": {"active": True}},
            "actions": {
                "controller": self.controller, "mode": "LEFT", "keep": False, "dragging": False,
                "scroll": "OFF", "keyboard": False, "menu": menu, "ready": menu and self.ready_flag,
                "dwellMs": 300, "session": 3, "epoch": self.floor, "epochFloor": self.floor,
                "exit": {"progress": 0.0},
            },
        }  # fmt: skip


@unittest.skipIf(app is None, "PyObjC is not installed in this environment")
class MenuDwellThroughThePanel(unittest.TestCase):
    """Open the tile, move onto an item and finish its dwell through the real panel, controller and worker."""

    def setUp(self):
        import time

        from overlay.bridge import Worker

        self.time = time
        self.device = FakeDevice()
        self.device.clock = time.monotonic

        class Bridge:
            def post(_self, body):
                time.sleep(0.02)
                return self.device.post(body)

        self.worker = Worker(Bridge())
        self.worker.start()
        self.model = app.L.OverlayModel("macos", epoch0=100)
        self.app = app.OverlayApp(app.Controller(self.worker, self.model), "right")
        self.app.panel.setFrameOrigin_((-20000.0, -20000.0))
        self.pointer = [5.0, 5.0]
        self.app.read_pointer = lambda: tuple(self.pointer)

        class Typer:
            def __init__(_t):
                _t.pressed, _t.ok = [], True

            def allowed(_t):
                return _t.ok

            def press(_t, request):
                _t.pressed.append(request)
                return True

            def open_settings(_t):
                return True

        self.typer = Typer()
        self.app.controller.keyboard_kind, self.app.controller.typer = "osk", self.typer

    def tearDown(self):
        self.worker.stop()

    def run_for(self, seconds, where):
        end = self.time.monotonic() + seconds
        while self.time.monotonic() < end:
            self.pointer[:] = where() if callable(where) else where
            self.app.tick()
            self.time.sleep(1 / 30)

    def tile_centre(self):
        t = self.app.tile
        return t.x + t.w / 2, t.y + t.h / 2

    def item_centre(self, ident):
        lay, f = self.app.layout, self.app.layout.frame
        r = lay.items[ident]
        return f.x + r.x + r.w / 2, f.top - (r.y + r.h / 2)

    def open_menu(self):
        self.run_for(0.8, (5.0, 5.0))  # session visible, unclaimed
        self.assertEqual(self.model.state, "unclaimed")
        self.run_for(0.8, self.tile_centre())
        self.assertEqual(self.model.state, "menu")
        self.run_for(0.2, self.tile_centre())  # let the device acknowledge the menu

    def test_open_the_tile_move_onto_an_item_and_complete_its_dwell(self):
        self.open_menu()
        self.assertTrue(self.model.status["actions"]["ready"])
        frame_before = self.app.panel.frame()
        right = self.item_centre("right")
        self.run_for(0.2, right)
        mid = self.model.view(self.time.monotonic())
        self.assertEqual(mid.hover, "right")
        self.assertGreater(mid.progress["right"], 0.0, "the ring must show progress while dwelling")
        frame_mid = self.app.panel.frame()
        self.assertEqual(
            (frame_before.origin.x, frame_before.origin.y, frame_before.size.height),
            (frame_mid.origin.x, frame_mid.origin.y, frame_mid.size.height),
            "the window must not resize (and restart the dwell) while an item is dwelled on",
        )
        self.run_for(0.8, right)
        self.assertEqual(self.device.selected, ["right"])
        self.assertEqual(self.model.state, "tile")

    def key_centre(self, ident):
        lay = self.app.osk_layout
        k = lay.key(ident).rect
        return lay.frame.x + k.x + k.w / 2, lay.frame.top - (k.y + k.h / 2)

    def test_the_keyboard_item_opens_the_nodx_keyboard_and_a_key_dwell_types(self):
        self.open_menu()
        kb = self.item_centre("keyboard")
        self.run_for(1.0, kb)
        self.assertTrue(self.model.osk.open)
        self.assertTrue(self.app.osk_shown, "the keyboard window must be on screen")
        want, cur = self.app.osk_layout.frame, self.app.osk_panel.frame()
        self.assertEqual(
            (cur.origin.x, cur.origin.y, cur.size.width, cur.size.height),
            (want.x, want.y, want.w, want.h),
        )
        self.assertEqual(self.device.selected, [], "the keyboard item is not a device selection")
        self.run_for(0.5, (5.0, 5.0))  # menu closes; the keyboard stays
        self.run_for(1.0, self.key_centre("h"))
        self.assertEqual(self.typer.pressed, [("text", "h")])
        self.assertTrue(
            self.device.reporting, "the device must be told the pointer is on the overlay keyboard"
        )
        pressed = len(self.typer.pressed)
        self.run_for(0.5, (5.0, 5.0))
        self.assertEqual(len(self.typer.pressed), pressed, "nothing types off the keys")

    def test_close_hides_the_keyboard_window(self):
        self.open_menu()
        self.run_for(1.0, self.item_centre("keyboard"))
        self.run_for(0.5, (5.0, 5.0))
        self.run_for(1.2, self.key_centre("close"))
        self.assertFalse(self.model.osk.open)
        self.assertFalse(self.app.osk_shown)
        self.assertEqual(self.typer.pressed, [])

    def test_every_item_is_reachable_at_its_own_centre_and_corners(self):
        self.open_menu()
        for ident, _label in app.L.MENU_ITEMS:
            lay, f = self.app.layout, self.app.layout.frame
            r = lay.items[ident]
            for fx, fy in ((0.5, 0.5), (0.05, 0.1), (0.95, 0.9)):
                gx, gy = f.x + r.x + r.w * fx, f.top - (r.y + r.h * fy)
                self.assertEqual(app.L.hit_test(lay, gx, gy), ident, (ident, fx, fy))

    def test_the_window_is_where_the_layout_says_so_the_hit_test_can_trust_it(self):
        self.open_menu()
        cur, lay = self.app.panel.frame(), self.app.layout.frame
        self.assertEqual(
            (cur.origin.x, cur.origin.y, cur.size.width, cur.size.height),
            (lay.x, lay.y, lay.w, lay.h),
        )

    def test_an_item_does_not_dwell_until_the_device_acknowledges_the_menu(self):
        self.device.ready_flag = False
        self.open_menu()
        right = self.item_centre("right")
        self.run_for(0.8, right)
        self.assertEqual(self.device.selected, [])
        d = self.model.diagnostics(self.time.monotonic(), tuple(self.pointer), self.app.layout)
        self.assertIn("device not ready", d["reason"])
        self.device.ready_flag = True
        self.run_for(0.2, self.tile_centre())
        self.run_for(0.9, right)
        self.assertEqual(self.device.selected, ["right"])

    def test_the_diagnostics_name_the_hovered_item_progress_and_reason(self):
        self.open_menu()
        self.run_for(0.15, self.item_centre("cancel"))
        d = self.model.diagnostics(self.time.monotonic(), tuple(self.pointer), self.app.layout)
        self.assertEqual(d["hit"], "cancel")
        self.assertEqual(d["reason"], "dwelling")
        self.assertGreater(d["progress"], 0.0)

    def test_jitter_inside_the_tolerance_does_not_restart_the_dwell(self):
        self.open_menu()
        x, y = self.item_centre("left")
        step = [0]

        def shaky():
            step[0] += 1
            return x + (4 if step[0] % 2 else -4), y + (3 if step[0] % 3 else -3)

        self.run_for(1.0, shaky)
        self.assertEqual(self.device.selected, ["left"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
