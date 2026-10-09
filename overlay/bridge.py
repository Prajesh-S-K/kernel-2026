"""The overlay's only link to the device: HTTP to the existing local companion bridge (desktop/server.py).

It never opens a serial port and never injects OS mouse events. Every reply from the bridge carries the device's
full status frame, so the overlay's own commands double as status polls.

Stale commands. A command or reply from an old overlay session must never act after a Stop, a timeout or a
reconnect. Each command therefore carries the claim epoch it was made under (and a claim also the session serial);
the device refuses anything that is not current, this worker drops queued commands whose epoch is no longer valid
before sending them, and the controller ignores replies that belong to an old epoch.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.request
from collections import deque

DEFAULT_URL = "http://127.0.0.1:8791/api/device"


class Bridge:
    """Synchronous client. `post` returns the parsed reply or None when the bridge could not be reached."""

    def __init__(self, url: str = DEFAULT_URL, timeout: float = 1.5):
        self.url = url
        self.timeout = timeout

    def post(self, body: dict) -> dict | None:
        request = urllib.request.Request(
            self.url, json.dumps(body).encode(), {"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                reply = json.load(response)
        except Exception:
            return None
        return reply if isinstance(reply, dict) else None


def body_for(
    kind: str, value: object = None, session: int | None = None, epoch: int | None = None
) -> dict:
    """The validated request the bridge expects for each overlay command. Every command except a status poll
    carries the claim epoch (a claim also the session serial), so the device can refuse a stale one."""
    if kind == "status":
        return {"action": "status"}
    if epoch is None:
        raise ValueError(f"overlay command {kind!r} needs its epoch")
    token: dict = {"epoch": int(epoch)}
    if kind == "overlay":
        if value:
            if session is None:
                raise ValueError("an overlay claim needs the session serial")
            token["session"] = int(session)
        return {"action": "actions", "op": "overlay", "enabled": bool(value), **token}
    if kind == "menu":
        return {"action": "actions", "op": "menu", "open": bool(value), **token}
    if kind == "select":
        return {"action": "actions", "op": "select", "target": str(value), **token}
    if kind == "keyboard":
        return {"action": "actions", "op": "keyboard", "enabled": bool(value), **token}
    raise ValueError(f"unknown overlay command {kind!r}")


class Worker:
    """Sends requests on a background thread so a slow serial link never freezes the overlay. Commands are sent in
    order; only a repeat of the SAME pending command at the tail of the queue (same epoch) is coalesced, so a
    selection can never be reordered behind a later 'menu' report. A command queued under an epoch that is no
    longer the valid one is dropped, never sent."""

    def __init__(self, bridge: Bridge, clock=time.monotonic):
        self.bridge = bridge
        self.clock = clock
        self._queue: deque[tuple[str, dict, int | None]] = deque()
        self._replies: deque[tuple[float, dict | None, dict, int | None]] = deque()
        self._wake = threading.Event()
        self._lock = threading.Lock()
        self._stop = False
        self.epoch: int | None = (
            None  # the epoch that is valid now; anything stamped otherwise is stale
        )
        self.last_sent = -1e9
        self.sent = 0
        self.failed = 0
        self.dropped = 0
        self.latencies: list[float] = []
        self._thread = threading.Thread(target=self._run, name="nodx-overlay-bridge", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop = True
        self._wake.set()

    def set_epoch(self, epoch: int) -> None:
        """A new epoch invalidates everything still queued from the old one."""
        with self._lock:
            self.epoch = epoch

    def send(
        self, kind: str, value: object = None, session: int | None = None, epoch: int | None = None
    ) -> None:
        body = body_for(kind, value, session, epoch)
        stamp = epoch if kind != "status" else self.epoch
        key = kind if kind in ("menu", "status") else ""
        with self._lock:
            tail = self._queue[-1] if self._queue else None
            if key and tail and tail[0] == key and tail[2] == stamp:
                self._queue[-1] = (key, body, stamp)
            else:
                self._queue.append((key, body, stamp))
        self._wake.set()

    def ensure_traffic(self, now: float, interval: float = 0.5) -> None:
        """Make sure a request goes out at least every `interval` s (a status poll when nothing else is due)."""
        with self._lock:
            idle = not self._queue
        if idle and now - self.last_sent >= interval:
            self.send("status")

    def drain(self) -> list[tuple[float, dict | None, dict, int | None]]:
        with self._lock:
            out = list(self._replies)
            self._replies.clear()
        return out

    def _run(self) -> None:
        while not self._stop:
            self._wake.wait(0.2)
            self._wake.clear()
            while True:
                with self._lock:
                    item = self._queue.popleft() if self._queue else None
                    current = self.epoch
                if item is None:
                    break
                _key, body, stamp = item
                if stamp is not None and current is not None and stamp != current:
                    self.dropped += 1  # queued under an old epoch: never sent
                    continue
                started = self.clock()
                self.last_sent = started
                reply = self.bridge.post(body)
                done = self.clock()
                self.sent += 1
                if reply is None:
                    self.failed += 1
                else:
                    self.latencies = (self.latencies + [(done - started) * 1000])[-200:]
                with self._lock:
                    self._replies.append((done, reply, body, stamp))
