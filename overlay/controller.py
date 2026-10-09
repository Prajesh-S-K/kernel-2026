"""Ties the pure model to the bridge worker. No AppKit: the app feeds it the pointer and the layout."""

from __future__ import annotations

from . import keyboard as kb
from .bridge import Worker
from .logic import Command, Layout, OverlayModel


class Controller:
    def __init__(self, worker: Worker, model: OverlayModel | None = None, keyboard=kb, clock=None):
        self.worker = worker
        self.model = model or OverlayModel()
        self.keyboard = keyboard
        self.opened_settings = 0
        self.log: list[tuple[str, object]] = []

    def tick(self, now: float, pointer: tuple[float, float], layout: Layout) -> list[Command]:
        for done, reply, body in self.worker.drain():
            self.model.on_reply(reply if reply and "state" in reply else None, now)
        commands = self.model.tick(now, pointer, layout)
        for command in commands:
            self._dispatch(command, now)
        # replies must keep flowing even when nothing else is due (the tile appears from a status poll)
        if self.model.state in ("hidden", "unclaimed", "lost"):
            self.worker.ensure_traffic(now)
        return commands

    def _dispatch(self, command: Command, now: float) -> None:
        self.log.append((command.kind, command.value))
        if command.kind in ("overlay", "menu", "select"):
            self.worker.send(command.kind, command.value)
        elif command.kind == "keyboard_toggle":
            self._keyboard(now)

    def _keyboard(self, now: float) -> None:
        a = (self.model.status or {}).get("actions") or {}
        if a.get("keyboard"):
            self.worker.send("keyboard", False)  # the explicit way back to ordinary NodX control
            self.model.say("Keyboard mode off: NodX clicks are active again.", now)
            return
        if self.keyboard.keyboard_running():
            if self.model.keyboard_clicks == "macos":
                self.worker.send("keyboard", True)
                self.model.say(kb.OPENED_TEXT, now)
            else:
                self.model.say("Keyboard: NodX dwell clicks stay active (no macOS dwell).", now)
            return
        self.opened_settings += 1
        self.keyboard.open_settings()
        self.model.say(kb.MISSING_TEXT, now, seconds=20.0)
