#!/usr/bin/env python3
"""Deterministic synthetic fixtures; never label as human or device data."""

import csv
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for name, fault in (("synthetic_motion", False), ("synthetic_fault_recovery", True)):
    with (ROOT / "fixtures" / (name + ".csv")).open("w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(
            ["timestampMs", "gyroX", "gyroY", "gyroZ", "accelX", "accelY", "accelZ", "valid"]
        )
        for i in range(1, 601):
            yaw = 20 * math.sin(i * 0.04) if i > 30 else 0
            writer.writerow(
                [i * 10, "nan" if fault and i == 200 else round(yaw, 5), 0, 0, 0, 0, 1, 1]
            )
