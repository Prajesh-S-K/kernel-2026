"""The AppKit side of the overlay: a small non-activating floating panel. Needs the PyObjC packages in
overlay/requirements.txt. All decisions are made by `logic.OverlayModel`; this file only draws, hit-tests with
the real pointer position and moves/clamps the window.

Design points that matter for safety:
  * the panel never takes focus (non-activating, cannot become key or main), so the working application keeps
    its keyboard focus;
  * the whole window area absorbs mouse events (it ignores them but they do not pass through), so an operating
    system click that lands while the pointer is on the overlay is delivered to the overlay, not to the target
    underneath;
  * it makes no calls that move or press the mouse.
"""

from __future__ import annotations

import json
import os
import signal
import time
from pathlib import Path

import objc
from AppKit import (
    NSAppearance,
    NSApplication,
    NSApplicationActivationPolicyAccessory,
    NSApplicationDidChangeScreenParametersNotification,
    NSBackingStoreBuffered,
    NSBezierPath,
    NSBitmapImageFileTypePNG,
    NSColor,
    NSEvent,
    NSFont,
    NSFontAttributeName,
    NSForegroundColorAttributeName,
    NSMakeRect,
    NSMutableParagraphStyle,
    NSNotificationCenter,
    NSPanel,
    NSParagraphStyleAttributeName,
    NSScreen,
    NSStatusWindowLevel,
    NSTextAlignmentLeft,
    NSTimer,
    NSView,
    NSWindowCollectionBehaviorCanJoinAllSpaces,
    NSWindowCollectionBehaviorFullScreenAuxiliary,
    NSWindowCollectionBehaviorIgnoresCycle,
    NSWindowCollectionBehaviorStationary,
    NSWindowStyleMaskBorderless,
    NSWindowStyleMaskNonactivatingPanel,
)
from Foundation import NSAttributedString, NSObject

from . import logic as L
from . import osk as K
from .bridge import Bridge, Worker, body_for
from .controller import Controller
from .typing import Typer

PAD_TEXT = 12.0
SETTINGS = Path.home() / "Library" / "Application Support" / "NodX Overlay" / "position.json"

COLOURS = {
    "idle": ((0.13, 0.16, 0.19), (0.45, 0.55, 0.60)),
    "ready": ((0.07, 0.22, 0.19), (0.18, 0.83, 0.69)),
    "warn": ((0.25, 0.18, 0.04), (1.0, 0.69, 0.13)),
    "danger": ((0.30, 0.07, 0.08), (1.0, 0.35, 0.37)),
}


def _color(rgb, alpha=1.0):
    return NSColor.colorWithSRGBRed_green_blue_alpha_(rgb[0], rgb[1], rgb[2], alpha)


def _attrs(size, bold=False, rgb=(0.95, 0.97, 0.98), align=NSTextAlignmentLeft):
    style = NSMutableParagraphStyle.alloc().init()
    style.setAlignment_(align)
    font = NSFont.boldSystemFontOfSize_(size) if bold else NSFont.systemFontOfSize_(size)
    return {
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: _color(rgb),
        NSParagraphStyleAttributeName: style,
    }


def _text(string, rect, attrs):
    NSAttributedString.alloc().initWithString_attributes_(string, attrs).drawInRect_(rect)


def screens() -> list[tuple[int, L.Rect]]:
    out = []
    for screen in NSScreen.screens():
        v = screen.visibleFrame()
        number = int(screen.deviceDescription().get("NSScreenNumber", 0))
        out.append((number, L.Rect(v.origin.x, v.origin.y, v.size.width, v.size.height)))
    return out


class OverlayPanel(NSPanel):
    def canBecomeKeyWindow(self):  # never steal focus from the working application
        return False

    def canBecomeMainWindow(self):
        return False


