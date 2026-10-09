"""Ties the pure model to the bridge worker. No AppKit: the app feeds it the pointer and the layout."""

from __future__ import annotations

from . import keyboard as kb
from .bridge import Worker
from .logic import Command, Layout, OverlayModel


class Controller:
    def __init__(self, worker: Worker, model: OverlayModel | None = None, keyboard=kb):
        self.worker = worker
        self.model = model or OverlayModel()
        self.keyboard = keyboard
        self.opened_settings = 0
        self.stale_replies = 0
        self._kb_check = 0.0
        self.log: list[tuple[str, object]] = []

    def tick(self, now: float, pointer: tuple[float, float], layout: Layout) -> list[Command]:
        for _done, reply, _body, stamp in self.worker.drain():
            if stamp is not None and stamp != self.model.epoch:
                self.stale_replies += (
                    1  # a delayed reply from an old epoch must not drive the model
                )
                continue
            self.model.on_reply(reply if reply and "state" in reply else None, now)
        self.worker.set_epoch(self.model.epoch)
        commands = self.model.tick(now, pointer, layout)
        self.worker.set_epoch(
            self.model.epoch
        )  # the tick may have started a new epoch (loss, stop)
        for command in commands:
            self._dispatch(command, now)
        self._watch_keyboard(now)
        # replies must keep flowing even when nothing else is due (the tile appears from a status poll)
        if self.model.state in ("hidden", "unclaimed", "lost"):
            self.worker.ensure_traffic(now)
        return commands

    def _dispatch(self, command: Command, now: float) -> None:
        self.log.append((command.kind, command.value))
        if command.kind in ("overlay", "menu", "select"):
            self.worker.send(command.kind, command.value, command.session, command.epoch)
        elif command.kind == "keyboard_toggle":
            self._keyboard(now, command)

    def _send_keyboard(self, on: bool) -> None:
        self.worker.send("keyboard", on, self.model.session, self.model.epoch)

    def _watch_keyboard(self, now: float) -> None:
        """While keyboard mode lasts, notice if the keyboard's host process has gone: NodX clicks come back."""
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
            # The host process is running. That does NOT mean a keyboard is on screen, so the message says so and
            # keeps the way back (Keyboard again, Cancel, any action, Stop) in view.
            if self.model.keyboard_clicks == "macos":
                self._send_keyboard(True)
                self.model.say(kb.OPENED_TEXT, now, seconds=20.0)
            else:
                self.model.say("Keyboard: NodX dwell clicks stay active (no macOS dwell).", now)
            return
        self.opened_settings += 1
        self.keyboard.open_settings()
        self.model.say(kb.MISSING_TEXT, now, seconds=20.0)
