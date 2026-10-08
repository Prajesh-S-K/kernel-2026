#!/usr/bin/env python3
"""Loopback companion server; one session drives the shared native C++ engine."""

import argparse
import hashlib
import json
import math
import re
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    from desktop.transport import NativeDevice, SerialDevice
except ImportError:
    from transport import NativeDevice, SerialDevice

ROOT = Path(__file__).resolve().parents[1]
LOCK = threading.Lock()
TRIAL_LOCK = threading.Lock()


def number(data, key, default, low, high):
    raw = data.get(key, default)
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        raise ValueError(f"Invalid {key}")
    value = float(raw)
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"Invalid {key}")
    return value


def integer(data, key, default, low, high):
    value = number(data, key, default, low, high)
    if not value.is_integer():
        raise ValueError(f"{key} must be an integer")
    return int(value)


def boolean(data, key, default=False):
    value = data.get(key, default)
    if not isinstance(value, bool):
        raise ValueError(f"{key} must be a boolean")
    return value


GESTURE_NAME = re.compile(r"(nod|turn|tilt)[1-3]")
MODES = ("LEGACY_SWITCH", "HANDS_FREE", "CONFIG_INVALID")


def required_boolean(data, key):
    if key not in data:
        raise ValueError(f"{key} is required")
    return boolean(data, key)


