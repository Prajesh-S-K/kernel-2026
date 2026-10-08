#!/usr/bin/env python3
"""Work out the gyro/accelerometer axis mapping from a guided bench capture of the REAL firmware.

The firmware's telemetry carries a `sensor` block in SENSOR coordinates, before any mapping: the last raw
gyro/accel frame and `angle`, the running integral of each raw gyro axis in degrees. The operator performs
three isolated movements, each separated by stillness: (1) YAW: turn LEFT, return, turn RIGHT, return;
(2) PITCH: tilt UP, return, tilt DOWN, return; (3) ROLL: tilt to the RIGHT (right ear down), return, tilt
LEFT, return. For each movement this finds the dominant sensor axis and the direction of the FIRST lobe.

Firmware conventions it maps onto (core/src/calibration.cpp, motion.cpp):
  * mapped yaw rate is NEGATIVE when turning LEFT, positive when turning right;
  * mapped pitch rate is NEGATIVE when tilting UP, positive when tilting down;
  * roll is POSITIVE for a tilt to the right: the mapped gyro roll rate and atan2(accel[1], accel[2]) must
    both increase for it, and accel[2] reads +1 g at rest (the mapped vertical axis reads positive).
The result is a measurement-based RECOMMENDATION for the START axis mapping, not a validation.
"""

import json
import sys
from pathlib import Path

ACTIVE_DPS = 6.0  # a gyro axis counts as moving above this rate
MIN_SPAN_DEG = 12.0  # a movement must sweep at least this much on its dominant axis
GAP_MS = 2500  # stillness this long separates two movements


def samples_from_lines(lines):
    """Telemetry JSON lines -> [(timeMs, angle[3], accel[3])] for lines with a seen sensor block."""
    rows = []
    for text in lines:
        text = text.strip()
        start = text.find("{")
        if start < 0:
            continue
        try:
            data = json.loads(text[start:])
        except ValueError:
            continue
        sensor = data.get("sensor")
        if not isinstance(sensor, dict) or not sensor.get("seen"):
            continue
        angle, accel = sensor.get("angle"), sensor.get("accel")
        if (
            isinstance(angle, list)
            and isinstance(accel, list)
            and len(angle) == 3
            and len(accel) == 3
            and isinstance(data.get("timeMs"), int)
        ):
            rows.append((data["timeMs"], [float(v) for v in angle], [float(v) for v in accel]))
    return rows


def movements(rows):
    """Split the capture into movements: runs of activity separated by at least GAP_MS of stillness.
    Returns a list of (first_index, last_index) over `rows`."""
    active = []
    for index in range(1, len(rows)):
        dt = (rows[index][0] - rows[index - 1][0]) / 1000.0
        if dt <= 0:
            active.append(False)
            continue
        rate = max(abs(rows[index][1][a] - rows[index - 1][1][a]) / dt for a in range(3))
        active.append(rate > ACTIVE_DPS)
    groups, current, last_active_time = [], None, None
    for offset, is_active in enumerate(active):
        index = offset + 1
        if is_active:
            if current is None or rows[index][0] - last_active_time > GAP_MS:
                if current is not None:
                    groups.append(tuple(current))
                current = [index - 1, index]
            else:
                current[1] = index
            last_active_time = rows[index][0]
    if current is not None:
        groups.append(tuple(current))
    return groups


def describe(rows, span):
    """Dominant gyro axis and first-lobe sign inside one movement, plus the accel change per axis."""
    first, last = span
    start = rows[first][1]
    best_axis, best_range = 0, -1.0
    for axis in range(3):
        values = [rows[i][1][axis] - start[axis] for i in range(first, last + 1)]
        spread = max(values) - min(values)
        if spread > best_range:
            best_axis, best_range = axis, spread
    values = [rows[i][1][best_axis] - start[best_axis] for i in range(first, last + 1)]
    # The first lobe is whichever side of the starting angle the series leaves towards first.
    first_sign = 0
    for value in values:
        if abs(value) >= MIN_SPAN_DEG / 3:
            first_sign = 1 if value > 0 else -1
            break
    accel_span = []
    for axis in range(3):
        series = [rows[i][2][axis] for i in range(first, last + 1)]
        accel_span.append(max(series) - min(series))
    return {
        "axis": best_axis,
        "span_deg": best_range,
        "first_sign": first_sign,
        "accel_span": accel_span,
        "other_axes_deg": [
            max(rows[i][1][a] for i in range(first, last + 1))
            - min(rows[i][1][a] for i in range(first, last + 1))
            for a in range(3)
        ],
    }