class OverlayView(NSView):
    def initWithApp_(self, app):
        self = objc.super(OverlayView, self).initWithFrame_(NSMakeRect(0, 0, L.TILE_W, L.TILE_H))
        if self is None:
            return None
        self.app = app
        return self

    def isFlipped(self):
        return True  # top-left origin, like the logic layout

    def acceptsFirstMouse_(self, event):
        return True  # a first click is absorbed here, never passed to another window

    # Every mouse event over the overlay is absorbed. A real mouse or trackpad can still drag the tile to move
    # it; the head-controlled pointer never presses on it (the device inhibits target actions over the overlay).
    def mouseDown_(self, event):
        self.window().performWindowDragWithEvent_(event)
        self.app.moved()

    def mouseUp_(self, event):
        pass

    def mouseDragged_(self, event):
        pass

    def rightMouseDown_(self, event):
        pass

    def rightMouseUp_(self, event):
        pass

    def otherMouseDown_(self, event):
        pass

    def otherMouseUp_(self, event):
        pass

    def scrollWheel_(self, event):
        pass

    def drawRect_(self, dirty):
        self.app.draw(self)


class OskView(NSView):
    """The NodX keyboard's content view: keys are drawn here, chosen only by the overlay's dwell."""

    def initWithApp_(self, app):
        self = objc.super(OskView, self).initWithFrame_(NSMakeRect(0, 0, 100, 100))
        if self is None:
            return None
        self.app = app
        return self

    def isFlipped(self):
        return True

    def acceptsFirstMouse_(self, event):
        return True

    def mouseDown_(self, event):  # absorbed, like the tile: a physical click never reaches the window below
        pass

    def mouseUp_(self, event):
        pass

    def mouseDragged_(self, event):
        pass

    def rightMouseDown_(self, event):
        pass

    def scrollWheel_(self, event):
        pass

    def drawRect_(self, dirty):
        self.app.draw_osk(self)


class Target(NSObject):
    """The Objective-C target of the timer and the screen-change notification."""

    def initWithApp_(self, app):
        self = objc.super(Target, self).init()
        if self is None:
            return None
        self.app = app
        return self

    def tick_(self, _timer):
        self.app.tick()

    def screensChanged_(self, _note):
        self.app.screens_changed()


