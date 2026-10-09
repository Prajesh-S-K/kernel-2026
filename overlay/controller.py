"""Ties the pure model to the bridge worker. No AppKit: the app feeds it the pointer and the layout."""

from __future__ import annotations

from . import keyboard as kb
from .bridge import Worker
from .logic import Command, Layout, OverlayModel
from .typing import NEEDS_PERMISSION_TEXT


OPEN_WAIT_S = 4.0  # how long to wait for the keyboard's host process after asking macOS to start it
MENU_POLL_S = 0.2  # status poll interval while the menu is open


class Controller:
    def __init__(
        self,
        worker: Worker,
        model: OverlayModel | None = None,
        keyboard=kb,
        opener: str = "settings",
        keyboard_kind: str = "apple",
        typer=None,
    ):
        self.keyboard_kind = keyboard_kind  # osk: the NodX keyboard; apple: the macOS Accessibility Keyboard
        self.typer = typer
        self._typing_check = -1e9
        self.opener = opener  # switch: ask macOS to switch the keyboard on; settings: open the Settings pane
        self._opening_until: float | None = None
        self.worker = worker
        self.model = model or OverlayModel()
        self.keyboard = keyboard
        self.opened_settings = 0
        self.stale_replies = 0
        self._kb_check = 0.0
        self.log: list[tuple[str, object]] = []

    def tick(
        self, now: float, pointer: tuple[float, float], layout: Layout, osk_layout=None
    ) -> list[Command]:
        for _done, reply, body, stamp in self.worker.drain():
            if stamp is not None and stamp != self.model.epoch:
                self.stale_replies += (
                    1  # a delayed reply from an old epoch must not drive the model
                )
                continue
            status = reply if reply and "state" in reply else None
            if status is not None and body.get("action") != "status":
                status = self._without_menu_state(status)
            self.model.on_reply(status, now)
        self.worker.set_epoch(self.model.epoch)
        self._watch_typing(now)
        commands = self.model.tick(now, pointer, layout, osk_layout)
        self.worker.set_epoch(
            self.model.epoch
        )  # the tick may have started a new epoch (loss, stop)
        for command in commands:
            self._dispatch(command, now)
        self._watch_keyboard(now)
        # replies must keep flowing even when nothing else is due (the tile appears from a status poll). While the
        # menu is open a status poll is also what tells the overlay the device has acknowledged it (see
        # _without_menu_state), so it is polled quickly then.
        if self.model.state in ("hidden", "unclaimed", "lost"):
            self.worker.ensure_traffic(now)
        elif self.model.state == "menu":
            self.worker.ensure_traffic(now, MENU_POLL_S)
        return commands

    def _without_menu_state(self, status: dict) -> dict:
        """The device builds the reply to a command before its next control tick, from the previous tick's time, so
        a menu report made by that very command looks 'from the future' and the reply says menu/ready = false even
        though the device has accepted it (seen on the bench: every menu-command reply said not ready, every plain
        status poll said ready). Replies to commands therefore keep the last status poll's menu/ready."""
        merged = dict(status)
        actions = dict(merged.get("actions") or {})
        before = (self.model.status or {}).get("actions") or {}
        for key in ("menu", "ready"):
            actions[key] = before.get(key, False) if actions.get("controller") == "OVERLAY" else False
        merged["actions"] = actions
        return merged

    def _dispatch(self, command: Command, now: float) -> None:
        self.log.append((command.kind, command.value))
        if command.kind in ("overlay", "menu", "select"):
            self.worker.send(command.kind, command.value, command.session, command.epoch)
        elif command.kind == "keyboard_toggle":
            if self.keyboard_kind == "osk":
                self._toggle_osk(now)
            else:
                self._keyboard(now, command)
        elif command.kind == "key" and self.typer is not None:
            self.typer.press(command.value)

    def _toggle_osk(self, now: float) -> None:
        if not self.model.toggle_osk():
            self.model.say("NodX keyboard closed.", now, seconds=6.0)
            return
        self._typing_check = -1e9
        self._watch_typing(now)
        if self.model.osk.typing_allowed:
            self.model.say(
                "NodX keyboard: first click where the text should go, then dwell on a key. Close, Stop or "
                "Keyboard again hides it.",
                now,
                seconds=12.0,
            )

    def _watch_typing(self, now: float) -> None:
        """While the NodX keyboard is open, keep checking the macOS permission to post key events."""
        if not self.model.osk.open or self.typer is None or now - self._typing_check < 2.0:
            return
        first = self._typing_check < -1e8
        self._typing_check = now
        allowed = bool(self.typer.allowed())
        self.model.osk.typing_allowed = allowed
        if not allowed and first:
            self.opened_settings += 1
            if hasattr(self.typer, "open_settings"):
                self.typer.open_settings()
            self.model.say(NEEDS_PERMISSION_TEXT, now, seconds=25.0)

    def _send_keyboard(self, on: bool) -> None:
        self.worker.send("keyboard", on, self.model.session, self.model.epoch)

    def _watch_keyboard(self, now: float) -> None:
        """While keyboard mode lasts, notice if the keyboard's host process has gone: NodX clicks come back."""
        self._finish_opening(now)
        a = (self.model.status or {}).get("actions") or {}
        if not a.get("keyboard") or not self.model.claimed or now - self._kb_check < 2.0:
            return
        self._kb_check = now
        if not self.keyboard.keyboard_running():
            self._send_keyboard(False)
            self.model.say(kb.GONE_TEXT, now, seconds=20.0)

    def _keyboard(self, now: float, command: Command | None = None) -> None:
        a = (self.model.status or {}).get("actions") or {}
        if a.get("keyboard"):
            self._send_keyboard(False)  # the way back to ordinary NodX clicks
            self.model.say(kb.CLOSED_TEXT, now, seconds=20.0)
            return
        if self.keyboard.keyboard_running():
            self._enter_keyboard(now)
            return
        if self.opener == "switch" and self.keyboard.switch_on():
            self._opening_until = now + OPEN_WAIT_S
            self.model.say(kb.REQUESTED_TEXT, now, seconds=OPEN_WAIT_S)
            return
        self._open_settings(now, "")

    def _enter_keyboard(self, now: float) -> None:
        # The host process is running. That does NOT mean a keyboard is on screen, so the message says so and
        # keeps the way back (Keyboard again, Cancel, any action, Stop) in view.
        if self.model.keyboard_clicks == "macos":
            self._send_keyboard(True)
            self.model.say(kb.OPENED_TEXT, now, seconds=20.0)
        else:
            self.model.say("Keyboard: NodX dwell clicks stay active (no macOS dwell).", now)

    def _open_settings(self, now: float, prefix: str) -> None:
        self.opened_settings += 1
        self.keyboard.open_settings()
        self.model.say(prefix + kb.missing_text(self.model.keyboard_clicks), now, seconds=20.0)

    def _finish_opening(self, now: float) -> None:
        """After asking macOS to switch the keyboard on: continue once its host process runs, or give up and
        open Settings (never claim it opened)."""
        if self._opening_until is None:
            return
        if not self.model.claimed:
            self._opening_until = None  # the overlay lost or released its controls meanwhile: do nothing
        elif self.keyboard.keyboard_running():
            self._opening_until = None
            self._enter_keyboard(now)
        elif now >= self._opening_until:
            self._opening_until = None
            self._open_settings(now, kb.NOT_STARTED_TEXT)
