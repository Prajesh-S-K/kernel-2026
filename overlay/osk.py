"""The NodX on-screen keyboard: pure layout, hit testing and dwell typing. No AppKit, no I/O.

Keys are chosen by the overlay's own dwell (never by a mouse click), so no second click generator exists: the
device does not act on target dwells while the pointer is over the overlay, and the macOS Accessibility Keyboard
and its dwell are not involved. A key produces a typing request that `typing.Typer` posts as a key event; it never
produces a mouse event. Coordinates follow `logic`: screen points with the origin bottom-left, a window's local
layout with the origin top-left.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from .logic import Dwell, Rect

KEY = 52.0
GAP = 6.0
PAD = 10.0
MARGIN = 8.0
KEY_DWELL_FACTOR = 0.6  # a key dwell is this fraction of the device dwell time...
KEY_DWELL_MIN_S = 0.45  # ...but never shorter than this

SHIFTED = {
    **dict(zip("1234567890", "!@#$%^&*()", strict=True)),
    ",": ";",
    ".": ":",
    "'": '"',
    "-": "_",
    "?": "?",
    "@": "@",
}

# (id, label, kind, width in key units)
ROWS: tuple[tuple[tuple[str, str, str, float], ...], ...] = (
    tuple((c, c, "char", 1.0) for c in "1234567890") + (("backspace", "⌫", "backspace", 1.6),),
    tuple((c, c, "char", 1.0) for c in "qwertyuiop"),
    tuple((c, c, "char", 1.0) for c in "asdfghjkl") + (("return", "⏎", "return", 1.6),),
    (("shift", "⇧", "shift", 1.4),) + tuple((c, c, "char", 1.0) for c in "zxcvbnm,."),
    (
        ("close", "Close", "close", 1.6),
        ("space", "space", "space", 4.4),
        ("-", "-", "char", 1.0),
        ("'", "'", "char", 1.0),
        ("?", "?", "char", 1.0),
        ("@", "@", "char", 1.0),
    ),
)
BACKSPACE_CODE = 51
RETURN_CODE = 36


@dataclass(frozen=True)
class Key:
    ident: str
    label: str
    kind: str  # char | space | backspace | return | shift | close
    rect: Rect  # LOCAL (top-left origin)


@dataclass(frozen=True)
class OskLayout:
    frame: Rect  # the window frame (screen coordinates)
    keys: tuple[Key, ...]

    def key(self, ident: str) -> Key | None:
        return next((k for k in self.keys if k.ident == ident), None)


def osk_layout(visible: Rect) -> OskLayout:
    """The keyboard centred at the bottom of `visible` (shrunk to fit narrow screens)."""
    widest = max(sum(w for *_x, w in row) + (len(row) - 1) * GAP / KEY for row in ROWS)
    width = widest * KEY + 2 * PAD
    scale = min(1.0, max(0.5, (visible.w - 2 * MARGIN) / width))
    unit, gap, pad = KEY * scale, GAP * scale, PAD * scale
    width_px = widest * unit + 2 * pad
    height_px = len(ROWS) * unit + (len(ROWS) - 1) * gap + 2 * pad
    # whole points: macOS rounds window frames, and the hit test must use the frame the window really has
    width_px, height_px = float(math.ceil(width_px)), float(math.ceil(height_px))
    frame = Rect(
        float(round(visible.x + (visible.w - width_px) / 2)),
        float(round(visible.y + MARGIN)),
        width_px,
        height_px,
    )
    keys = []
    for r, row in enumerate(ROWS):
        x = pad
        y = pad + r * (unit + gap)
        for ident, label, kind, w in row:
            kw = w * unit + (w - 1) * gap if w > 1 else unit
            keys.append(Key(ident, label, kind, Rect(x, y, kw, unit)))
            x += kw + gap
    return OskLayout(frame, tuple(keys))


def osk_hit(layout: OskLayout, gx: float, gy: float) -> Key | None:
    lx, ly = gx - layout.frame.x, layout.frame.top - gy
    return next((k for k in layout.keys if k.rect.contains(lx, ly)), None)


def key_dwell_s(device_dwell_s: float) -> float:
    return max(KEY_DWELL_MIN_S, KEY_DWELL_FACTOR * device_dwell_s)


class Osk:
    """Open/close, one-shot Shift, and one dwell per key (leave a key before it can type again)."""

    def __init__(self):
        self.open = False
        self.shift = False
        self.typing_allowed = True  # false until the macOS permission to post key events exists
        self.dwell = Dwell()
        self.typed = 0

    def show(self) -> None:
        self.open = True
        self.shift = False
        self.dwell = Dwell()

    def close(self) -> None:
        self.open = False
        self.shift = False
        self.dwell = Dwell()

    def update(
        self, layout: OskLayout, pointer: tuple[float, float], now: float, device_dwell_s: float
    ):
        """Feed the pointer. Returns a typing request ('text', str) / ('key', code), or None. Never fires when the
        keyboard is closed or typing is not allowed (the dwell still shows, so the user sees why nothing types)."""
        if not self.open:
            return None
        key = osk_hit(layout, *pointer)
        if not self.dwell.update(key.ident if key else None, pointer, now, key_dwell_s(device_dwell_s)):
            return None
        return self._press(key)

    def _press(self, key: Key):
        if key.kind == "close":
            self.close()
            return None
        if key.kind == "shift":
            self.shift = not self.shift
            return None
        if not self.typing_allowed:
            return None
        if key.kind == "backspace":
            self.typed += 1
            return ("key", BACKSPACE_CODE)
        if key.kind == "return":
            self.typed += 1
            return ("key", RETURN_CODE)
        if key.kind == "space":
            self.typed += 1
            return ("text", " ")
        text = key.label
        if self.shift:
            text = SHIFTED.get(text, text.upper())
            self.shift = False
        self.typed += 1
        return ("text", text)