class OverlayApp:
    def __init__(self, controller, edge):
        self.controller = controller
        visibles = [r for _n, r in screens()]
        saved = self._load()
        base = (
            L.clamp_to_visible(L.Rect(saved["x"], saved["y"], L.TILE_W, L.TILE_H), visibles)
            if saved
            else L.default_tile_frame(visibles[0], edge)
        )
        self.tile = base
        self.layout = L.collapsed_layout(base)
        self.panel = self._make_panel(base)
        self.osk_panel = self._make_panel(base)
        self.osk_view = OskView.alloc().initWithApp_(self)
        self.osk_panel.setContentView_(self.osk_view)
        self.osk_shown = False
        self.osk_layout = None
        self.view = OverlayView.alloc().initWithApp_(self)
        self.panel.setContentView_(self.view)
        self.shown = False
        self.timer = None
        self.pointer = None
        self.diag = None
        self.quit_requested = False
        self.target = Target.alloc().initWithApp_(self)
        NSNotificationCenter.defaultCenter().addObserver_selector_name_object_(
            self.target, "screensChanged:", NSApplicationDidChangeScreenParametersNotification, None
        )

    # ---- window ------------------------------------------------------------------------------------
    def _make_panel(self, frame):
        panel = OverlayPanel.alloc().initWithContentRect_styleMask_backing_defer_(
            NSMakeRect(frame.x, frame.y, frame.w, frame.h),
            NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel,
            NSBackingStoreBuffered,
            False,
        )
        panel.setFloatingPanel_(True)  # (this resets the level, so the level is set after it)
        panel.setLevel_(NSStatusWindowLevel)
        panel.setHidesOnDeactivate_(False)
        panel.setOpaque_(False)
        panel.setBackgroundColor_(NSColor.clearColor())
        panel.setHasShadow_(True)
        panel.setIgnoresMouseEvents_(False)  # absorb clicks that land on the overlay
        panel.setBecomesKeyOnlyIfNeeded_(True)
        panel.setCollectionBehavior_(
            NSWindowCollectionBehaviorCanJoinAllSpaces
            | NSWindowCollectionBehaviorFullScreenAuxiliary
            | NSWindowCollectionBehaviorStationary
            | NSWindowCollectionBehaviorIgnoresCycle
        )
        panel.setAppearance_(NSAppearance.appearanceNamed_("NSAppearanceNameDarkAqua"))
        return panel

    def _load(self):
        try:
            data = json.loads(SETTINGS.read_text())
            return {"x": float(data["x"]), "y": float(data["y"])}
        except Exception:
            return None

    def _save(self):
        try:
            SETTINGS.parent.mkdir(parents=True, exist_ok=True)
            SETTINGS.write_text(json.dumps({"x": self.tile.x, "y": self.tile.y}))
        except Exception:
            pass

    def moved(self):
        """The user dragged the window with a real mouse: keep it on screen, snap it to an edge, remember it."""
        f = self.panel.frame()
        lay = self.layout
        gx = f.origin.x + lay.tile.x
        gy = f.origin.y + f.size.height - lay.tile.y - L.TILE_H
        visibles = [r for _n, r in screens()]
        self.tile = L.snap_to_edge(L.Rect(gx, gy, L.TILE_W, L.TILE_H), visibles)
        self._save()
        self.relayout()

    def screens_changed(self):
        visibles = [r for _n, r in screens()]
        self.tile = L.clamp_to_visible(self.tile, visibles)
        self.relayout()

    def _wanted_layout(self, now):
        v = self.controller.model.view(now)
        visibles = [r for _n, r in screens()]
        if v.banner:
            return L.expanded_layout(self.tile, visibles, banner=True)
        if self.controller.model.state == "menu":
            return L.expanded_layout(self.tile, visibles)
        if (
            v.notice
        ):  # a short message under (or above) the tile; the same room the scroll banner uses
            return L.expanded_layout(self.tile, visibles, banner=True)
        return L.collapsed_layout(self.tile)

    def relayout(self):
        self.layout = self._wanted_layout(time.monotonic())
        f = self.layout.frame
        cur = self.panel.frame()
        if (cur.origin.x, cur.origin.y, cur.size.width, cur.size.height) != (f.x, f.y, f.w, f.h):
            self.panel.setFrame_display_(NSMakeRect(f.x, f.y, f.w, f.h), True)

    # ---- the loop ----------------------------------------------------------------------------------
    def start(self):
        self.timer = NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(
            1 / 30, self.target, "tick:", None, True
        )

    def read_pointer(self):
        loc = NSEvent.mouseLocation()
        return loc.x, loc.y

    def tick(self):
        if self.quit_requested:
            self.shutdown()
            return
        now = time.monotonic()
        px, py = self.read_pointer()  # global points, origin bottom-left, any display
        self.relayout()
        self.osk_layout = self._wanted_osk()
        self.controller.tick(now, (px, py), self.layout, self.osk_layout)
        self.relayout()  # the state may have changed (expanded / collapsed)
        view = self.controller.model.view(now)
        if view.visible and not self.shown:
            self.panel.orderFrontRegardless()
            self.shown = True
        elif not view.visible and self.shown:
            self.panel.orderOut_(None)
            self.shown = False
        self._show_osk(view)
        self.pointer = (px, py)
        self.diag = None
        if os.environ.get("NODX_OVERLAY_DIAG") and view.visible:
            self.diag = self.controller.model.diagnostics(now, self.pointer, self.layout)
            self._diag_log(now, self.diag)
        events, self.controller.model.dwell.events = self.controller.model.dwell.events, []
        if os.environ.get("NODX_OVERLAY_DIAG") and events:
            self._event_log(events)
        self.view.setNeedsDisplay_(True)
        self._debug(now, view)

    def _wanted_osk(self):
        """The NodX keyboard's layout on the display that holds the tile (None while it is closed)."""
        if not self.controller.model.osk.open:
            return None
        visibles = [r for _n, r in screens()]
        here = next((v for v in visibles if v.contains(self.tile.x + 1, self.tile.y + 1)), None)
        return K.osk_layout(here or visibles[0])

    def _show_osk(self, view):
        want = self.osk_layout is not None and view.visible
        if want:
            f = self.osk_layout.frame
            cur = self.osk_panel.frame()
            if (cur.origin.x, cur.origin.y, cur.size.width, cur.size.height) != (f.x, f.y, f.w, f.h):
                self.osk_panel.setFrame_display_(NSMakeRect(f.x, f.y, f.w, f.h), True)
            if not self.osk_shown:
                self.osk_panel.orderFrontRegardless()
                self.osk_shown = True
            self.osk_view.setNeedsDisplay_(True)
        elif self.osk_shown:
            self.osk_panel.orderOut_(None)
            self.osk_shown = False

    def release_controls(self):
        """Tell the device the overlay is going away: close the menu report and release the claim, both under the
        epoch in force (a command from an older epoch would be refused)."""
        self.controller.worker.stop()
        bridge = self.controller.worker.bridge
        model = self.controller.model
        for kind in ("menu", "overlay"):
            bridge.post(body_for(kind, False, model.session, model.epoch))

    def shutdown(self):
        """A clean quit switches the overlay's device controls off at once (a held button is released by the
        device's normal path). A crash or kill -9 cannot do this: the device then times the overlay out in 5 s."""
        self.release_controls()
        NSApplication.sharedApplication().terminate_(None)

    def _event_log(self, events):
        """Every dwell restart/drop with its cause, the pointer and the device's menu/ready (never rate-limited)."""
        path = os.environ.get("NODX_OVERLAY_LOG")
        if not path:
            return
        a = (self.controller.model.status or {}).get("actions") or {}
        with open(path, "a") as out:
            for e in events:
                row = {"t": round(time.time(), 3), "event": e, "pointer": self.pointer,
                       "menu": a.get("menu"), "ready": a.get("ready"), "controller": a.get("controller")}
                out.write(json.dumps(row) + "\n")

    def _diag_log(self, now, d):
        """Append a diagnostics row (NODX_OVERLAY_LOG) when the reason or hovered item changes, at most 5 Hz."""
        path = os.environ.get("NODX_OVERLAY_LOG")
        row = (d["hit"], d["reason"], d["ready"], round(d["progress"], 1))
        if not path or row == getattr(self, "_last_diag", None) or now - getattr(self, "_diag_at", 0) < 0.2:
            return
        self._last_diag, self._diag_at = row, now
        with open(path, "a") as out:
            out.write(json.dumps({"t": round(time.time(), 3), "diag": d, "pointer": self.pointer}) + "\n")

    def _debug(self, now, view):
        """NODX_OVERLAY_LOG=<file>: append one JSON line whenever the overlay's state changes (for bench notes)."""
        path = os.environ.get("NODX_OVERLAY_LOG")
        if not path:
            return
        f = self.layout.frame
        row = (
            view.state,
            view.visible,
            view.alert,
            view.sub,
            round(f.x),
            round(f.y),
            round(f.w),
            round(f.h),
        )
        if row != getattr(self, "_last_row", None):
            self._last_row = row
            with open(path, "a") as out:
                out.write(
                    json.dumps(
                        {
                            "t": round(time.time(), 3),
                            "state": view.state,
                            "visible": view.visible,
                            "alert": view.alert,
                            "sub": view.sub,
                            "frame": [f.x, f.y, f.w, f.h],
                        }
                    )
                    + "\n"
                )

    # ---- drawing -----------------------------------------------------------------------------------
    def draw(self, view):
        now = time.monotonic()
        v = self.controller.model.view(now)
        lay = self.layout
        if not v.visible:
            return
        fill, edge = COLOURS.get(v.colour, COLOURS["idle"])
        ptr = getattr(self, "pointer", None)
        hover = L.hit_test(lay, *ptr) if ptr else None
        self._tile(lay.tile, v, fill, edge, hover == "tile", v.progress.get("tile", 0.0))
        d = getattr(self, "diag", None)
        if d:
            text = f"{d['hit']} {d['progress']:.2f} r{d['restarts']} m{d['moved']} {d['reason']}"
            _text(text, NSMakeRect(lay.tile.x + 4, lay.tile.y + lay.tile.h - 14, lay.tile.w - 8, 13), _attrs(9, False, (1, 1, 0.6)))
        for ident, label in L.MENU_ITEMS:
            rect = lay.items.get(ident)
            if rect is not None:
                self._item(rect, label, ident, v, hover == ident, v.progress.get(ident, 0.0))
        if lay.banner is not None:
            if v.banner:
                self._banner(lay.banner, v)
            else:
                self._notice(lay.banner, v.notice)

    def draw_osk(self, view):
        lay = self.osk_layout
        model = self.controller.model
        if lay is None or not model.osk.open:
            return
        self._rrect(L.Rect(0, 0, lay.frame.w, lay.frame.h), 14, (0.08, 0.10, 0.13), (0.45, 0.55, 0.60), 2.0)
        ptr = getattr(self, "pointer", None)
        over = K.osk_hit(lay, *ptr) if ptr else None
        osk = model.osk
        for key in lay.keys:
            hovered = over is not None and over.ident == key.ident
            on = key.kind == "shift" and osk.shift
            fill = (0.30, 0.07, 0.08) if key.kind == "close" else ((0.07, 0.30, 0.26) if on else (0.16, 0.20, 0.24))
            line = (1, 1, 1) if hovered else (0.45, 0.55, 0.60)
            self._rrect(key.rect, 8, fill, line, 3.0 if hovered else 1.5)
            label = key.label
            if key.kind == "char" and osk.shift:
                label = K.SHIFTED.get(label, label.upper())
            size = 20 if key.kind in ("char", "backspace", "return", "shift") else 15
            _text(
                label,
                NSMakeRect(key.rect.x + 8, key.rect.y + (key.rect.h - size - 6) / 2, key.rect.w - 16, size + 6),
                _attrs(size, True, (0.95, 0.97, 0.98)),
            )
            if hovered and osk.dwell.target == key.ident and osk.dwell.progress > 0:
                self._ring(key.rect.x + key.rect.w - 12, key.rect.y + 12, 7, osk.dwell.progress, (0.18, 0.83, 0.69))
        if not osk.typing_allowed:
            _text(
                "TYPING NOT ALLOWED YET: see the notice",
                NSMakeRect(PAD_TEXT, lay.frame.h - 18, lay.frame.w - 2 * PAD_TEXT, 14),
                _attrs(11, True, (1.0, 0.69, 0.13)),
            )

    def _rrect(self, r, radius, fill, line, width=2.0):
        path = NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
            NSMakeRect(r.x, r.y, r.w, r.h), radius, radius
        )
        _color(fill, 0.96).setFill()
        path.fill()
        _color(line).setStroke()
        path.setLineWidth_(width)
        path.stroke()

    def _ring(self, cx, cy, radius, progress, rgb):
        track = NSBezierPath.bezierPathWithOvalInRect_(
            NSMakeRect(cx - radius, cy - radius, 2 * radius, 2 * radius)
        )
        _color((0.25, 0.30, 0.34)).setStroke()
        track.setLineWidth_(5.0)
        track.stroke()
        if progress > 0:
            arc = NSBezierPath.bezierPath()
            arc.appendBezierPathWithArcWithCenter_radius_startAngle_endAngle_clockwise_(
                (cx, cy), radius, -90, -90 + 360 * progress, False
            )
            _color(rgb).setStroke()
            arc.setLineWidth_(5.0)
            arc.stroke()

    def _tile(self, r, v, fill, edge, hovered, progress):
        self._rrect(r, 12, fill, (1, 1, 1) if hovered else edge, 3.0 if hovered else 2.0)
        if v.alert:
            # DRAGGING / SCROLLING / KEYBOARD / LOST are the first thing you see
            _text(
                v.title,
                NSMakeRect(r.x + 12, r.y + 4, r.w - 52, 16),
                _attrs(11, True, (0.85, 0.9, 0.92)),
            )
            size = 18 if len(v.alert) < 14 else 12
            _text(v.alert, NSMakeRect(r.x + 12, r.y + 20, r.w - 48, 28), _attrs(size, True, edge))
        else:
            _text(v.title, NSMakeRect(r.x + 12, r.y + 6, r.w - 56, 22), _attrs(16, True))
            _text(
                v.sub,
                NSMakeRect(r.x + 12, r.y + 28, r.w - 52, 20),
                _attrs(13, False, (0.80, 0.86, 0.88)),
            )
        if progress > 0 or hovered:
            self._ring(r.x + r.w - 24, r.y + r.h / 2, 11, progress, (0.18, 0.83, 0.69))

    def _item(self, r, label, ident, v, hovered, progress):
        danger = ident in ("stop",)
        line = (1.0, 0.35, 0.37) if danger else ((1, 1, 1) if hovered else (0.45, 0.55, 0.60))
        fill = (0.30, 0.07, 0.08) if danger else (0.16, 0.20, 0.24 if not hovered else 0.30)
        self._rrect(r, 10, fill, line, 3.0 if hovered else 2.0)
        _text(label, NSMakeRect(r.x + 14, r.y + (r.h - 20) / 2 - 2, r.w - 60, 22), _attrs(16, True))
        if hovered or progress > 0:
            self._ring(r.x + r.w - 26, r.y + r.h / 2, 11, progress, (0.18, 0.83, 0.69))

    def _banner(self, r, v):
        self._rrect(r, 12, (0.25, 0.18, 0.04), (1.0, 0.69, 0.13), 3.0)
        warn = (1.0, 0.69, 0.13)
        _text("SCROLLING", NSMakeRect(r.x + 12, r.y + 6, r.w - 24, 20), _attrs(14, True, warn))
        _text(
            "HOLD STILL TO EXIT SCROLLING",
            NSMakeRect(r.x + 12, r.y + 26, r.w - 24, 40),
            _attrs(16, True, (1, 1, 1)),
        )
        _text(
            "Pausing to read also exits scrolling.",
            NSMakeRect(r.x + 12, r.y + 68, r.w - 24, 16),
            _attrs(11, False, (0.85, 0.85, 0.85)),
        )
        bar = NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
            NSMakeRect(r.x + 12, r.y + r.h - 20, r.w - 24, 9), 4.5, 4.5
        )
        _color((0.1, 0.08, 0.02)).setFill()
        bar.fill()
        if v.exit > 0:
            fillbar = NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
                NSMakeRect(r.x + 12, r.y + r.h - 20, (r.w - 24) * min(1.0, v.exit), 9), 4.5, 4.5
            )
            _color(warn).setFill()
            fillbar.fill()

    def _notice(self, r, text):
        self._rrect(r, 12, (0.10, 0.16, 0.24), (0.45, 0.65, 0.95), 2.0)
        _text(
            text,
            NSMakeRect(r.x + 10, r.y + 6, r.w - 20, r.h - 12),
            _attrs(12, False, (0.92, 0.95, 1.0)),
        )


