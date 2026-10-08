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


def analyze(rows):
    """Returns a dict with `ok`, `problems`, the three movement descriptions and the recommended mapping."""
    problems = []
    if len(rows) < 20:
        return {"ok": False, "problems": ["too few telemetry samples with a seen sensor block"]}
    spans = movements(rows)
    if len(spans) != 3:
        problems.append(f"expected 3 separated movements (yaw, pitch, roll), found {len(spans)}")
        return {"ok": False, "problems": problems, "movementsFound": len(spans)}
    yaw, pitch, roll = (describe(rows, s) for s in spans)
    for name, item in (("yaw", yaw), ("pitch", pitch), ("roll", roll)):
        if item["span_deg"] < MIN_SPAN_DEG:
            problems.append(
                f"{name} movement swept only {item['span_deg']:.1f} deg on its dominant axis"
            )
        if item["first_sign"] == 0:
            problems.append(f"{name} movement has no clear first lobe")
        others = sorted(item["other_axes_deg"], reverse=True)
        if others[1] > 0.6 * others[0]:
            problems.append(
                f"{name} movement was not isolated: a second axis swept {others[1]:.0f} of {others[0]:.0f} deg"
            )
    axes = [yaw["axis"], pitch["axis"], roll["axis"]]
    if len(set(axes)) != 3:
        problems.append(f"the three movements did not use three different gyro axes: {axes}")
    if problems:
        return {"ok": False, "problems": problems, "yaw": yaw, "pitch": pitch, "roll": roll}
    # Gyro signs: yaw LEFT negative, pitch UP negative, roll RIGHT positive after mapping.
    gyro_signs = [-yaw["first_sign"], -pitch["first_sign"], roll["first_sign"]]
    # Accelerometer: mapped[2] = vertical (reads +1 g at rest), mapped[1] = lateral (+ for a right tilt).
    rest = rest_accel(rows)
    vertical = max(range(3), key=lambda a: abs(rest[a]))
    vertical_sign = 1 if rest[vertical] > 0 else -1
    lateral_candidates = [a for a in range(3) if a != vertical]
    lateral = max(lateral_candidates, key=lambda a: roll["accel_span"][a])
    forward = [a for a in range(3) if a not in (vertical, lateral)][0]
    first, last = spans[2]
    start_lateral = rows[first][2][lateral]
    # Direction the lateral accel axis moves during the FIRST lobe (a tilt to the right).
    lateral_move = 0.0
    for i in range(first, last + 1):
        delta = rows[i][2][lateral] - start_lateral
        if abs(delta) > 0.08:
            lateral_move = delta
            break
    lateral_sign = 1 if lateral_move > 0 else -1
    if lateral_move == 0:
        problems.append("the lateral accelerometer axis did not move enough during the roll")
    # With mapped az = vertical_sign*raw[vertical] and ay = lateral_sign*raw[lateral] (right tilt => ay up).
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
    if len(argv) != 2:
        print(
            "usage: bench_axes.py LOG (telemetry JSON lines from the real firmware)",
            file=sys.stderr,
        )
        return 2
    rows = samples_from_lines(Path(argv[1]).read_text(errors="replace").splitlines())
    result = analyze(rows)
    names = ["x", "y", "z"]
    print(f"{len(rows)} usable telemetry samples")
    for key, label in (
        ("yaw", "YAW (left first)"),
        ("pitch", "PITCH (up first)"),
        ("roll", "ROLL (right first)"),
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
        print("flags:", flags(result))
        return 0
    print("NOT RELIABLE:", "; ".join(result["problems"]))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
