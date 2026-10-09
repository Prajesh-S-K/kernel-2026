"""Pure overlay logic: geometry, dwell timing and the overlay state machine. No AppKit, no I/O.

Coordinates use the AppKit convention for screens and windows (origin bottom-left, y up, points). A window's
LOCAL layout uses a top-left origin (y down) because that is how it is drawn. Everything is in points, so
display scaling (Retina) does not change any of the numbers here.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

MENU_ITEMS = (
    ("left", "Left-click"),
    ("right", "Right-click"),
    ("double", "Double-click"),
    ("drag", "Drag"),
    ("drop", "Drop"),
    ("scroll", "Scroll"),
    ("keyboard", "Keyboard"),
    ("cancel", "Cancel"),
    ("stop", "Pause / Stop"),
)
SELECT_TARGETS = {
    "left",
    "right",
    "double",
    "drag",
    "drop",
    "scroll",
    "cancel",
    "stop",
}  # sent as `select`

TILE_W, TILE_H = 200.0, 52.0  # a large, easy dwell target
ITEM_W, ITEM_H = 200.0, 52.0
MARGIN = 8.0
BANNER_W, BANNER_H = (
    260.0,
    118.0,
)  # the static banner shown while scrolling (pointer frozen: nothing to dwell on)
DWELL_TOLERANCE = 14.0  # points of pointer movement that restart a menu dwell
LEAVE_GRACE_S = 0.8  # the menu collapses this long after the pointer left it
HEARTBEAT_S = 0.5  # the overlay repeats its state; the device treats > 2.5 s of silence as lost
LOST_AFTER_S = 2.5  # no successful bridge reply for this long: the overlay is "lost"
CLAIM_GRACE_S = 1.5  # a claim command is in flight: the status may not show it yet
DEFAULT_DWELL_S = 1.2
KEYBOARD_HINT = (
    "Keyboard mode: NodX clicks are paused. NodX only knows the keyboard's host process is running, not that a "
    "keyboard is on screen. No keyboard? Dwell on this tile and choose Keyboard again, Cancel or any action to "
    "get NodX clicks back. Pause / Stop and the physical button also work."
)


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float

    @property
    def right(self) -> float:
        return self.x + self.w

    @property
    def top(self) -> float:
        return self.y + self.h

    def contains(self, px: float, py: float) -> bool:
        return self.x <= px <= self.right and self.y <= py <= self.top


def clamp_to_visible(frame: Rect, visibles: list[Rect]) -> Rect:
    """Move (never resize) `frame` fully inside the visible area of the screen it overlaps most, or of the
    first screen when it overlaps none. Works for any number of screens, negative origins and mixed sizes."""
    if not visibles:
        return frame
    best, best_area = visibles[0], -1.0
    for v in visibles:
        ox = max(0.0, min(frame.right, v.right) - max(frame.x, v.x))
        oy = max(0.0, min(frame.top, v.top) - max(frame.y, v.y))
        if ox * oy > best_area:
            best, best_area = v, ox * oy
    x = min(max(frame.x, best.x), max(best.x, best.right - frame.w))
    y = min(max(frame.y, best.y), max(best.y, best.top - frame.h))
    return Rect(x, y, frame.w, frame.h)


def snap_to_edge(frame: Rect, visibles: list[Rect]) -> Rect:
    """After the user moved the tile: clamp it, then snap it to the nearest vertical screen edge so it never
    rests in the middle of someone's work."""
    f = clamp_to_visible(frame, visibles)
    best = visibles[0] if visibles else f
    for v in visibles:
        if v.contains(f.x + f.w / 2, f.y + f.h / 2):
            best = v
            break
    left_gap = f.x - best.x
    right_gap = best.right - f.right
    x = best.x + MARGIN if left_gap <= right_gap else best.right - f.w - MARGIN
    return clamp_to_visible(Rect(x, f.y, f.w, f.h), visibles)


def default_tile_frame(visible: Rect, edge: str = "right", offset: float = 0.2) -> Rect:
    """The first position: near an edge (`left` or `right`), `offset` of the way down from the top."""
    y = visible.top - TILE_H - MARGIN - offset * max(0.0, visible.h - TILE_H - 2 * MARGIN)
    x = visible.x + MARGIN if edge == "left" else visible.right - TILE_W - MARGIN
    return clamp_to_visible(Rect(x, y, TILE_W, TILE_H), [visible])


@dataclass(frozen=True)
class Layout:
    frame: Rect  # the window frame (screen coordinates)
    tile: Rect  # LOCAL (top-left origin)
    items: dict[str, Rect]  # LOCAL
    banner: Rect | None


def collapsed_layout(tile_frame: Rect) -> Layout:
    return Layout(tile_frame, Rect(0, 0, tile_frame.w, tile_frame.h), {}, None)