def run(
    url: str, edge: str, keyboard_clicks: str, open_keyboard: str = "settings", keyboard: str = "apple"
) -> None:
    bridge = Bridge(url)
    worker = Worker(bridge)
    from .logic import OverlayModel

    controller = Controller(
        worker,
        OverlayModel(keyboard_clicks),
        opener=open_keyboard,
        keyboard_kind=keyboard,
        typer=Typer() if keyboard == "osk" else None,
    )
    NSApplication.sharedApplication().setActivationPolicy_(NSApplicationActivationPolicyAccessory)
    app = OverlayApp(controller, edge)

    def request_quit(*_args):
        app.quit_requested = True  # handled on the next timer tick, on the main thread

    signal.signal(signal.SIGINT, request_quit)
    signal.signal(signal.SIGTERM, request_quit)
    worker.start()
    app.start()
    NSApplication.sharedApplication().run()


def selftest(png_dir: str | None = None) -> dict:
    """Builds the panel OFF SCREEN, checks the properties that keep it safe, and renders every state to PNG files
    (no screen-recording permission is needed: the view draws itself into a bitmap)."""
    NSApplication.sharedApplication().setActivationPolicy_(NSApplicationActivationPolicyAccessory)

    class _NullWorker:
        def drain(self):
            return []

        def send(self, *a):
            pass

        def ensure_traffic(self, *a):
            pass

    controller = Controller(_NullWorker(), L.OverlayModel("macos"))
    app = OverlayApp(controller, "right")
    panel = app.panel
    panel.setFrameOrigin_((-20000.0, -20000.0))  # never visible during the check
    results = {
        "level": int(panel.level()),
        "floating": bool(panel.isFloatingPanel()),
        "nonactivating": bool(panel.styleMask() & NSWindowStyleMaskNonactivatingPanel),
        "canBecomeKey": bool(panel.canBecomeKeyWindow()),
        "canBecomeMain": bool(panel.canBecomeMainWindow()),
        "ignoresMouseEvents": bool(panel.ignoresMouseEvents()),
        "hidesOnDeactivate": bool(panel.hidesOnDeactivate()),
        "canJoinAllSpaces": bool(
            panel.collectionBehavior() & NSWindowCollectionBehaviorCanJoinAllSpaces
        ),
        "fullScreenAuxiliary": bool(
            panel.collectionBehavior() & NSWindowCollectionBehaviorFullScreenAuxiliary
        ),
        "screens": [{"id": n, "visible": [r.x, r.y, r.w, r.h]} for n, r in screens()],
        "scale": [float(s.backingScaleFactor()) for s in NSScreen.screens()],
    }
    if png_dir:
        os.makedirs(png_dir, exist_ok=True)
        shots = _states()
        written = []
        for name, status, state, claimed, hit in shots:
            m = controller.model
            m.status, m.last_ok, m.state, m.claimed = status, time.monotonic(), state, claimed
            m.dwell.target = hit[0] if hit else None
            m.dwell.progress = hit[1] if hit else 0.0
            now = time.monotonic()
            app.layout = app._wanted_layout(now)
            f = app.layout.frame
            panel.setContentSize_((f.w, f.h))
            app.view.setFrame_(NSMakeRect(0, 0, f.w, f.h))
            app.pointer = None
            rep = app.view.bitmapImageRepForCachingDisplayInRect_(app.view.bounds())
            app.view.cacheDisplayInRect_toBitmapImageRep_(app.view.bounds(), rep)
            data = rep.representationUsingType_properties_(NSBitmapImageFileTypePNG, {})
            path = os.path.join(png_dir, f"overlay-{name}.png")
            data.writeToFile_atomically_(path, True)
            written.append(path)
        results["png"] = written
        # the NodX keyboard (Shift on, one key mid-dwell)
        m = controller.model
        m.osk.show()
        m.osk.shift = True
        app.osk_layout = K.osk_layout(L.Rect(0, 0, 1440, 870))
        f = app.osk_layout.frame
        app.osk_panel.setContentSize_((f.w, f.h))
        app.osk_view.setFrame_(NSMakeRect(0, 0, f.w, f.h))
        m.osk.dwell.target, m.osk.dwell.progress = "g", 0.6
        k = app.osk_layout.key("g").rect
        app.pointer = (f.x + k.x + k.w / 2, f.top - (k.y + k.h / 2))
        rep = app.osk_view.bitmapImageRepForCachingDisplayInRect_(app.osk_view.bounds())
        app.osk_view.cacheDisplayInRect_toBitmapImageRep_(app.osk_view.bounds(), rep)
        path = os.path.join(png_dir, "overlay-9-nodx-keyboard.png")
        rep.representationUsingType_properties_(NSBitmapImageFileTypePNG, {}).writeToFile_atomically_(
            path, True
        )
        results["osk_png"] = path
    return results