def rest_accel(rows, count=8):
    head = rows[:count]
    return [sum(r[2][a] for r in head) / len(head) for a in range(3)]


DIRECTIONS = {"yaw": ("left", "right"), "pitch": ("up", "down"), "roll": ("right", "left")}


def analyze(rows, declared=None):
    """Returns a dict with `ok`, `problems`, the three movement descriptions and the recommended mapping.

    The operator's pace varies, so movements are not matched by fixed timing: every burst of activity that
    swept at least MIN_SPAN_DEG is classified by its dominant sensor axis, and the FIRST burst seen on each
    axis is that movement's first lobe. Exactly three different axes must appear, in the guided order yaw,
    pitch, roll. `declared` is what the OPERATOR says they did first in each movement, for example
    {"yaw": "right", "pitch": "down", "roll": "left"}; no direction is assumed, and without it the capture
    is analysed but no signs are derived. Physics is then used as a cross-check: with the vertical axis known
    from gravity, the right-hand rule says which sign a LEFT turn must have on that axis."""
    problems = []
    declared = declared or {}
    if len(rows) < 20:
        return {"ok": False, "problems": ["too few telemetry samples with a seen sensor block"]}
    bursts = []
    for span in movements(rows):
        item = describe(rows, span)
        if item["span_deg"] >= MIN_SPAN_DEG:
            bursts.append((span, item))
    firsts, order = {}, []
    for span, item in bursts:
        if item["axis"] not in firsts:
            firsts[item["axis"]] = (span, item)
            order.append(item["axis"])
    if len(order) != 3:
        problems.append(
            f"expected movements on 3 different gyro axes (yaw, pitch, roll), found {len(order)}: {order}"
        )
        return {"ok": False, "problems": problems, "burstsFound": len(bursts)}
    spans = [firsts[axis][0] for axis in order]
    yaw, pitch, roll = (firsts[axis][1] for axis in order)
    for name, item in (("yaw", yaw), ("pitch", pitch), ("roll", roll)):
        if item["first_sign"] == 0:
            problems.append(f"{name} movement has no clear first lobe")
        others = sorted(item["other_axes_deg"], reverse=True)
        if others[1] > 0.6 * others[0]:
            problems.append(
                f"{name} movement was not isolated: a second axis swept {others[1]:.0f} of {others[0]:.0f} deg"
            )
    axes = order
    if problems:
        return {"ok": False, "problems": problems, "yaw": yaw, "pitch": pitch, "roll": roll}
    missing = [
        name for name in ("yaw", "pitch", "roll") if declared.get(name) not in DIRECTIONS[name]
    ]
    if missing:
        return {
            "ok": False,
            "problems": [f"the operator's first direction is needed for: {', '.join(missing)}"],
            "yaw": yaw,
            "pitch": pitch,
            "roll": roll,
            "gyroAxes": axes,
            "firstSigns": [yaw["first_sign"], pitch["first_sign"], roll["first_sign"]],
        }
    # What the FIRST lobe was, in the firmware's terms: raw sign of LEFT, UP and RIGHT on each axis.
    left_raw = yaw["first_sign"] if declared["yaw"] == "left" else -yaw["first_sign"]
    up_raw = pitch["first_sign"] if declared["pitch"] == "up" else -pitch["first_sign"]
    right_raw = roll["first_sign"] if declared["roll"] == "right" else -roll["first_sign"]
    # Firmware conventions: mapped yaw negative for LEFT, mapped pitch negative for UP, mapped roll positive for RIGHT.
    gyro_signs = [-left_raw, -up_raw, right_raw]
    # Accelerometer: mapped[2] = vertical (reads +1 g at rest), mapped[1] = lateral (+ for a right tilt).
    rest = rest_accel(rows)
    vertical = max(range(3), key=lambda a: abs(rest[a]))
    vertical_sign = 1 if rest[vertical] > 0 else -1
    lateral_candidates = [a for a in range(3) if a != vertical]
    lateral = max(lateral_candidates, key=lambda a: roll["accel_span"][a])
    forward = [a for a in range(3) if a not in (vertical, lateral)][0]
    first, last = spans[2]
    start_lateral = rows[first][2][lateral]
    first_lobe_move = 0.0
    for i in range(first, last + 1):
        delta = rows[i][2][lateral] - start_lateral
        if abs(delta) > 0.08:
            first_lobe_move = delta
            break
    if first_lobe_move == 0:
        problems.append("the lateral accelerometer axis did not move enough during the roll")
    right_move = first_lobe_move if declared["roll"] == "right" else -first_lobe_move
    lateral_sign = 1 if right_move > 0 else -1
    # Physics cross-check, independent of the operator: a LEFT turn is counter-clockwise seen from above, a
    # positive rotation about the UP direction. The gravity axis reads +1 g when it points up, so its raw
    # gyro rate for a LEFT turn has the sign of that reading.
    checks = []
    if yaw["axis"] != vertical:
        problems.append(
            f"the yaw rotation was about axis {yaw['axis']} but gravity is on axis {vertical}: the board was not level for the yaw"
        )
    else:
        expected_left_raw = vertical_sign
        agree = expected_left_raw == left_raw
        checks.append(
            f"yaw: right-hand rule predicts a raw LEFT sign of {expected_left_raw:+d}; measured with your answer {left_raw:+d}: {'agrees' if agree else 'CONFLICT'}"
        )
        if not agree:
            problems.append(
                "the declared yaw direction contradicts the gravity/right-hand-rule check"
            )
    return {
        "ok": not problems,
        "problems": problems,
        "yaw": yaw,
        "pitch": pitch,
        "roll": roll,
        "gyroAxes": axes,
        "gyroSigns": gyro_signs,
        "accelAxes": [forward, lateral, vertical],
        "accelSigns": [1, lateral_sign, vertical_sign],
        "restAccel": rest,
        "physicsChecks": checks,
        "burstsUsed": len(bursts),
    }


