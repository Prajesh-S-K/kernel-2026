"""The macOS Accessibility Keyboard, honestly.

What this macOS offers (verified on macOS 27.0.1): the Accessibility Keyboard is hosted by the system input
method "Assistive Control" (started by launchd on demand). There is NO public API, app bundle, command-line tool
or documented URL that opens it. The supported ways to show it are the user-facing switch in System Settings >
Accessibility > Keyboard and the Accessibility Shortcut. Driving those by UI scripting or posting key events would
be brittle (and a second input-injection path), so the overlay does not do it.

So the "Keyboard" menu item:
  * if the Accessibility Keyboard is already running, switches NodX into keyboard mode;
  * otherwise opens System Settings at the Accessibility pane (a supported deep link) and says what to switch on
    once. It never enables a setting by itself and it never claims the keyboard opened.

Detection uses NSWorkspace (running applications): it needs no permission. It can only tell that the keyboard's
host process is running, not that its window is on screen.
"""

from __future__ import annotations

import subprocess

SETTINGS_URL = "x-apple.systempreferences:com.apple.Accessibility-Settings.extension"
HOST_NAME_HINTS = ("assistive control", "accessibility keyboard")
HOST_BUNDLE_PREFIXES = ("com.apple.assistivecontrol", "com.apple.accessibility.keyboard")

SETUP_TEXT = (
    "One-time setup: System Settings > Accessibility > Keyboard > Accessibility Keyboard: switch it on, "
    "and turn on its dwell option so keys are chosen by holding the pointer still. Then choose Keyboard again."
)
OPENED_TEXT = (
    "Keyboard mode: NodX clicks are paused. The Accessibility Keyboard's host process is running, but NodX "
    "cannot see whether a keyboard is on screen. No keyboard? Dwell on the tile and choose Keyboard again, "
    "Cancel or any action to get NodX clicks back."
)
CLOSED_TEXT = (
    "Keyboard mode off: NodX clicks are active again. This does NOT switch off the Accessibility Keyboard's own "
    "dwell: hide the keyboard or turn its dwell off so only one thing clicks (see docs/OVERLAY.md)."
)
GONE_TEXT = "The Accessibility Keyboard stopped running: NodX clicks are active again."
MISSING_TEXT = "The Accessibility Keyboard is not running. " + SETUP_TEXT
SETUP_TEXT_NODX = (
    "One-time setup: System Settings > Accessibility > Keyboard > Accessibility Keyboard: switch it on and "
    "leave all its dwell options OFF: NodX makes the clicks, so only one thing clicks. Then choose Keyboard again."
)


def missing_text(keyboard_clicks: str = "macos") -> str:
    """What to tell the user when the keyboard is not running. With NodX doing the clicking, the macOS dwell
    options must stay off (two click generators would double-click keys)."""
    setup = SETUP_TEXT_NODX if keyboard_clicks == "nodx" else SETUP_TEXT
    return "The Accessibility Keyboard is not running. " + setup


def running_apps() -> list[tuple[str, str]]:
    """(name, bundle id) of the running applications; empty when AppKit is not available."""
    try:
        from AppKit import NSWorkspace
    except Exception:
        return []
    return [
        (str(a.localizedName() or ""), str(a.bundleIdentifier() or ""))
        for a in NSWorkspace.sharedWorkspace().runningApplications()
    ]


def keyboard_running(apps: list[tuple[str, str]] | None = None) -> bool:
    for name, bundle in apps if apps is not None else running_apps():
        if any(h in name.lower() for h in HOST_NAME_HINTS) or bundle.lower().startswith(
            HOST_BUNDLE_PREFIXES
        ):
            return True
    return False


PREF_DOMAIN = "com.apple.universalaccess"
PREF_KEY = "virtualKeyboardOnOff"  # the preference behind the Accessibility Keyboard switch (macOS 27.0.1)
REQUESTED_TEXT = "Opening the Accessibility Keyboard (asked macOS to switch it on)..."
NOT_STARTED_TEXT = "macOS did not start the Accessibility Keyboard. "


def switch_on(runner=subprocess.run) -> bool:
    """Ask macOS to switch the Accessibility Keyboard on by setting its own on/off preference, exactly what the
    switch in System Settings stores. UNDOCUMENTED and unverified across macOS versions: the caller must check
    that the host process really starts, and fall back to opening the Settings pane when it does not. It sets only
    this one preference (never any dwell option) and never turns the keyboard off."""
    try:
        done = runner(
            ["defaults", "write", PREF_DOMAIN, PREF_KEY, "-bool", "true"], timeout=5, check=False
        )
        return done.returncode == 0
    except Exception:
        return False


def open_settings(runner=subprocess.run) -> bool:
    """Open System Settings at the Accessibility pane (a supported deep link). Changes nothing."""
    try:
        return runner(["open", SETTINGS_URL], timeout=5, check=False).returncode == 0
    except Exception:
        return False
