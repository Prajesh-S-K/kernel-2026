"""The NodX on-screen keyboard: layout, dwell typing, the device zone, the typer and the controller. Pure Python
(no real key event is ever posted: the system library is replaced by a fake)."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from overlay import logic as L  # noqa: E402
from overlay import osk as K  # noqa: E402
from overlay import typing as T  # noqa: E402
from overlay.controller import Controller  # noqa: E402

MAIN = L.Rect(0, 0, 1440, 870)


def actions(**kw):
    base = {
        "enabled": True, "controller": "OVERLAY", "mode": "LEFT", "keep": False, "dragging": False,
        "scroll": "OFF", "keyboard": False, "menu": True, "ready": True, "dwellMs": 1000, "session": 3,
        "epochFloor": 0,
    }  # fmt: skip
    base.update(kw)
    return base


def status(**kw):
    return {"state": "ACTIVE", "handsFree": {"uncalDemo": {"active": True}}, "actions": actions(**kw)}


def centre(layout, ident):
    k = layout.key(ident).rect
    return layout.frame.x + k.x + k.w / 2, layout.frame.top - (k.y + k.h / 2)


class Layout(unittest.TestCase):
    def test_every_key_is_inside_the_window_and_none_overlap(self):
        for visible in (MAIN, L.Rect(0, 0, 1024, 600), L.Rect(0, 0, 700, 500)):
            lay = K.osk_layout(visible)
            self.assertTrue(
                visible.x <= lay.frame.x and lay.frame.right <= visible.right, (visible, lay.frame)
            )
            self.assertGreaterEqual(lay.frame.y, visible.y)
            rects = [k.rect for k in lay.keys]
            for k in lay.keys:
                r = k.rect
                self.assertTrue(0 <= r.x and r.right <= lay.frame.w + 0.01, k.ident)
                self.assertTrue(0 <= r.y and r.top <= lay.frame.h + 0.01, k.ident)
            for i, a in enumerate(rects):
                for b in rects[i + 1 :]:
                    apart = a.right <= b.x or b.right <= a.x or a.top <= b.y or b.top <= a.y
                    self.assertTrue(apart)

    def test_every_key_is_hit_at_its_centre_and_a_large_target(self):
        lay = K.osk_layout(MAIN)
        for key in lay.keys:
            self.assertGreaterEqual(min(key.rect.w, key.rect.h), 25, key.ident)
            hit = K.osk_hit(lay, *centre(lay, key.ident))
            self.assertIsNotNone(hit)
            self.assertEqual(hit.ident, key.ident)
        self.assertIsNone(K.osk_hit(lay, lay.frame.x - 5, lay.frame.y))


class Typing(unittest.TestCase):
    def setUp(self):
        self.lay = K.osk_layout(MAIN)
        self.osk = K.Osk()
        self.osk.show()
        self.t = 0.0

    def dwell(self, ident, seconds=1.0, device_dwell=1.0):
        """Rest on a key; returns every typing request made."""
        out = []
        pos = centre(self.lay, ident)
        end = self.t + seconds
        while self.t <= end:
            r = self.osk.update(self.lay, pos, self.t, device_dwell)
            if r:
                out.append(r)
            self.t += 0.033
        return out

    def leave(self):
        self.osk.update(self.lay, (-50.0, -50.0), self.t, 1.0)
        self.t += 0.05

    def test_a_dwell_types_the_key_once_and_must_be_left_before_it_types_again(self):
        self.assertEqual(self.dwell("h", 3.0), [("text", "h")])
        self.leave()
        self.assertEqual(self.dwell("h", 1.0), [("text", "h")])

    def test_a_short_rest_types_nothing(self):
        self.assertEqual(self.dwell("h", 0.3), [])

    def test_the_key_dwell_follows_the_device_dwell_with_a_floor(self):
        self.assertAlmostEqual(K.key_dwell_s(1.2), 0.72)
        self.assertEqual(K.key_dwell_s(0.2), K.KEY_DWELL_MIN_S)

    def test_shift_is_one_shot_and_shifts_digits(self):
        self.assertEqual(self.dwell("shift", 1.0), [])
        self.assertTrue(self.osk.shift)
        self.leave()
        self.assertEqual(self.dwell("a", 1.0), [("text", "A")])
        self.assertFalse(self.osk.shift)
        self.leave()
        self.dwell("shift", 1.0)
        self.leave()
        self.assertEqual(self.dwell("1", 1.0), [("text", "!")])

    def test_special_keys(self):
        self.assertEqual(self.dwell("backspace", 1.0), [("key", K.BACKSPACE_CODE)])
        self.leave()
        self.assertEqual(self.dwell("return", 1.0), [("key", K.RETURN_CODE)])
        self.leave()
        self.assertEqual(self.dwell("space", 1.0), [("text", " ")])

    def test_close_hides_the_keyboard_and_types_nothing(self):
        self.assertEqual(self.dwell("close", 1.0), [])
        self.assertFalse(self.osk.open)

    def test_nothing_types_while_typing_is_not_allowed_or_the_keyboard_is_closed(self):
        self.osk.typing_allowed = False
        self.assertEqual(self.dwell("h", 2.0), [])
        self.osk.typing_allowed = True
        self.osk.close()
        self.assertEqual(self.dwell("h", 2.0), [])


class ModelIntegration(unittest.TestCase):
    def make(self, **kw):
        m = L.OverlayModel("nodx", epoch0=100)
        m.on_reply(status(**kw), 1.0)
        m.state, m.claimed = "tile", True
        m.claim_sent_at = -100.0
        m.osk.show()
        self.lay = K.osk_layout(MAIN)
        self.tile = L.collapsed_layout(L.default_tile_frame(MAIN))
        return m

    def run_on(self, m, ident, seconds=1.5, start=1.0, **kw):
        out, t = [], start
        pos = centre(self.lay, ident)
        while t < start + seconds:
            m.on_reply(status(**kw), t)
            out += m.tick(t, pos, self.tile, self.lay)
            t += 0.033
        return out

    def test_a_key_dwell_yields_a_typing_command_under_the_current_epoch(self):
        m = self.make()
        cmds = self.run_on(m, "h")
        keys = [c for c in cmds if c.kind == "key"]
        self.assertEqual([c.value for c in keys], [("text", "h")])
        self.assertEqual(keys[0].epoch, m.epoch)

    def test_the_device_is_told_the_pointer_is_on_the_overlay_while_it_is_on_the_keyboard(self):
        m = self.make()
        cmds = self.run_on(m, "g", 0.2)
        menus = [c for c in cmds if c.kind == "menu"]
        self.assertTrue(menus and menus[0].value is True, "the device must inhibit target clicks here")

    def test_nothing_types_unless_the_overlay_holds_the_controls(self):
        for claimed, state in ((False, "unclaimed"), (True, "lost"), (False, "hidden")):
            m = self.make()
            m.claimed, m.state = claimed, state
            cmds = self.run_on(m, "h", 1.5)
            self.assertEqual([c for c in cmds if c.kind == "key"], [], (claimed, state))

    def test_nothing_types_while_dragging_or_scrolling(self):
        for extra in ({"dragging": True}, {"scroll": "ACTIVE"}):
            m = self.make(**extra)
            cmds = self.run_on(m, "h", 1.5, **extra)
            self.assertEqual([c for c in cmds if c.kind == "key"], [], extra)

    def test_loss_stop_and_a_new_session_close_the_keyboard(self):
        m = self.make()
        m._lose(2.0)
        self.assertFalse(m.osk.open)
        m = self.make()
        m._reset()
        self.assertFalse(m.osk.open)

    def test_a_stale_reply_after_loss_cannot_reopen_the_keyboard_or_type(self):
        m = self.make()
        m._lose(2.0)
        cmds = self.run_on(m, "h", 1.5, start=2.1)
        self.assertEqual([c for c in cmds if c.kind == "key"], [])
        self.assertFalse(m.osk.open)

    def test_the_menu_collapses_when_the_pointer_is_only_on_the_keyboard(self):
        m = self.make()
        m.state, m.menu_open = "menu", True
        pos = centre(self.lay, "h")
        t = 1.0
        while t < 3.0:
            m.on_reply(status(), t)
            m.tick(t, pos, self.tile, self.lay)
            t += 0.033
        self.assertEqual(m.state, "tile", "the keyboard must not keep the menu open")


class FakeLib:
    def __init__(self, trusted=True):
        self.trusted, self.events, self.posts = trusted, [], []

    def AXIsProcessTrusted(self):
        return self.trusted

    class _Fn:
        def __init__(self, fn):
            self.fn, self.argtypes, self.restype = fn, None, None

        def __call__(self, *a):
            return self.fn(*a)

    def __getattr__(self, name):
        table = {
            "CGEventCreateKeyboardEvent": lambda src, code, down: self.events.append(
                {"code": code, "down": down, "text": None}
            )
            or len(self.events),
            "CGEventKeyboardSetUnicodeString": lambda ev, n, arr: self.events[ev - 1].update(
                text="".join(chr(arr[i]) for i in range(n))
            ),
            "CGEventPost": lambda tap, ev: self.posts.append(self.events[ev - 1]),
            "CFRelease": lambda ev: None,
        }
        if name in table:
            return self._Fn(table[name])
        raise AttributeError(name)


class TyperTests(unittest.TestCase):
    def test_text_is_posted_as_key_down_and_up_with_the_character(self):
        lib = FakeLib()
        self.assertTrue(T.Typer(lib).press(("text", "A")))
        self.assertEqual([(e["down"], e["text"]) for e in lib.posts], [(True, "A"), (False, "A")])

    def test_a_virtual_key_posts_its_code(self):
        lib = FakeLib()
        self.assertTrue(T.Typer(lib).press(("key", 51)))
        self.assertEqual([(e["code"], e["down"]) for e in lib.posts], [(51, True), (51, False)])

    def test_nothing_is_posted_without_the_macos_permission(self):
        lib = FakeLib(trusted=False)
        typer = T.Typer(lib)
        self.assertFalse(typer.allowed())
        self.assertFalse(typer.press(("text", "a")))
        self.assertEqual(lib.posts, [])


class FakeWorker:
    def __init__(self):
        self.sent = []

    def drain(self):
        return []

    def send(self, kind, value=None, session=None, epoch=None):
        self.sent.append((kind, value))

    def ensure_traffic(self, *a):
        pass

    def set_epoch(self, *a):
        pass


class FakeTyper:
    def __init__(self, allowed=True):
        self.ok, self.pressed, self.opened = allowed, [], 0

    def allowed(self):
        return self.ok

    def press(self, request):
        self.pressed.append(request)
        return True

    def open_settings(self):
        self.opened += 1
        return True


class ControllerOsk(unittest.TestCase):
    def make(self, allowed=True):
        model = L.OverlayModel("nodx", epoch0=100)
        model.on_reply(status(), 1.0)
        model.claimed = True
        typer = FakeTyper(allowed)
        worker = FakeWorker()
        return Controller(worker, model, keyboard_kind="osk", typer=typer), worker, typer, model

    def test_the_keyboard_item_shows_and_hides_the_nodx_keyboard_without_touching_the_device(self):
        c, w, t, m = self.make()
        c._dispatch(L.Command("keyboard_toggle"), 2.0)
        self.assertTrue(m.osk.open)
        c._dispatch(L.Command("keyboard_toggle"), 3.0)
        self.assertFalse(m.osk.open)
        self.assertEqual([x for x in w.sent if x[0] == "keyboard"], [], "no macOS keyboard mode")

    def test_typing_requests_reach_the_typer(self):
        c, w, t, m = self.make()
        c._dispatch(L.Command("key", ("text", "x")), 2.0)
        self.assertEqual(t.pressed, [("text", "x")])

    def test_without_the_permission_it_says_so_opens_the_pane_once_and_never_types(self):
        c, w, t, m = self.make(allowed=False)
        c._dispatch(L.Command("keyboard_toggle"), 2.0)
        self.assertFalse(m.osk.typing_allowed)
        self.assertIn("Accessibility", m.notice)
        self.assertEqual(t.opened, 1)
        c._watch_typing(5.0)
        c._watch_typing(8.0)
        self.assertEqual(t.opened, 1, "the pane opens once, not every check")
        t.ok = True
        c._watch_typing(11.0)
        self.assertTrue(m.osk.typing_allowed, "typing starts once the permission is granted")


if __name__ == "__main__":
    unittest.main(verbosity=2)
