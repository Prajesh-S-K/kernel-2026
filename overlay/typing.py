"""Posting key events for the NodX keyboard. Key events only: this module never creates a mouse event.

It uses the system's Quartz event services through ctypes (no extra package). macOS drops posted events unless
the application running the overlay is allowed to control the computer (System Settings > Privacy & Security >
Accessibility), so `allowed()` is checked before the keyboard types, and nothing is posted without it.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import subprocess

SETTINGS_URL = "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"
NEEDS_PERMISSION_TEXT = (
    "NodX keyboard is shown but cannot type yet: allow the app running the overlay (named 'Python') under "
    "System Settings > Privacy & Security > Accessibility, then try again. No key is sent until then."
)
HID_EVENT_TAP = 0


class Typer:
    def __init__(self, lib=None):
        self._lib = lib
        self.posted = 0

    def _load(self):
        if self._lib is None:
            path = ctypes.util.find_library("ApplicationServices")
            lib = ctypes.CDLL(path)
            lib.AXIsProcessTrusted.restype = ctypes.c_bool
            lib.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
            lib.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
            lib.CGEventKeyboardSetUnicodeString.argtypes = [
                ctypes.c_void_p,
                ctypes.c_ulong,
                ctypes.POINTER(ctypes.c_uint16),
            ]
            lib.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
            self._lib = lib
        return self._lib

    def open_settings(self, runner=subprocess.run) -> bool:
        """Open the Privacy > Accessibility pane. Changes nothing."""
        try:
            return runner(["open", SETTINGS_URL], timeout=5, check=False).returncode == 0
        except Exception:
            return False

    def allowed(self) -> bool:
        try:
            return bool(self._load().AXIsProcessTrusted())
        except Exception:
            return False

    def press(self, request) -> bool:
        """('text', str) types that text; ('key', code) presses a virtual key. False when nothing was posted."""
        try:
            lib = self._load()
            if not lib.AXIsProcessTrusted():
                return False
            kind, value = request
            for down in (True, False):
                code = int(value) if kind == "key" else 0
                event = lib.CGEventCreateKeyboardEvent(None, code, down)
                if not event:
                    return False
                if kind == "text":
                    units = value.encode("utf-16-le")
                    array = (ctypes.c_uint16 * (len(units) // 2)).from_buffer_copy(units)
                    lib.CGEventKeyboardSetUnicodeString(event, len(array), array)
                lib.CGEventPost(HID_EVENT_TAP, event)
                if hasattr(lib, "CFRelease"):
                    lib.CFRelease(event)
            self.posted += 1
            return True
        except Exception:
            return False