def command_for(data):
    action = data.get("action", "status")
    if action == "step":
        count = integer(data, "count", 5, 1, 50)
        motion = [number(data, axis, 0, -1000, 1000) for axis in ("yaw", "pitch", "roll")]
        flags = [
            int(boolean(data, k, default))
            for k, default in (("pressed", False), ("connected", True), ("automatic", True))
        ]
        fault = integer(data, "fault", 0, 0, 6)
        command = "step " + " ".join(map(str, [count, *motion, *flags, fault]))
        if "enabled" in data:  # additive: the simulated maintained control-enable switch
            command += f" {int(boolean(data, 'enabled'))}"
        return command
    if action == "enable":
        return f"enable {int(required_boolean(data, 'enabled'))}"
    if action == "gesture":
        name = data.get("name")
        if not isinstance(name, str) or not GESTURE_NAME.fullmatch(name):
            raise ValueError("Invalid gesture name")
        return f"gesture {name} {number(data, 'scale', 1, 0.25, 3):g}"
    if action == "train":
        operation = data.get("op")
        if operation in ("cancel", "accept"):
            return f"train {operation}"
        if operation == "start" and data.get("gesture") in ("pause", "drag"):
            return f"train start {data['gesture']}"
        raise ValueError("Invalid training request")
    if action == "handsfree":
        operation = data.get("op")
        if operation in ("commit", "legacy"):
            return f"handsfree {operation}"
        if operation == "switchless":
            return "handsfree switchless " + ("on" if required_boolean(data, "enabled") else "off")
        if operation == "demo":
            return "handsfree demo " + ("on" if required_boolean(data, "enabled") else "off")
        if operation == "uncal":
            return "handsfree uncal " + ("start" if required_boolean(data, "enabled") else "stop")
        if operation == "uncalreverse":
            return (
                f"handsfree uncal reverse {int(required_boolean(data, 'horizontal'))} "
                f"{int(required_boolean(data, 'vertical'))}"
            )
        if operation == "uncaldwell":
            return "handsfree uncal dwell " + ("on" if required_boolean(data, "enabled") else "off")
        if operation == "uncaldwellset":
            ms, tolerance = data.get("ms"), data.get("tolerance")
            if type(ms) is not int or not 500 <= ms <= 5000:
                raise ValueError("Dwell duration must be 500 to 5000 ms")
            if (
                type(tolerance) not in (int, float)
                or isinstance(tolerance, bool)
                or not 2 <= tolerance <= 50
            ):
                raise ValueError("Dwell tolerance must be 2 to 50")
            return f"handsfree uncal dwell set {ms} {float(tolerance):.1f}"
        if operation == "enable" and data.get("kind") in ("maintained", "momentary"):
            return f"handsfree enable {data['kind']}"
        raise ValueError("Invalid hands-free request")
    if action == "calibrate":
        return "calibrate guided" if data.get("guided") is True else "calibrate"
    if action == "capture":
        operation = data.get("op")
        if operation == "start":
            seconds = data.get("seconds")
            if type(seconds) is not int or not 1 <= seconds <= 10:
                raise ValueError("Capture length must be 1 to 10 seconds")
            return f"capture start {seconds}"
        if operation == "stop":
            return "capture stop"
        if operation == "get":
            offset = data.get("offset")
            if type(offset) is not int or not 0 <= offset < 1000:
                raise ValueError("Capture offset must be 0 to 999")
            return f"capture get {offset}"
        raise ValueError("Invalid capture request")
    if action == "quick":
        operation = data.get("op")
        if operation == "practice" and data.get("frame") in ("fallback", "configured"):
            return f"quick practice {data['frame']}"
        if operation in ("retry", "cancel", "accept", "clear"):
            return f"quick {operation}"
        if operation == "enable":
            return "quick enable " + ("on" if required_boolean(data, "enabled") else "off")
        if operation == "set":
            sensitivity, tolerance = data.get("sensitivity"), data.get("returnTolerance")
            for value, low, high, label in (
                (sensitivity, 0.5, 2.0, "Sensitivity"),
                (tolerance, 0.15, 0.6, "Return tolerance"),
            ):
                if type(value) not in (int, float) or isinstance(value, bool):
                    raise ValueError(f"{label} must be a number")
                if not low <= value <= high:
                    raise ValueError(f"{label} must be {low} to {high}")
            return f"quick set {float(sensitivity):.2f} {float(tolerance):.2f}"
        raise ValueError("Invalid quick gesture request")
    if action == "click":
        operation = data.get("op")
        if operation == "train" and data.get("frame") in ("fallback", "configured"):
            return f"click train {data['frame']}"
        if operation in ("cancel", "accept", "clear"):
            return f"click {operation}"
        if operation == "enable":
            return "click enable " + ("on" if required_boolean(data, "enabled") else "off")
        raise ValueError("Invalid click request")
    if action in ("map", "control"):
        operation = data.get("op")
        allowed = {
            "map": ("start", "cancel", "accept", "save", "clear"),
            "control": ("start", "stop"),
        }[action]
        if operation not in allowed:
            raise ValueError(f"Invalid {action} request")
        return f"{action} {operation}"
    if action in ("status", "cancel", "resume", "pause", "generic", "load", "corrupt"):
        return action
    if action in ("dwell", "scroll"):
        return f"{action} {int(boolean(data, 'enabled'))}"
    if action == "settings":
        return (
            f"settings {int(boolean(data, 'dwellEnabled'))} {int(boolean(data, 'scrollEnabled'))}"
        )
    if action == "record":
        return "record " + ("on" if boolean(data, "enabled") else "off")
    raise ValueError("Unknown action")


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / "ui"), **kwargs)

    def log_message(self, *args):
        pass

    def reply(self, code, data):
        payload = json.dumps(data, allow_nan=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        # Refuse cross-origin mutation of this local control session.
        origin = self.headers.get("Origin")
        if origin and origin != f"http://{self.headers.get('Host')}":
            self.reply(403, {"error": "Cross-origin request refused"})
            return
        try:
            size = int(self.headers.get("Content-Length", 0))
            if not 0 < size <= 65536:
                raise ValueError("Invalid request size")
            data = json.loads(self.rfile.read(size))
            if not isinstance(data, dict):
                raise ValueError("Expected a JSON object")
            with TRIAL_LOCK if self.path == "/api/trial" else LOCK:
                if self.path == "/api/trial":
                    required = (
                        "trial",
                        "condition",
                        "inputSource",
                        "distance",
                        "width",
                        "movementTimeMs",
                        "hit",
                        "profile",
                    )
                    if not isinstance(data, dict) or any(k not in data for k in required):
                        raise ValueError("Incomplete trial")
                    if data["inputSource"] not in ("SIMULATED", "HOST_POINTER"):
                        raise ValueError("Unknown input source")
                    for key in ("distance", "width", "movementTimeMs"):
                        number(data, key, 0, 0.001, 1e8)
                    boolean(data, "hit")
                    boolean(data, "aborted")
                    integer(data, "trial", 1, 1, 1000000)
                    if "interactionMode" in data and data["interactionMode"] not in MODES:
                        raise ValueError("Unknown interaction mode")
                    if "gestureInterruptions" in data:
                        integer(data, "gestureInterruptions", 0, 0, 1000000)
                    context = data.get("blockContext")
                    if context is not None:
                        if (
                            not isinstance(context, dict)
                            or context.get("profile") != data["profile"]
                        ):
                            raise ValueError("Trial profile differs from block context")
                        for key in (
                            "deviceSource",
                            "inputSource",
                            "condition",
                            "selectionMethod",
                            "interactionMode",
                            "gestureConfigId",
                        ):
                            if context.get(key) != data.get(key):
                                raise ValueError("Trial differs from block context")
                    encoded = json.dumps(data["profile"], sort_keys=True, allow_nan=False)
                    data["profileHash"] = hashlib.sha256(encoded.encode()).hexdigest()
                    data["softwareVersion"] = "0.2.0"
                    with (self.server.runtime / "trials.jsonl").open("a") as out:
                        out.write(json.dumps(data, allow_nan=False) + "\n")
                    self.reply(200, {"ok": True, "profileHash": data["profileHash"]})
                elif self.path == "/api/device" and data.get("action") == "reconnect":
                    device = self.server.device
                    if not hasattr(device, "reopen"):
                        raise ValueError("Reconnect is available only for the hardware device")
                    device.reopen()
                    self.reply(200, {"ok": True, "reason": "reconnecting; the board is rebooting"})
                elif self.path == "/api/device":
                    self.reply(200, self.server.device.request(command_for(data)))
                else:
                    self.reply(404, {"error": "Unknown endpoint"})
        except (ValueError, TypeError, KeyError, RuntimeError, OSError) as error:
            self.reply(400, {"error": str(error)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--runtime", type=Path, default=ROOT / "runtime")
    parser.add_argument("--executable", type=Path, default=ROOT / "build" / "nodx_sim")
    parser.add_argument("--serial", help="ESP32 USB serial port; needs pyserial==3.5")
    args = parser.parse_args()
    if not args.serial and not args.executable.exists():
        parser.error("Build nodx_sim first; see BUILD_GUIDE.md")
    args.runtime.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.runtime = args.runtime
    server.device = (
        SerialDevice(args.serial) if args.serial else NativeDevice(args.executable, args.runtime)
    )
    print(
        f"NodX companion: http://127.0.0.1:{args.port} ({'HARDWARE' if args.serial else 'SIMULATED'} device)",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.device.close()
        server.server_close()


if __name__ == "__main__":
    main()
