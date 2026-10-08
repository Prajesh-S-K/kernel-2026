"""Bounded native/serial transport; a failed native session must be restarted."""

import faulthandler
import json
import math
import os
import select
import subprocess
import time

REQUEST_TIMEOUT_SECONDS = 2.0
MAX_REPLY_BYTES = 16384


def decode_reply(line):
    data = json.loads(
        line, parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Nonfinite reply"))
    )
    if not isinstance(data, dict) or not isinstance(data.get("ok"), bool):
        raise ValueError("Malformed device reply")

    if "protocol" in data and (type(data["protocol"]) is not int or data["protocol"] != 1):
        raise ValueError("Invalid protocol revision")
    if "requestId" in data:
        request_id = data["requestId"]
        if type(request_id) is not int or not 0 <= request_id <= 4294967295:
            raise ValueError("Invalid reply request ID")

    def finite(value):
        if isinstance(value, float):
            return math.isfinite(value)
        if isinstance(value, dict):
            return all(finite(item) for item in value.values())
        if isinstance(value, list):
            return all(finite(item) for item in value)
        return True

    if not finite(data):
        raise ValueError("Nonfinite device reply")
    return data


class NativeDevice:
    def __init__(self, executable, runtime):
        self.process = subprocess.Popen(
            [str(executable), str(runtime)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            bufsize=0,
        )
        self.pending = bytearray()
        self.failed = False

    def request(self, command):
        if self.failed or self.process.poll() is not None:
            raise RuntimeError("Control engine stopped; restart the companion")
        deadline = time.monotonic() + REQUEST_TIMEOUT_SECONDS
        try:
            self.process.stdin.write((command + "\n").encode())
            while time.monotonic() < deadline:
                ready, _, _ = select.select(
                    [self.process.stdout], [], [], max(0, deadline - time.monotonic())
                )
                if not ready:
                    break
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError("Control engine stopped")
                self.pending.extend(chunk)
                if len(self.pending) > MAX_REPLY_BYTES:
                    raise ValueError("Device reply too large")
                if b"\n" in self.pending:
                    line, _, remainder = self.pending.partition(b"\n")
                    self.pending = bytearray(remainder)
                    return decode_reply(line)
            raise RuntimeError("Control engine reply timed out after two seconds")
        except (OSError, ValueError, RuntimeError):
            self.failed = True
            self.close()
            raise

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=0.2)
        self.process.stdin.close()
        self.process.stdout.close()


class SerialDevice:
    """Correlate acknowledgements; retain fragments until a complete JSON line."""

    def __init__(self, port):
        import serial

        # DTR/RTS are held LOW before the port opens: on the ESP32-S3 native USB port their edges
        # reset the chip, and the companion must not reboot the device by connecting to it.
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.05
        self.serial.write_timeout = 0.2
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.sequence = 0
        # Optional raw capture of everything the device sends (bench evidence; kept local).
        tee = os.environ.get("NODX_SERIAL_TEE")
        self.tee = open(tee, "ab", buffering=0) if tee else None

    def request(self, command):
        # Optional bench trace (NODX_SERIAL_TRACE=<file>): every non-status command, every slow or
        # failed request, and a thread dump if one request runs longer than four seconds.
        trace = os.environ.get("NODX_SERIAL_TRACE")
        if not trace:
            return self._request(command)
        started = time.monotonic()
        with open(trace, "a", buffering=1) as log:
            faulthandler.dump_traceback_later(4, file=log)
            try:
                reply = self._request(command)
                outcome = "ok"
                return reply
            except Exception as error:
                outcome = f"ERROR {error}"
                raise
            finally:
                faulthandler.cancel_dump_traceback_later()
                elapsed = time.monotonic() - started
                if outcome != "ok" or elapsed > 0.5 or command.split()[0] not in ("status", "step"):
                    log.write(f"{time.time():.3f} {command[:60]!r} {outcome} {elapsed:.3f}s\n")

    def _request(self, command):
        op = command.split()[0]
        if op in ("corrupt", "record"):
            raise ValueError("This action is available only for the native simulator")
        if op == "step":
            command = "status"
        elif op in ("dwell", "scroll"):
            command = op + (" on" if command.split()[1] == "1" else " off")
        deadline = time.monotonic() + REQUEST_TIMEOUT_SECONDS
        self.serial.reset_input_buffer()
        self.sequence = self.sequence % 4294967295 + 1
        self.serial.write((f"@{self.sequence} {command}\n").encode())
        pending = bytearray()
        while time.monotonic() < deadline:
            fragment = self.serial.readline()
            if getattr(self, "tee", None) and fragment:
                self.tee.write(fragment)
            pending.extend(fragment)
            if len(pending) > MAX_REPLY_BYTES:
                raise ValueError("Serial reply too large")
            # Decode a complete object even when a serial driver omits the newline.
            try:
                data = decode_reply(pending.strip())
            except ValueError:
                data = None
            if data and data.get("protocol") == 1 and data.get("requestId") == self.sequence:
                return data
            if b"\n" in pending:
                lines = pending.split(b"\n")
                pending = bytearray(lines.pop())
                for line in lines:
                    try:
                        data = decode_reply(line)
                    except ValueError:
                        continue
                    if data.get("protocol") == 1 and data.get("requestId") == self.sequence:
                        return data
        raise RuntimeError("ESP32 acknowledgement timed out after two seconds")

    def close(self):
        self.serial.close()
