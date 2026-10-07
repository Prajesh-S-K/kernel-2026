# PROTOCOL · version 1

## Browser bridge

Loopback only. `POST /api/device` accepts a JSON object with `action`: status, step, calibrate, cancel, resume, pause, generic, load, dwell, scroll, corrupt, record. Dwell/scroll/record take boolean `enabled`. Step takes count (1–50; default 5), yaw/pitch/roll (°/s), pressed, connected, automatic, fault (0–6). Automatic means mathematical input during native calibration; normal manual inputs otherwise. Number bounds and nonfinite inputs are rejected. Cross-origin mutations are refused. Use one control tab/session.

Desktop process commands are one ASCII line, one JSON result line. `step count yaw pitch roll pressed connected automatic fault` executes fixed 10ms samples. Results include version/source, logical time, system/cursor/calibration/dwell state, reason, counters, filtered motion, actual current profile and an ordered list of `[dx,dy,wheel,button]` reports. Dwell pulse contains both press and release reports. Native input is semantic yaw/pitch/roll, not untransformed hardware axes.

Firmware USB uses newline commands at 115200 baud. Periodic and requested JSON include `protocol:1`, source HARDWARE and the same display fields. The bridge prefixes commands with `@requestId`; firmware echoes the ID so unsolicited telemetry (ID 0) cannot be mistaken for a command acknowledgement. Reports are empty because BLE controls the actual host pointer, which the browser observes through ordinary pointer/click events. Serial bridge maps native step to status and selection toggles to on/off. Native-only recording/corruption are refused by the hardware bridge. No browser virtual input is injected into real firmware through this bridge. Physical transport timing and USB reconnect remain hardware test items.

## Sensor recordings/replay

```csv
timestampMs,gyroX,gyroY,gyroZ,accelX,accelY,accelZ,valid
10,0,0,0,0,0,1,1
```

Header plus strictly increasing millisecond timestamps; gyro °/s and acceleration g; valid 0/1 (optional for imported seven-column files). NaN/Inf floats are parsed and rejected by sensor health, allowing fault replay. Bad rows/timestamps are refused. Native `record on/off` writes every sample to its runtime `samples.csv`; a new recording replaces the prior file. Firmware raw-register captures must be transformed to the desktop semantic convention or replayed through the qualified hardware `AxisTransform` in a custom runner.

## Profiles

84 bytes: little-endian magic uint32, schema uint32, generation uint32; 3 bias floats; 2 deadzones; 4 gains; alpha/precision/fast/dwellTolerance floats; dwellMs uint32; scrollThreshold/scrollGain floats; flags uint32 (bit0 scroll, bit1 dwell); CRC32 uint32 over preceding 80 bytes. Floats are IEEE754 binary32. CRC polynomial 0xedb88320 detects corruption. Every decode checks exact length, magic/schema, checksum, flags and numeric bounds. Newest valid generation wins; generation wrap is refused.

Save writes the older/invalid slot and verifies exact bytes. Memory adapter injects failed/torn saves; desktop adapter fsyncs a temp file, atomically renames and fsyncs the directory; NVS adapter writes a separate Preferences blob key. Interrupted physical NVS writes still need board/power-cycle testing. Exported JSON is readable evidence of parameters, not a firmware binary blob or automatic import format.

## Raw trials

`POST /api/trial` appends JSONL with session/block/trial, condition, inputSource, selection method, targetX/Y/width, startX/Y, endX/Y, distance, wall start/selection timestamps, high-resolution wall selection duration, nominal ID, hit, aborted/abort reason, device clocks, dwell cancellation delta, complete profile snapshot, viewport/DPR and software version. Server adds SHA256 of canonical profile JSON. UI export includes profileHash once disk logging succeeds and marks persistence failures.

`SIMULATED` means synthetic NodX control, `HOST_POINTER` means an ordinary host pointer (which can later be BLE). The input source alone does not prove hardware use. An aborted attempt is raw evidence but excluded from summary calculations. Report hit rate, misses, mean selection time of all completed attempts and successful ΣnominalID/ΣselectionTime. No effective-width throughput or unmeasured adaptation improvement is claimed.