def expanded_layout(
    tile_frame: Rect, visibles: list[Rect], items=MENU_ITEMS, banner: bool = False
) -> Layout:
    """The tile with its menu (or the scroll banner) attached below it, or above it when there is more room
    above. Uses as many columns as the room needs. The tile does not move on screen unless the screen is too
    small for any arrangement, in which case the window is moved to fit (it always ends up fully visible)."""
    visible = next((v for v in visibles if v.contains(tile_frame.x + 1, tile_frame.y + 1)), None)
    visible = visible or (visibles[0] if visibles else tile_frame)
    n = len(items)
    space_below = tile_frame.y - visible.y
    space_above = visible.top - tile_frame.top
    below = space_below >= space_above
    space = max(space_below, space_above)
    if banner:
        cols, rows, body_w, body_h = 1, 0, BANNER_W, BANNER_H
    else:
        rows_fit = max(1, int((space - MARGIN) // ITEM_H))
        cols = min(
            max(1, math.ceil(n / rows_fit)), max(1, int((visible.w + MARGIN) // (ITEM_W + MARGIN)))
        )
        rows = math.ceil(n / cols)
        body_w, body_h = cols * ITEM_W + (cols - 1) * MARGIN, rows * ITEM_H
    width = max(tile_frame.w, body_w)
    height = TILE_H + MARGIN + body_h
    x = min(max(tile_frame.x, visible.x), max(visible.x, visible.right - width))
    if below:
        frame = Rect(x, tile_frame.top - height, width, height)
        top = TILE_H + MARGIN
    else:
        frame = Rect(x, tile_frame.y, width, height)
        top = 0.0
    fitted = clamp_to_visible(
        frame, visibles or [visible]
    )  # last resort on a tiny screen: move, never clip
    frame = Rect(fitted.x, fitted.y, frame.w, frame.h)
    tile_y = 0.0 if below else height - TILE_H  # the tile stays at its end of the window
    tile = Rect(tile_frame.x - frame.x, tile_y, tile_frame.w, TILE_H)
    if banner:
        return Layout(frame, tile, {}, Rect(0, top, width, BANNER_H))
    rects = {}
    for i, (ident, _label) in enumerate(items):
        col, row = divmod(i, rows)
        rects[ident] = Rect(col * (ITEM_W + MARGIN), top + row * ITEM_H, ITEM_W, ITEM_H - 4)
    return Layout(frame, tile, rects, None)


def self_items(layout: Layout) -> set[str]:
    return set(layout.items)


def to_local(layout: Layout, gx: float, gy: float) -> tuple[float, float]:
    return gx - layout.frame.x, layout.frame.top - gy


def hit_test(layout: Layout, gx: float, gy: float) -> str | None:
    """'tile', a menu item id, 'banner', or None when the global point is not on the overlay at all."""
    lx, ly = to_local(layout, gx, gy)
    if layout.tile.contains(lx, ly):
        return "tile"
    for ident, rect in layout.items.items():
        if rect.contains(lx, ly):
            return ident
    if layout.banner and layout.banner.contains(lx, ly):
        return "banner"
    return None


def on_overlay(layout: Layout, gx: float, gy: float) -> bool:
    """True for ANY point of the overlay window (gaps between items included): a click there is absorbed by the
    overlay, so target actions are inhibited for the whole window, not only for the controls."""
    return layout.frame.contains(gx, gy)


class Dwell:
    """Times one dwell on one control. Leaving the control resets it; pointer movement beyond `tolerance`
    restarts it; completing it fires once and then the control stays locked until the pointer leaves it
    (one selection per hover)."""

    def __init__(self, tolerance: float = DWELL_TOLERANCE):
        self.tolerance = tolerance
        self.target: str | None = None
        self.anchor = (0.0, 0.0)
        self.since = 0.0
        self.locked: str | None = None
        self.progress = 0.0

    def reset(self) -> None:
        self.target = None
        self.progress = 0.0

    def update(
        self, target: str | None, pos: tuple[float, float], now: float, dwell_s: float
    ) -> bool:
        if target != self.locked:
            self.locked = None  # the control that was just chosen has been left
        if target is None:
            self.reset()
            return False
        if target == self.locked:
            self.target, self.progress = target, 0.0  # chosen a moment ago: leave it first
            return False
        if target != self.target:
            self.target, self.anchor, self.since, self.progress = target, pos, now, 0.0
            return False
        if math.hypot(pos[0] - self.anchor[0], pos[1] - self.anchor[1]) > self.tolerance:
            self.anchor, self.since, self.progress = pos, now, 0.0
            return False
        self.progress = min(1.0, (now - self.since) / max(0.05, dwell_s))
        if self.progress >= 1.0:
            self.locked = target
            self.progress = 0.0
            return True
        return False


@dataclass
class Command:
    """What the overlay wants sent through the bridge. `kind` is overlay | menu | select | keyboard_toggle |
    open_keyboard | open_settings."""

    kind: str
    value: object = None
    session: int | None = None  # the device session this command belongs to
    epoch: int | None = None  # the overlay claim epoch: the device refuses a stale one


@dataclass
class View:
    visible: bool = False
    state: str = "hidden"  # hidden | unclaimed | tile | menu | lost
    expanded: bool = False
    banner: bool = False
    title: str = "NodX"
    sub: str = ""
    alert: str = ""  # DRAGGING / SCROLLING / KEYBOARD / LOST ... (prominent)
    colour: str = "idle"  # idle | ready | warn | danger
    progress: dict[str, float] = field(default_factory=dict)
    hover: str | None = None
    notice: str = ""
    exit: float = 0.0  # progress of the dwell that leaves Scroll (shown while scrolling)


def _actions(status: dict | None) -> dict:
    return (status or {}).get("actions") or {}


def session_active(status: dict | None) -> bool:
    s = status or {}
    uncal = ((s.get("handsFree") or {}).get("uncalDemo") or {}).get("active") is True
    return s.get("state") == "ACTIVE" and uncal


class OverlayModel:
    """The overlay's state machine. Feed it the pointer and the latest device status; it returns the commands
    to send and a `View` to draw. The overlay times only its own menu dwell; the firmware keeps timing target
    dwells, so no action can be executed by both."""

    def __init__(self, keyboard_clicks: str = "macos", epoch0: int | None = None):
        # Every loss, stop or hide starts a NEW epoch. Commands and replies carry the epoch they were made in; the
        # worker drops stale queued commands, the controller ignores stale replies, and the device refuses a claim
        # that is not newer than any it accepted. A time-based start keeps a restarted overlay above an old one.
        self.epoch = int(time.time()) if epoch0 is None else epoch0
        self.state = "hidden"
        self.keyboard_clicks = (
            keyboard_clicks  # macos: suppress NodX target clicks while the keyboard is open
        )
        self.dwell = Dwell()
        self.claimed = False
        self.menu_open = (
            False  # the menu is expanded (selections possible once the device acknowledges)
        )
        self.tile_locked = (
            False  # just selected: the pointer must leave the tile before it can expand again
        )
        self.left_at: float | None = None
        self.last_zone = False
        self.last_beat = -1e9
        self.last_ok = None  # time of the last successful bridge reply
        self.claim_sent_at = -1e9
        self.notice = ""
        self.notice_until = 0.0
        self.status: dict | None = None

    # ---- inputs -----------------------------------------------------------------------------------
    @property
    def session(self) -> int:
        return int(_actions(self.status).get("session") or 0)

    def _cmd(self, kind: str, value: object = None) -> "Command":
        return Command(kind, value, self.session, self.epoch)

    def on_reply(self, status: dict | None, now: float) -> None:
        """A bridge reply (or None for a failure)."""
        if status is not None:
            self.status = status
            self.last_ok = now

    def say(self, text: str, now: float, seconds: float = 8.0) -> None:
        self.notice, self.notice_until = text, now + seconds

    # ---- one tick ---------------------------------------------------------------------------------
    def tick(self, now: float, pointer: tuple[float, float], layout: Layout) -> list[Command]:
        out: list[Command] = []
        s = self.status
        a = _actions(s)
        comm_ok = self.last_ok is not None and now - self.last_ok <= LOST_AFTER_S
        if not comm_ok:
            # No word from the bridge: stop reporting (the device switches the overlay controls off by itself
            # after its own timeout), show it, and wait for an explicit dwell once contact returns.
            if self.state not in ("hidden", "lost"):
                self._lose(now)
            return out
        if not session_active(s):
            self._reset()  # no explicitly started session: nothing is shown and no claim is remembered
            return out
        if self.state == "hidden":
            self.state = "unclaimed"
        if (
            self.claimed
            and now - self.claim_sent_at > CLAIM_GRACE_S
            and a.get("controller") != "OVERLAY"
        ):
            # the device dropped (or never gave) our claim: never assume it back
            if a.get("controller") == "BROWSER":
                self.say("The browser palette is active: turn it off first.", now)
            self._lose(now)
        hit = hit_test(layout, *pointer)
        zone = on_overlay(layout, *pointer)
        dwell_s = (a.get("dwellMs") or DEFAULT_DWELL_S * 1000) / 1000.0
        if hit != "tile":
            self.tile_locked = False  # leaving the tile unlocks it
        if self.state in ("unclaimed", "lost", "tile"):
            on_tile = hit == "tile" and not self.tile_locked
            if self.dwell.update("tile" if on_tile else None, pointer, now, dwell_s):
                out += self._expand(now)
        elif self.state == "menu":
            ready = a.get("menu") is True and a.get("ready") is True  # acknowledged by the device
            target = hit if hit in self_items(layout) else None
            if ready and self.dwell.update(target, pointer, now, dwell_s):
                out += self._choose(target, now)
            elif not ready:
                self.dwell.reset()
            if zone:
                self.left_at = None
            elif self.left_at is None:
                self.left_at = now
            elif now - self.left_at > LEAVE_GRACE_S:
                self._collapse()
        # the overlay repeats whether the pointer is on it; the device acts on target dwells only while it is not
        if self.claimed and self.state in ("tile", "menu"):
            if zone != self.last_zone or now - self.last_beat >= HEARTBEAT_S:
                out.append(self._cmd("menu", zone))
                self.last_zone, self.last_beat = zone, now
        return out

    # ---- transitions ------------------------------------------------------------------------------
    def _expand(self, now: float) -> list[Command]:
        out: list[Command] = []
        if not self.claimed or self.state in ("lost", "unclaimed"):
            # An explicit dwell on the tile claims the controls, in an epoch above everything the device accepted.
            floor = int(_actions(self.status).get("epochFloor") or 0)
            self.epoch = max(self.epoch, floor + 1)
            out.append(self._cmd("overlay", True))
            self.claimed = True
            self.claim_sent_at = now
        self.state = "menu"
        self.menu_open = True
        self.left_at = None
        self.dwell.reset()
        self.dwell.locked = "tile"
        self.last_zone = False
        self.last_beat = -1e9  # report "on the overlay" at once
        return out

    def _collapse(self) -> None:
        self.state = "tile" if self.claimed else "unclaimed"
        self.menu_open = False
        self.left_at = None
        self.dwell.reset()

    def _choose(self, item: str | None, now: float) -> list[Command]:
        if item is None:
            return []
        out: list[Command] = []
        if item == "keyboard":
            out.append(self._cmd("keyboard_toggle"))
        elif item in SELECT_TARGETS:
            out.append(self._cmd("select", item))
        self._collapse()
        # one selection per hover: leave the tile before it can expand again
        self.tile_locked = True
        return out

    def _lose(self, now: float) -> None:
        self.epoch += 1  # everything queued or in flight so far is now stale
        self.claimed = False
        self.menu_open = False
        self.state = "lost"
        self.left_at = None
        self.dwell.reset()

    def _reset(self) -> None:
        if self.state != "hidden":
            self.epoch += (
                1  # the session ended: a delayed reply or queued command from it must not act
            )
        self.claimed = False
        self.menu_open = False
        self.state = "hidden"
        self.tile_locked = False
        self.dwell = Dwell()
        self.last_zone = False

    # ---- the picture ------------------------------------------------------------------------------
    def view(self, now: float, layout: Layout | None = None) -> View:
        a = _actions(self.status)
        v = View()
        v.state = self.state
        if self.state == "hidden":
            return v
        v.visible = True
        v.expanded = self.state == "menu"
        v.notice = self.notice if now < self.notice_until else ""
        if not v.notice and _actions(self.status).get("keyboard"):
            v.notice = KEYBOARD_HINT  # persistent while keyboard mode lasts
        if self.state == "lost":
            v.title, v.sub, v.alert, v.colour = (
                "NodX ▾",
                "",
                "LOST · dwell to resume",
                "danger",
            )
        elif self.state == "unclaimed":
            v.title, v.sub, v.colour = "NodX ▾", "Dwell to start", "idle"
        else:
            mode = (a.get("mode") or "LEFT").title().replace("Double", "Double-click")
            label = {
                "LEFT": "Left-click",
                "RIGHT": "Right-click",
                "DOUBLE": "Double-click",
                "DRAG": "Drag",
                "SCROLL": "Scroll",
            }.get(a.get("mode") or "LEFT", mode)
            keep = " · kept" if a.get("keep") and a.get("mode") in ("RIGHT", "DOUBLE") else ""
            one = (
                " · one shot" if not a.get("keep") and a.get("mode") in ("RIGHT", "DOUBLE") else ""
            )
            v.title, v.sub, v.colour = "NodX ▾", f"{label}{keep}{one}", "ready"
            if a.get("keyboard"):
                v.alert, v.colour = "KEYBOARD", "warn"
            if a.get("scroll") == "ACTIVE":
                v.alert, v.colour, v.banner = "SCROLLING", "warn", True
                v.expanded = False
                v.exit = float((a.get("exit") or {}).get("progress") or 0.0)
            if a.get("dragging"):
                v.alert, v.colour = "DRAGGING", "danger"
        if self.dwell.target:
            v.progress[self.dwell.target] = self.dwell.progress
            v.hover = self.dwell.target
        return v
