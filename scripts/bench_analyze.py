#!/usr/bin/env python3
"""Turn bench-diagnostic serial logs (DIAG,... and BLE,... lines) into a plain summary.

Every threshold below is a START value for a first breadboard look. A line saying PASS means "this
reading is inside the START range", never "the hardware is validated". Missing readings are reported
as MISSING, not silently skipped.
"""

import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"(?:^|\s)(DIAG|BLE),([a-zA-Z0-9_-]+),(.*)$")


def parse_line(text):
    """Return (family, stage, {key: value}) or None. Values stay strings; a/b/c triples are split."""
    match = LINE.search(text.rstrip("\r\n"))
    if not match:
        return None
    family, stage, rest = match.groups()
    fields = {}
    for part in rest.split(","):
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key.strip()] = value.strip()
    return family, stage, fields


def number(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def triple(value):
    parts = (value or "").split("/")
    numbers = [number(p) for p in parts]
    return numbers if len(numbers) == 3 and None not in numbers else None


def collect(lines):
    """Group readings. Later lines of the same key overwrite earlier ones, except lists."""
    data = {
        "info": {},
        "scan": {"addresses": []},
        "imu": {},
        "button": {"burstList": [], "toggleList": []},
        "ble": {"states": [], "sends": [], "warnings": []},
        "regs": [],
        "raw": {},
        "ignored": 0,
    }
    for text in lines:
        parsed = parse_line(text)
        if not parsed:
            if text.strip():
                data["ignored"] += 1
            continue
        family, stage, fields = parsed
        if family == "DIAG" and stage in ("info", "imu"):
            data[stage].update(fields)
        elif family == "DIAG" and stage == "scan":
            if "address" in fields:
                data["scan"]["addresses"].append(fields["address"].lower())
            else:
                data["scan"].update(fields)
        elif family == "DIAG" and stage == "raw":
            if "summary" in fields or "rangeAccelLsb" in fields or "minAccelLsb" in fields:
                data["raw"].update(fields)
        elif family == "DIAG" and stage == "regs":
            data["regs"].append(fields)
        elif family == "DIAG" and stage == "button":
            if "burst" in fields:
                data["button"]["burstList"].append(fields)
            elif "gateToggle" in fields:
                data["button"]["toggleList"].append(fields)
            else:
                data["button"].update(fields)
        elif family == "BLE":
            if stage == "state":
                data["ble"]["states"].append(fields)
            elif stage == "send":
                data["ble"]["sends"].append(fields)
            elif stage == "warn":
                data["ble"]["warnings"].append(",".join(fields) or "warning")
    return data


def check(name, status, detail):
    return {"name": name, "status": status, "detail": detail}


def judge(data):
    out = []
    # ---- stage 0: board facts against the candidate N16R8 configuration
    info = data["info"]
    if not info:
        out.append(check("board facts", "MISSING", "no DIAG,info lines"))
    else:
        flash, psram = number(info.get("flashBytes")), number(info.get("psramBytes"))
        out.append(
            check(
                "flash size is the 16 MB candidate",
                "PASS" if flash == 16 * 1024 * 1024 else "FAIL",
                f"flashBytes={info.get('flashBytes')} flashMode={info.get('flashMode')} (DIO/QIO question stays open)",
            )
        )
        out.append(
            check(
                "PSRAM is the 8 MB candidate (heap-reported, within 1% below 8 MiB)",
                "PASS"
                if psram is not None and 0.99 * 8 * 1024 * 1024 <= psram <= 8 * 1024 * 1024
                else "FAIL",
                f"psramBytes={info.get('psramBytes')}",
            )
        )
        out.append(
            check(
                "chip and USB facts recorded",
                "INFO",
                f"chip={info.get('chip')} rev={info.get('revision')} usbCdcOnBoot={info.get('usbCdcOnBoot', 'not-set')} reset={info.get('resetReason')}",
            )
        )
    # ---- stage 1: bus
    scan = data["scan"]
    if "found" not in scan and "error" not in scan:
        out.append(check("I2C scan", "MISSING", "no DIAG,scan result"))
    elif "error" in scan:
        out.append(check("I2C scan", "FAIL", scan["error"]))
    else:
        out.append(
            check(
                "SDA and SCL idle HIGH (pull-ups present)",
                "PASS" if scan.get("idleSda") == "1" and scan.get("idleScl") == "1" else "FAIL",
                f"idleSda={scan.get('idleSda')} idleScl={scan.get('idleScl')}",
            )
        )
        out.append(
            check(
                "MPU6050 answers at 0x68",
                "PASS" if "0x68" in data["scan"]["addresses"] else "FAIL",
                f"addresses={data['scan']['addresses']}",
            )
        )
    # ---- stage 2: sensor
    imu = data["imu"]
    if not imu:
        out.append(check("MPU6050 test", "MISSING", "no DIAG,imu lines"))
    elif "error" in imu:
        out.append(check("MPU6050 test", "FAIL", imu["error"]))
    else:
        out.append(
            check(
                "WHO_AM_I is 0x68",
                "PASS" if imu.get("whoAmI", "").lower() == "0x68" else "FAIL",
                f"whoAmI={imu.get('whoAmI')}",
            )
        )
        out.append(
            check(
                "firmware init register sequence accepted",
                "PASS" if imu.get("firmwareInitSequenceOk") == "1" else "FAIL",
                f"ok={imu.get('firmwareInitSequenceOk')}",
            )
        )
        without, with_ = (
            number(imu.get("dataReadyNoIntEnable")),
            number(imu.get("dataReadyWithIntEnable")),
        )
        if without is None or with_ is None:
            out.append(
                check("data-ready gating the firmware uses", "MISSING", "no dataReady counts")
            )
        elif without > 0:
            out.append(
                check(
                    "data-ready gating the firmware uses",
                    "PASS",
                    f"status bit seen {int(without)} times without INT_ENABLE",
                )
            )
        elif with_ > 0:
            out.append(
                check(
                    "data-ready gating the firmware uses",
                    "FAIL",
                    "the status bit only appears after INT_ENABLE is set: the shipped read path would never see a frame",
                )
            )
        else:
            out.append(
                check(
                    "data-ready gating the firmware uses",
                    "FAIL",
                    "the data-ready bit never appeared",
                )
            )
        seconds, changed = number(imu.get("seconds")), number(imu.get("framesChanged"))
        if seconds and changed is not None:
            rate = changed / seconds
            out.append(
                check(
                    "distinct sensor frames about 100 per second (START 80-120)",
                    "PASS" if 80 <= rate <= 120 else "FAIL",
                    f"{rate:.1f} frames/s",
                )
            )
        out.append(
            check(
                "no I2C read errors",
                "PASS" if imu.get("i2cErrors") == "0" else "FAIL",
                f"i2cErrors={imu.get('i2cErrors')}",
            )
        )
        gmean, gstd = triple(imu.get("gyroMeanDps")), triple(imu.get("gyroStdDps"))
        if gmean and gstd:
            out.append(
                check(
                    "gyro bias under 5 dps and noise under 1 dps at rest (START)",
                    "PASS" if max(abs(v) for v in gmean) < 5 and max(gstd) < 1 else "FAIL",
                    f"mean={gmean} std={gstd}",
                )
            )
        mag = number(imu.get("accelMagMeanG"))
        if mag is not None:
            out.append(
                check(
                    "accelerometer magnitude about 1 g (START 0.9-1.1)",
                    "PASS" if 0.9 <= mag <= 1.1 else "FAIL",
                    f"{mag:.4f} g",
                )
            )
        worst = number(imu.get("intervalUsMax"))
        if worst is not None:
            out.append(
                check(
                    "worst 10 ms poll interval under 30 ms (START)",
                    "PASS" if worst < 30000 else "FAIL",
                    f"{worst:.0f} us",
                )
            )
    if data["regs"]:
        registers = ", ".join(
            f"{r['reg']}={r['value']}" for r in data["regs"] if "reg" in r and r.get("read") == "1"
        )
        frames = [r["raw"] for r in data["regs"] if "raw" in r]
        out.append(check("MPU registers read-only dump", "INFO", registers))
        if frames:
            out.append(
                check(
                    "raw frames identical across the dump (a live sensor is not)",
                    "FAIL" if len(set(frames)) == 1 else "PASS",
                    f"{len(frames)} frames, {len(set(frames))} distinct",
                )
            )
    raw = data["raw"]
    if raw:
        accel, gyro = triple(raw.get("rangeAccelLsb")), triple(raw.get("rangeGyroLsb"))
        if accel and gyro:
            # START: a gentle tilt through several orientations moves a live accelerometer by well over
            # 0.2 g (3277 LSB at +-2 g) on some axis, and a gyro by over 5 dps (655 LSB at +-250 dps).
            out.append(
                check(
                    "raw accel responds to tilting (range over 0.2 g on some axis, START)",
                    "PASS" if max(accel) >= 3277 else "FAIL",
                    f"ranges={accel} LSB, distinctFrames={raw.get('distinctFrames')}",
                )
            )
            out.append(
                check(
                    "raw gyro responds to tilting (range over 5 dps on some axis, START)",
                    "PASS" if max(gyro) >= 655 else "FAIL",
                    f"ranges={gyro} LSB",
                )
            )
    # ---- stage 3: enable button
    button = data["button"]
    if "idleLevel" not in button and "error" not in button:
        out.append(check("enable button test", "MISSING", "no DIAG,button lines"))
    elif "error" in button:
        out.append(check("enable button test", "FAIL", button["error"]))
    else:
        out.append(
            check(
                "pin idles HIGH with the internal pull-up",
                "PASS" if button.get("idleLevel") == "1" else "FAIL",
                f"idleLevel={button.get('idleLevel')}",
            )
        )
        bursts = button["burstList"]
        presses = [b for b in bursts if b.get("kind") == "press"]
        out.append(
            check(
                "at least one press and one release observed",
                "PASS" if presses and any(b.get("kind") == "release" for b in bursts) else "FAIL",
                f"bursts={len(bursts)}",
            )
        )
        worst_edges = max((number(b.get("edges"), 0) for b in bursts), default=0)
        worst_span = max((number(b.get("spanUs"), 0) for b in bursts), default=0)
        out.append(
            check(
                "bounce stays within 20 edges and 10 ms per burst (START)",
                "PASS" if worst_edges <= 20 and worst_span <= 10000 else "FAIL",
                f"worst edges={int(worst_edges)} worst span={int(worst_span)} us",
            )
        )
        out.append(
            check(
                "no edges dropped by the recorder",
                "PASS" if button.get("droppedEdges") == "0" else "FAIL",
                f"dropped={button.get('droppedEdges')}",
            )
        )
        toggles = int(number(button.get("gateToggles"), 0))
        out.append(
            check(
                "the real gate toggled once per press that lasted 30 ms or more",
                "INFO",
                f"presses={len(presses)} gateToggles={toggles} (a shorter press is correctly ignored)",
            )
        )
    # ---- stage 4: BLE
    ble = data["ble"]
    if not ble["states"] and not ble["sends"]:
        out.append(check("BLE probe", "MISSING", "no BLE lines"))
    else:
        linked = any(
            s.get("secured") == "1" and s.get("subscribed") == "1" and s.get("connected") == "1"
            for s in ble["states"]
        )
        out.append(
            check(
                "a secured, subscribed link was seen",
                "PASS" if linked else "FAIL",
                f"state lines={len(ble['states'])}",
            )
        )
        delivered = [s for s in ble["sends"] if s.get("delivered") == "1"]
        failed = [s for s in ble["sends"] if s.get("delivered") == "0"]
        if ble["sends"]:
            out.append(
                check(
                    "commanded reports delivered",
                    "PASS" if delivered and not failed else "FAIL",
                    f"delivered={len(delivered)} failed={len(failed)}",
                )
            )
        for warning in ble["warnings"]:
            out.append(check("link lost while the button was down", "WARN", warning))
    return out


def report(checks):
    lines = [
        "Bench readings against START thresholds. PASS means 'inside the START range', not 'validated'.",
        "",
    ]
    width = max((len(c["status"]) for c in checks), default=4)
    for item in checks:
        lines.append(f"{item['status']:<{width}}  {item['name']}: {item['detail']}")
    counts = {}
    for item in checks:
        counts[item["status"]] = counts.get(item["status"], 0) + 1
    lines += ["", "Totals: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))]
    return "\n".join(lines)


def analyze_text(text):
    data = collect(text.splitlines())
    checks = judge(data)
    return {"checks": checks, "report": report(checks), "ignoredLines": data["ignored"]}


def main(argv):
    if len(argv) != 2:
        print("usage: bench_analyze.py LOGFILE", file=sys.stderr)
        return 2
    result = analyze_text(Path(argv[1]).read_text(errors="replace"))
    print(result["report"])
    print(json.dumps({"ignoredLines": result["ignoredLines"]}))
    return 1 if any(c["status"] == "FAIL" for c in result["checks"]) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
