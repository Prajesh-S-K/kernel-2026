"""Overlay logic: geometry on synthetic display layouts, dwell rules and the state machine. Pure Python."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from overlay import logic as L  # noqa: E402

MAIN = L.Rect(0, 0, 1440, 870)  # a Retina laptop: 1440 x 900 points minus the menu bar
RIGHT_EXTRA = L.Rect(1440, -180, 2560, 1415)  # a larger display on the right, lower origin
LEFT_EXTRA = L.Rect(-1920, 40, 1920, 1055)  # a display on the left: NEGATIVE origin
SMALL = L.Rect(0, 0, 1024, 600)  # a small screen where a one-column menu cannot fit


def actions(**kw):
    base = {
        "enabled": True,
        "controller": "OVERLAY",
        "mode": "LEFT",
        "keep": False,
        "dragging": False,
        "scroll": "OFF",
        "keyboard": False,
        "menu": False,
        "ready": False,
        "dwellMs": 1200,
    }
    base.update(kw)
    return base


def status(active=True, **kw):
    return {
        "state": "ACTIVE" if active else "CALIBRATION_REQUIRED",
        "handsFree": {"uncalDemo": {"active": active}},
        "actions": actions(**kw),
    }


class Geometry(unittest.TestCase):
    def test_clamp_keeps_the_window_inside_whichever_screen_it_is_on(self):
        screens = [MAIN, RIGHT_EXTRA, LEFT_EXTRA]
        for frame in (
            L.Rect(-5000, 100, L.TILE_W, L.TILE_H),  # far off to the left
            L.Rect(9000, 100, L.TILE_W, L.TILE_H),  # far off to the right
            L.Rect(700, -900, L.TILE_W, L.TILE_H),  # below everything
            L.Rect(700, 5000, L.TILE_W, L.TILE_H),  # above everything
            L.Rect(1400, 100, L.TILE_W, L.TILE_H),  # straddling two displays
            L.Rect(-100, 40, L.TILE_W, L.TILE_H),  # straddling the negative origin
        ):
            out = L.clamp_to_visible(frame, screens)
            self.assertEqual((out.w, out.h), (frame.w, frame.h), "the size must not change")
            self.assertTrue(
                any(
                    v.x <= out.x and out.right <= v.right and v.y <= out.y and out.top <= v.top
                    for v in screens
                ),
                f"{frame} -> {out} is not inside any screen",
            )

    def test_a_window_stays_on_the_display_it_mostly_overlaps(self):
        screens = [MAIN, RIGHT_EXTRA]
        on_right = L.clamp_to_visible(L.Rect(1500, 300, L.TILE_W, L.TILE_H), screens)
        self.assertEqual(on_right, L.Rect(1500, 300, L.TILE_W, L.TILE_H))
        # 40 points on the main display, the rest on the right one: it belongs to the right display
        mostly_right = L.clamp_to_visible(L.Rect(1400, 300, L.TILE_W, L.TILE_H), screens)
        self.assertGreaterEqual(mostly_right.x, RIGHT_EXTRA.x)
        # most of it on the main display, 40 points on the right one: it belongs to the main display
        mostly_main = L.clamp_to_visible(
            L.Rect(MAIN.right - (L.TILE_W - 40), 300, L.TILE_W, L.TILE_H), screens
        )
        self.assertLessEqual(mostly_main.right, MAIN.right)

    def test_snap_goes_to_the_nearest_edge_of_its_own_screen(self):
        left = L.snap_to_edge(L.Rect(300, 400, L.TILE_W, L.TILE_H), [MAIN])
        right = L.snap_to_edge(L.Rect(1200, 400, L.TILE_W, L.TILE_H), [MAIN])
        self.assertEqual(left.x, MAIN.x + L.MARGIN)
        self.assertEqual(right.right, MAIN.right - L.MARGIN)
        second = L.snap_to_edge(L.Rect(1500, 400, L.TILE_W, L.TILE_H), [MAIN, RIGHT_EXTRA])
        self.assertEqual(second.x, RIGHT_EXTRA.x + L.MARGIN)
        neg = L.snap_to_edge(L.Rect(-300, 400, L.TILE_W, L.TILE_H), [MAIN, LEFT_EXTRA])
        self.assertEqual(neg.right, LEFT_EXTRA.right - L.MARGIN)

    def test_default_position_is_near_an_edge_and_inside_the_usable_area(self):
        for visible in (MAIN, RIGHT_EXTRA, LEFT_EXTRA, SMALL):
            for edge in ("left", "right"):
                for offset in (0.0, 0.45, 1.0):
                    f = L.default_tile_frame(visible, edge, offset)
                    self.assertTrue(visible.x <= f.x and f.right <= visible.right)
                    self.assertTrue(visible.y <= f.y and f.top <= visible.top)
                    self.assertEqual((f.w, f.h), (L.TILE_W, L.TILE_H))

    def test_expanded_layout_fits_on_screen_in_every_position_and_screen(self):
        for visibles in ([MAIN], [MAIN, RIGHT_EXTRA, LEFT_EXTRA], [SMALL]):
            for visible in visibles:
                for edge in ("left", "right"):
                    for offset in (0.0, 0.5, 1.0):
                        tile = L.default_tile_frame(visible, edge, offset)
                        lay = L.expanded_layout(tile, visibles)
                        f = lay.frame
                        self.assertTrue(
                            visible.x <= f.x and f.right <= visible.right + 1e-6, (visible, f)
                        )
                        self.assertTrue(
                            visible.y <= f.y and f.top <= visible.top + 1e-6, (visible, f)
                        )
                        # the tile did not move on screen
                        gx, gy = f.x + lay.tile.x, f.top - lay.tile.y - L.TILE_H
                        self.assertAlmostEqual(gx, tile.x, places=3)
                        self.assertAlmostEqual(gy, tile.y, places=3)
                        for ident, _ in L.MENU_ITEMS:
                            r = lay.items[ident]
                            self.assertTrue(0 <= r.x and r.right <= f.w + 1e-6, ident)
                            self.assertTrue(0 <= r.y and r.top <= f.h + 1e-6, ident)
                            self.assertGreaterEqual(r.h, 44, "targets must stay large")

    def test_items_never_overlap_the_tile_or_each_other(self):
        tile = L.default_tile_frame(MAIN, "right", 0.2)
        lay = L.expanded_layout(tile, [MAIN])
        rects = [lay.tile, *lay.items.values()]
        for i, a in enumerate(rects):
            for b in rects[i + 1 :]:
                overlap_x = min(a.right, b.right) - max(a.x, b.x)
                overlap_y = min(a.top, b.top) - max(a.y, b.y)
                self.assertFalse(overlap_x > 0.5 and overlap_y > 0.5, (a, b))

    def test_small_screens_use_more_columns(self):
        for offset in (0.0, 0.5, 1.0):
            tile = L.default_tile_frame(SMALL, "right", offset)
            lay = L.expanded_layout(tile, [SMALL])
            fits = lay.frame.y >= SMALL.y and lay.frame.top <= SMALL.top
            self.assertTrue(fits, f"offset {offset}: {lay.frame}")
        mid = L.expanded_layout(L.default_tile_frame(SMALL, "right", 0.5), [SMALL])
        self.assertGreater(mid.frame.w, L.ITEM_W + 1, "expected two columns with room neither way")

    def test_hit_test_and_whole_window_zone(self):
        tile = L.default_tile_frame(MAIN, "right", 0.3)
        lay = L.expanded_layout(tile, [MAIN])
        # the middle of every item hits that item; the tile hits the tile
        for ident, _ in L.MENU_ITEMS:
            r = lay.items[ident]
            gx = lay.frame.x + r.x + r.w / 2
            gy = lay.frame.top - (r.y + r.h / 2)
            self.assertEqual(L.hit_test(lay, gx, gy), ident)
        t = lay.tile
        self.assertEqual(
            L.hit_test(lay, lay.frame.x + t.x + 3, lay.frame.top - (t.y + t.h / 2)), "tile"
        )
        self.assertIsNone(L.hit_test(lay, lay.frame.x - 20, lay.frame.y + 5))
        # a gap between items is on the overlay (a click there is absorbed) but is not a control
        gap_x = lay.frame.x + lay.items["left"].x + 5
        gap_y = lay.frame.top - (lay.items["left"].top - 2)
        self.assertTrue(L.on_overlay(lay, gap_x, gap_y))
        self.assertFalse(L.on_overlay(lay, lay.frame.x - 20, lay.frame.y + 5))

    def test_the_menu_lists_exactly_the_requested_items(self):
        self.assertEqual(
            [i for i, _ in L.MENU_ITEMS],
            ["left", "right", "double", "drag", "drop", "scroll", "keyboard", "cancel", "stop"],
        )
        self.assertEqual([n for _, n in L.MENU_ITEMS][-1], "Pause / Stop")  # Pause/Stop is one item


class DwellRules(unittest.TestCase):
    def test_completes_after_the_dwell_and_fires_once(self):
        d = L.Dwell()
        fired = [d.update("a", (0, 0), t / 10, 1.2) for t in range(0, 40)]
        self.assertEqual(fired.count(True), 1)
        self.assertEqual(fired.index(True), 12)

    def test_movement_restarts_leaving_resets_and_a_chosen_control_must_be_left(self):
        d = L.Dwell(tolerance=14)
        for t in range(10):
            d.update("a", (0, 0), t / 10, 1.2)
        self.assertGreater(d.progress, 0.5)
        d.update("a", (40, 0), 1.0, 1.2)  # moved beyond the tolerance
        self.assertEqual(d.progress, 0)
        d.update(None, (0, 0), 1.1, 1.2)  # left
        self.assertIsNone(d.target)
        done = False
        for t in range(0, 30):
            done = d.update("a", (0, 0), 2 + t / 10, 1.2) or done
        self.assertTrue(done)
        # staying on the chosen control never fires again
        again = [d.update("a", (0, 0), 6 + t / 10, 1.2) for t in range(50)]
        self.assertFalse(any(again))
        d.update(None, (0, 0), 12, 1.2)  # leave
        fired = [d.update("a", (0, 0), 13 + t / 10, 1.2) for t in range(20)]
        self.assertTrue(any(fired), "it must be selectable again after leaving it")

    def test_small_jitter_does_not_restart(self):
        d = L.Dwell(tolerance=14)
        fired = False
        for t in range(20):
            fired = d.update("a", (t % 3, t % 2), t / 10, 1.2) or fired
        self.assertTrue(fired)


class Harness:
    """Drives the model with a fake clock, pointer and device status; layout follows the model like the app."""

    def __init__(self, keyboard_clicks="macos", screens=None):
        self.screens = screens or [MAIN]
        self.tile = L.default_tile_frame(self.screens[0], "right", 0.3)
        self.model = L.OverlayModel(keyboard_clicks)
        self.now = 100.0
        self.pointer = (10.0, 10.0)
        self.commands = []
        self.model.on_reply(status(), self.now)

    def layout(self):
        v = self.model.view(self.now)
        if v.banner:
            return L.expanded_layout(self.tile, self.screens, banner=True)
        if self.model.state == "menu":
            return L.expanded_layout(self.tile, self.screens)
        return L.collapsed_layout(self.tile)

    def reply(self, **kw):
        self.model.on_reply(
            status(**kw) if "active" not in kw else status(kw.pop("active")), self.now
        )

    def step(self, seconds=0.0333, reply=True, **kw):
        self.now += seconds
        if reply:
            st = kw.pop("status_obj", None)
            self.model.on_reply(st if st else self.model.status, self.now)
        cmds = self.model.tick(self.now, self.pointer, self.layout())
        self.commands += cmds
        return cmds

    def run(self, seconds, **kw):
        out = []
        for _ in range(int(seconds / 0.0333)):
            out += self.step(**kw)
        return out

    def on_tile(self):
        lay = self.layout()
        self.pointer = (lay.frame.x + lay.tile.x + 20, lay.frame.top - lay.tile.y - 20)

    def on_item(self, ident):
        lay = self.layout()
        r = lay.items[ident]
        self.pointer = (lay.frame.x + r.x + r.w / 2, lay.frame.top - (r.y + r.h / 2))

    def away(self):
        self.pointer = (self.tile.x - 400, self.tile.y)

    def set_status(self, **kw):
        self.model.on_reply(status(**kw), self.now)

    def kinds(self):
        return [(c.kind, c.value) for c in self.commands]


class Model(unittest.TestCase):
    def test_nothing_shows_without_an_explicitly_started_session(self):
        h = Harness()
        h.model.on_reply(status(active=False), h.now)
        h.step()
        self.assertFalse(h.model.view(h.now).visible)
        h.set_status()
        h.step()
        v = h.model.view(h.now)
        self.assertTrue(v.visible and v.state == "unclaimed")

    def test_a_cursor_passing_over_the_tile_does_not_expand_it(self):
        h = Harness()
        h.step()
        h.on_tile()
        h.run(0.4)  # well under the dwell
        h.away()
        h.run(0.3)
        self.assertEqual(h.model.state, "unclaimed")
        self.assertEqual(h.commands, [])

    def test_a_dwell_on_the_tile_claims_the_controls_and_opens_the_menu(self):
        h = Harness()
        h.step()
        h.on_tile()
        h.run(1.5)
        self.assertEqual(h.model.state, "menu")
        self.assertIn(("overlay", True), h.kinds())
        self.assertIn(
            ("menu", True), h.kinds(), "the pointer-on-overlay state must be reported at once"
        )

    def test_menu_selection_waits_for_the_devices_acknowledgement(self):
        h = Harness()
        h.step()
        h.on_tile()
        h.run(1.5)
        h.set_status(menu=False, ready=False)  # the device has not acknowledged yet
        h.on_item("right")
        before = len([c for c in h.commands if c.kind == "select"])
        h.run(2.5, status_obj=status(menu=False, ready=False))
        self.assertEqual(
            len([c for c in h.commands if c.kind == "select"]), before, "selected unacknowledged"
        )
        h.set_status(menu=True, ready=True)
        h.run(1.6)
        self.assertIn(("select", "right"), h.kinds())

    def _open_menu(self, h):
        h.step()
        h.on_tile()
        h.run(1.5)
        h.set_status(menu=True, ready=True)
        h.step()

    def test_selection_collapses_and_the_tile_must_be_left_before_it_expands_again(self):
        h = Harness()
        self._open_menu(h)
        h.on_item("double")
        h.run(1.6)
        selects = [c for c in h.commands if c.kind == "select"]
        self.assertEqual([c.value for c in selects], ["double"])
        self.assertEqual(h.model.state, "tile", "the menu did not collapse after the selection")
        # resting where the selection was made chooses nothing more
        count = len(h.commands)
        h.run(3.0)
        self.assertEqual([c.kind for c in h.commands[count:]], ["menu"] * len(h.commands[count:]))
        self.assertEqual(h.model.state, "tile")
        # moving onto the tile and dwelling there expands it again (a fresh hover)
        h.on_tile()
        h.run(1.5)
        self.assertEqual(h.model.state, "menu")

    def test_every_item_selects_its_own_target_and_keyboard_is_a_toggle(self):
        for ident, _ in L.MENU_ITEMS:
            h = Harness()
            self._open_menu(h)
            h.on_item(ident)
            h.run(1.6)
            kinds = [c for c in h.commands if c.kind in ("select", "keyboard_toggle")]
            expected = ("keyboard_toggle", None) if ident == "keyboard" else ("select", ident)
            self.assertEqual([(c.kind, c.value) for c in kinds], [expected], ident)

    def test_leaving_the_menu_collapses_it_without_selecting(self):
        h = Harness()
        self._open_menu(h)
        h.on_item("scroll")
        h.run(0.5)
        h.away()
        h.run(1.5)
        self.assertEqual(h.model.state, "tile")
        self.assertFalse([c for c in h.commands if c.kind == "select"])

    def test_the_overlay_repeats_its_state_and_reports_changes_at_once(self):
        h = Harness()
        self._open_menu(h)
        h.away()
        h.step()
        zone_changes = [c.value for c in h.commands if c.kind == "menu"]
        self.assertEqual(zone_changes[-1], False, "leaving the overlay was not reported")
        before = len([c for c in h.commands if c.kind == "menu"])
        h.run(2.0)
        beats = len([c for c in h.commands if c.kind == "menu"]) - before
        self.assertGreaterEqual(beats, 3, "no heartbeat")
        self.assertLessEqual(beats, 6)

    def test_an_item_cannot_be_selected_without_dwelling_and_the_dwell_restarts_on_movement(self):
        h = Harness()
        self._open_menu(h)
        h.on_item("left")
        for i in range(60):  # 2 s of pointer wandering by more than the tolerance every half second
            if i % 15 == 0:
                x, y = h.pointer
                h.pointer = (x + 30, y)
            h.step()
        self.assertFalse([c for c in h.commands if c.kind == "select"])

    def test_current_action_is_shown_when_collapsed(self):
        h = Harness()
        h.set_status(mode="RIGHT", controller="OVERLAY")
        h.model.claimed, h.model.state = True, "tile"
        h.model.claim_sent_at = -1e9
        v = h.model.view(h.now)
        self.assertIn("Right-click", v.sub)
        self.assertIn("one shot", v.sub)
        h.set_status(mode="DOUBLE", keep=True)
        self.assertIn("kept", h.model.view(h.now).sub)
        h.set_status(mode="LEFT")
        self.assertEqual(h.model.view(h.now).sub, "Left-click")

    def test_dragging_scrolling_and_keyboard_are_prominent(self):
        h = Harness()
        h.model.claimed, h.model.state = True, "tile"
        h.set_status(mode="DRAG", dragging=True)
        v = h.model.view(h.now)
        self.assertEqual((v.alert, v.colour), ("DRAGGING", "danger"))
        h.set_status(mode="SCROLL", scroll="ACTIVE")
        v = h.model.view(h.now)
        self.assertEqual((v.alert, v.banner), ("SCROLLING", True))
        h.set_status(keyboard=True)
        self.assertEqual(h.model.view(h.now).alert, "KEYBOARD")

    def test_losing_the_bridge_inhibits_and_resuming_needs_an_explicit_dwell(self):
        h = Harness()
        self._open_menu(h)
        h.away()
        h.run(1.5)
        # the bridge stops answering: no replies; heartbeats continue only until the loss limit
        for _ in range(int(4.0 / 0.0333)):
            h.step(reply=False)
            if h.model.state == "lost":
                break
        self.assertEqual(h.model.state, "lost")
        self.assertFalse(h.model.claimed)
        sent_before = len(h.commands)
        h.run(3.0, reply=False)
        self.assertEqual(len(h.commands), sent_before, "commands were sent while lost")
        # the bridge returns: still lost, nothing resumes by itself
        h.set_status()
        h.run(3.0)
        self.assertEqual(h.model.state, "lost")
        self.assertEqual(len(h.commands), sent_before)
        v = h.model.view(h.now)
        self.assertTrue(v.alert.startswith("LOST"))
        self.assertIn("dwell to resume", v.alert)
        # an explicit dwell on the tile claims it again
        h.on_tile()
        h.run(1.5)
        self.assertEqual(h.model.state, "menu")
        self.assertIn(("overlay", True), h.kinds()[sent_before:])

    def test_a_dropped_claim_is_never_assumed_back(self):
        h = Harness()
        self._open_menu(h)
        h.away()
        h.run(0.5)
        h.set_status(
            controller="NONE", menu=False, ready=False
        )  # the device switched the overlay off
        h.run(2.0)
        self.assertEqual(h.model.state, "lost")
        h.set_status(controller="BROWSER")
        h.on_tile()
        h.run(0.2)
        self.assertEqual(h.model.state, "lost")

    def test_a_refused_claim_explains_itself(self):
        h = Harness()
        h.step()
        h.on_tile()
        h.run(1.5)
        h.set_status(controller="BROWSER")
        h.away()
        h.run(2.5)
        self.assertEqual(h.model.state, "lost")
        self.assertIn("browser palette", h.model.view(h.now).notice)

    def test_ending_the_session_hides_everything_and_forgets_the_claim(self):
        h = Harness()
        self._open_menu(h)
        h.model.on_reply(status(active=False), h.now)
        h.step()
        self.assertFalse(h.model.view(h.now).visible)
        self.assertFalse(h.model.claimed)
        # a new explicit session shows the tile again but does not claim anything by itself
        before = len(h.commands)
        h.set_status(controller="NONE")
        h.away()
        h.run(3.0)
        self.assertEqual(h.model.state, "unclaimed")
        self.assertEqual(h.commands[before:], [], "the overlay claimed the controls by itself")

    def test_the_menu_does_not_react_to_the_pointer_while_scrolling_banner_is_shown(self):
        h = Harness()
        h.model.claimed, h.model.state = True, "tile"
        h.set_status(mode="SCROLL", scroll="ACTIVE")
        lay = L.expanded_layout(h.tile, h.screens, banner=True)
        self.assertEqual(lay.items, {})
        self.assertIsNotNone(lay.banner)

    def test_multi_display_tile_positions_all_work(self):
        for screens in ([MAIN, RIGHT_EXTRA], [LEFT_EXTRA, MAIN], [MAIN, RIGHT_EXTRA, LEFT_EXTRA]):
            for visible in screens:
                h = Harness(screens=screens)
                h.tile = L.default_tile_frame(visible, "right", 0.5)
                h.step()
                h.on_tile()
                h.run(1.5)
                self.assertEqual(h.model.state, "menu", visible)
                h.set_status(menu=True, ready=True)
                h.on_item("cancel")
                h.run(1.6)
                self.assertIn(("select", "cancel"), h.kinds(), visible)


if __name__ == "__main__":
    unittest.main(verbosity=2)