def _status(**kw):
    a = {
        "enabled": True,
        "controller": "OVERLAY",
        "mode": "LEFT",
        "keep": False,
        "dragging": False,
        "scroll": "OFF",
        "keyboard": False,
        "menu": True,
        "ready": True,
        "dwellMs": 1200,
        "exit": {"progress": 0.0},
    }
    a.update(kw)
    return {"state": "ACTIVE", "handsFree": {"uncalDemo": {"active": True}}, "actions": a}


def _states():
    return [
        (
            "1-tile-unclaimed",
            _status(controller="NONE", menu=False, ready=False),
            "unclaimed",
            False,
            ("tile", 0.4),
        ),
        ("2-tile-left-click", _status(menu=False), "tile", True, None),
        ("3-tile-right-oneshot", _status(menu=False, mode="RIGHT"), "tile", True, None),
        ("4-menu-hover", _status(), "menu", True, ("drag", 0.62)),
        ("5-dragging", _status(menu=False, mode="DRAG", dragging=True), "tile", True, None),
        (
            "6-scrolling",
            _status(menu=False, mode="SCROLL", scroll="ACTIVE", exit={"progress": 0.55}),
            "tile",
            True,
            None,
        ),
        ("7-keyboard", _status(menu=False, keyboard=True), "tile", True, None),
        ("8-lost", _status(controller="NONE", menu=False, ready=False), "lost", False, None),
    ]
