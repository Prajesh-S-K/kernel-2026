"""The overlay's only link to the device: HTTP to the existing local companion bridge (desktop/server.py).

It never opens a serial port and never injects OS mouse events. Every reply from the bridge carries the device's
full status frame, so the overlay's own commands double as status polls.
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


def body_for(kind: str, value: object = None) -> dict:
    """The validated request the bridge expects for each overlay command."""
    if kind == "status":
        return {"action": "status"}
    if kind == "overlay":
        return {"action": "actions", "op": "overlay", "enabled": bool(value)}
    if kind == "menu":
        return {"action": "actions", "op": "menu", "open": bool(value)}
    if kind == "select":
        return {"action": "actions", "op": "select", "target": str(value)}
    if kind == "keyboard":
        return {"action": "actions", "op": "keyboard", "enabled": bool(value)}
    raise ValueError(f"unknown overlay command {kind!r}")


class Worker:
    """Sends requests on a background thread so a slow serial link never freezes the overlay. Commands are sent
    in order; only a repeat of the SAME pending command at the tail of the queue is coalesced, so a selection can
    never be reordered behind a later 'menu' report."""

    def __init__(self, bridge: Bridge, clock=time.monotonic):
        self.bridge = bridge
        self.clock = clock
        self._queue: deque[tuple[str, dict]] = deque()
        self._replies: deque[tuple[float, dict | None, dict]] = deque()
        self._wake = threading.Event()
        self._lock = threading.Lock()
        self._stop = False
        self.last_sent = -1e9
        self.sent = 0
        self.failed = 0
        self.latencies: list[float] = []
        self._thread = threading.Thread(target=self._run, name="nodx-overlay-bridge", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop = True
        self._wake.set()

    def send(self, kind: str, value: object = None) -> None:
        body = body_for(kind, value)
        key = json.dumps([kind], sort_keys=True) if kind in ("menu", "status") else None
        with self._lock:
            if key and self._queue and self._queue[-1][0] == key:
                self._queue[-1] = (key, body)
            else:
                self._queue.append((key or "", body))
        self._wake.set()

    def ensure_traffic(self, now: float, interval: float = 0.5) -> None:
        """Make sure a request goes out at least every `interval` s (a status poll when nothing else is due)."""
        with self._lock:
            idle = not self._queue
        if idle and now - self.last_sent >= interval:
            self.send("status")

    def drain(self) -> list[tuple[float, dict | None, dict]]:
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
                if item is None:
                    break
                _key, body = item
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
                    self._replies.append((done, reply, body))