def flags(result):
    """PlatformIO build flags for the recommended mapping (empty when the analysis failed)."""
    if not result.get("ok"):
        return ""
    join = lambda values: ",".join(str(v) for v in values)  # noqa: E731
    return (
        f"-DNODX_GYRO_AXES={join(result['gyroAxes'])} -DNODX_GYRO_SIGNS={join(result['gyroSigns'])} "
        f"-DNODX_ACCEL_AXES={join(result['accelAxes'])} -DNODX_ACCEL_SIGNS={join(result['accelSigns'])}"
    )


def main(argv):
    if len(argv) < 2:
        print(
            "usage: bench_axes.py LOG [yaw=left|right] [pitch=up|down] [roll=right|left]\n"
            "  LOG: telemetry JSON lines from the real firmware; the = values are what the operator\n"
            "  says they did FIRST in each movement (nothing is assumed).",
            file=sys.stderr,
        )
        return 2
    declared = {}
    for item in argv[2:]:
        name, _, value = item.partition("=")
        declared[name] = value
    rows = samples_from_lines(Path(argv[1]).read_text(errors="replace").splitlines())
    result = analyze(rows, declared)
    names = ["x", "y", "z"]
    print(f"{len(rows)} usable telemetry samples")
    for key, label in (
        ("yaw", "YAW"),
        ("pitch", "PITCH"),
        ("roll", "ROLL"),
    ):
        if key in result:
            item = result[key]
            print(
                f"{label}: dominant gyro axis {names[item['axis']]}, swept {item['span_deg']:.0f} deg, "
                f"first lobe {'+' if item['first_sign'] > 0 else '-'}, other axes {[round(v) for v in item['other_axes_deg']]}"
            )
    if result.get("ok"):
        print(f"gyro axes (yaw,pitch,roll) = {result['gyroAxes']}  signs = {result['gyroSigns']}")
        print(
            f"accel axes (mapped 0,1,2) = {result['accelAxes']}  signs = {result['accelSigns']}  rest = {[round(v, 3) for v in result['restAccel']]}"
        )
        for line in result.get("physicsChecks", []):
            print("check:", line)
        print("flags:", flags(result))
        return 0
    for line in result.get("physicsChecks", []):
        print("check:", line)
    for line in result.get("physicsChecks", []):
        print("check:", line)
    print("NOT RELIABLE:", "; ".join(result["problems"]))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
