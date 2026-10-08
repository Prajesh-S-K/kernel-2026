# PROTOCOL · protocol 1, additive revision 2 (software 0.2.0)

## Browser bridge

Loopback only. `POST /api/device` accepts a JSON object with `action`: status, step, calibrate, cancel, resume, pause, generic, load, dwell, scroll, corrupt, record, settings. Dwell/scroll/record take boolean `enabled`. Step takes count (1–50; default 5), yaw/pitch/roll (°/s), pressed, connected, automatic, fault (0–6). Automatic means mathematical input during native calibration; normal manual inputs otherwise. Counts and fault selectors must be integers; boolean fields must be JSON booleans. Bounds, trailing native fields and nonfinite values are rejected. `settings` takes boolean `dwellEnabled` and `scrollEnabled` for temporary comparison overrides; `dwell`/`scroll` changes are persisted and failure returns `ok:false`. Cross-origin mutations are refused. Use one control tab/session.

Desktop process commands are one ASCII line, one JSON result line. `step count yaw pitch roll pressed connected automatic fault` executes fixed 10ms samples. Results include version/source, logical time, system/cursor/calibration/dwell state, reason, counters, filtered motion, actual current profile and an ordered list of `[dx,dy,wheel,button]` reports. Dwell pulse contains both press and release reports. Native input is semantic yaw/pitch/roll, not untransformed hardware axes.

Firmware USB uses newline commands at 115200 baud. Periodic and requested JSON include `protocol:1`, source HARDWARE or FIRMWARE_SIMULATED and the same display fields. The bridge prefixes commands with `@requestId`; firmware echoes the ID so unsolicited telemetry (ID 0) cannot be mistaken for a command acknowledgement. Reports are empty because BLE controls the actual host pointer, which the browser observes through ordinary pointer/click events. Serial bridge maps native step to status and selection toggles to on/off. Native-only recording/corruption are refused by the hardware bridge. No browser virtual input is injected into real firmware through this bridge. Physical transport timing and USB reconnect remain hardware test items.

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

## Revision 2 additions and delivery rules

Existing names/response fields and the 84-byte profile encoding remain compatible. `protocol` stays
1; `protocolRevision:2` and `faultCode` are additive. Desktop uses `softwareVersion:0.2.0`;
firmware retains its `firmware` version field. Fault codes: NONE, SENSOR_UNAVAILABLE,
SENSOR_TIMEOUT, TIMESTAMP, GYROSCOPE, ACCELERATION, GRAVITY, AXIS_MAPPING, LOOP_TIMING,
PROFILE, CALCULATION, TRANSPORT, STORAGE. Reasons remain readable display text.

Native and serial reply deadlines are two seconds; browser deadline is three seconds per request.
A failed native session is terminated and must restart. Serial retains partial JSON, rejects
malformed/nonfinite replies and accepts only the matching uint32 ID (1–4294967295).
Firmware ASCII lines are bounded to 80 bytes. Truncation rejects the whole command and preserves
any valid prefix ID for a negative acknowledgement; malformed IDs cannot be acknowledged as valid
requests. Bare commands remain usable in a serial monitor. Periodic ID-0 snapshots are coalesced
at 5 Hz. Two acknowledgement frames queue ahead of the next snapshot; each transmission writes
at most 64 currently available bytes, finishing a line before another starts.

Every raw trial carries `deviceSource`, `selectionMethod` and immutable `blockContext` containing
source, input source, condition, complete profile, arena width/height/DPR and software version.
The server rejects mismatched context fields. Exports wait for pending persistence and explicitly
mark failures. Summaries show blocks separately and split incompatible settings even if an ID is
reused. Profile/source/geometry changes invalidate a block. Generic/adaptive comparisons apply
identical temporary selection toggles; saved profiles remain unchanged by these overrides.
HOST_CLICK trials on the desktop measure ordinary host pointing, not NodX adaptation.

## Revision 3 additions: hands-free (additive)

Hardware firmware (not the simulators) adds a `sensor` object to every telemetry frame for bench sessions: `variant`, `seen`, `frames`, `ageMs`, the last raw `gyro` (deg/s) and `accel` (g) in SENSOR coordinates before any axis mapping, and `angle`, the running integral of each raw gyro axis since boot (deg). Additive; the frame stays well inside the 2048-byte buffer (measured 1581 bytes idle).

`protocol` stays 1; `protocolRevision` is 4 (3 added the hands-free commands; 4 adds the enable-input kind and the raw-vs-latched fields; both additive). Every existing command, field and the 84-byte profile
encoding are unchanged; old clients ignore the new object.

**Commands** (native line protocol / serial; the bridge uses `action` objects):

| Command | Bridge action | Notes |
|---|---|---|
| `enable 0\|1` | `{"action":"enable","enabled":bool}` | Simulated RAW enable input: button pressed / switch ON (simulators only; refused by hardware firmware). `enabled` is required. Starts released. A disabling edge releases at once |
| `handsfree demo on\|off` | `{"action":"handsfree","op":"demo","enabled":bool}` | Temporary movement-only demo (RAM only, off at every boot, never saved): no dwell click, no drag (the drag gesture is refused and counted), no wheel, pointer steps bounded to 6 px per report; the enable input and every safety check are unchanged. Changing it while control is active pauses control. Reported as `handsFree.demoMovementOnly` |
| `handsfree uncal start\|stop` | `{"action":"handsfree","op":"uncal","enabled":bool}` | Temporary UNCALIBRATED pointer demo (see `docs/BENCH_PLAN.md`): RAM-only validated demo profile, movement only, never saved, never started by a connection or reboot. `start` is refused (ok:false, reason set) unless the sensor is healthy, the axis mapping valid, BLE connected and no calibration/training/fault/active control exists; the explicit website start authorises it (`permission` WEBSITE_START in the status); the physical enable button is NOT required for this temporary fallback, and any press of it stops the demo at the press edge and revokes the permission. Any stop (Stop demo, a button press, calibration, pause, fault, disconnect, delivery failure) needs an explicit new start. Reported as `handsFree.uncalDemo` `{active, blocked, profileState, gain, deadzone, maxStep}`; `profileState` is MISSING, VALID or CORRUPT (a corrupt record is never repaired) |
| `handsfree uncal reverse <h 0\|1> <v 0\|1>` | `{"action":"handsfree","op":"uncalreverse","horizontal":bool,"vertical":bool}` | Flip the fallback's horizontal and/or vertical pointer direction. RAM only; reported as `uncalDemo.reverseX/reverseY` |
| `map start\|cancel\|accept\|save\|clear` | `{"action":"map","op":"start"}` etc. | Guided mapping. `start` needs a healthy sensor and BLE and no active control; teaching never moves the pointer and is never started by a boot, reconnect or fault. `accept` (only in the preview) makes the learned mapping active in RAM and does NOT write storage. `save` is explicit, transactional (two slots, version, checksum) and refused while control is active; a failed save keeps the RAM mapping and the previous stored one (`saveResult` SAVE_FAILED_RAM_ONLY). `clear` forgets the RAM mapping only |
| `click train fallback\|configured` | `{"action":"click","op":"train","frame":"fallback"}` | Teach the optional gesture click (RAM only). `fallback` uses the default axis mapping; `configured` uses the learned frame and needs a learned mapping. Refused while control is active, a fault is present or the sensor is not healthy |
| `click cancel\|accept\|clear` | `{"action":"click","op":"accept"}` | `accept` only after the ordinary-pointing check (phase READY); `clear` forgets the gesture. A gesture is bound to the frame it was taught in; relearning the mapping invalidates a configured-frame gesture |
| `click enable on\|off` | `{"action":"click","op":"enable","enabled":bool}` | Enable the gesture click while a control session runs (refused otherwise, or for a gesture of the other frame). Off at every start, cleared by every stop or fault |
| `control start\|stop` | `{"action":"control","op":"start"}` | Configured control with the learned mapping: healthy sensor, valid mapping, BLE and the physical enable-button permission (unchanged product-mode behaviour; never automatic), plus a valid learned mapping and no gross re-orientation (gravity direction within 75 degrees of the taught one). Movement only. `stop` is always accepted |
| `handsfree uncal dwell on\|off` | `{"action":"handsfree","op":"uncaldwell","enabled":bool}` | Explicitly enable dwell clicking inside the RUNNING uncalibrated demo (refused when the demo is not running). Off at every start; cleared by every stop. One primary-button click per completed dwell; no drag, double-click, right-click or scrolling |
| `handsfree uncal dwell set <ms> <tolerance>` | `{"action":"handsfree","op":"uncaldwellset","ms":int,"tolerance":number}` | Temporary dwell settings, RAM only, validated (500-5000 ms, tolerance 2-50). A running dwell restarts. Reported in `handsFree.uncalDemo.dwell` `{enabled, ms, tolerance, state, progress, clicks}`; `dwellProgress` (top level) follows the profile in use |
| `handsfree enable maintained\|momentary` | `{"action":"handsfree","op":"enable","kind":"momentary"}` | Stages the enable-input kind (default for a new setup: `momentary`); stored only by `handsfree commit`. `handsFree.switch` reports `kind`, `kindStaged`, `pressed` (raw), `latched` (button permission), `armed`, `on`, `permitted` |
| `step … fault [enable]` | `step` with optional `"enabled":bool` | Optional ninth integer 0/1 (raw input); omitted keeps the current simulated input state |
| `gesture <nod\|turn\|tilt><1-3> [scale]` | `{"action":"gesture","name":"nod2","scale":1}` | Scripted synthetic motion (simulators only); scale 0.25–3 |
| `train start pause\|drag` | `{"action":"train","op":"start","gesture":"pause"}` | Stops and releases output; needs a valid profile |
| `train cancel` / `train accept` | `{"action":"train","op":"cancel\|accept"}` | Cancel keeps all stored/staged data; accept needs a validated pattern, stages only |
| `handsfree commit` | `{"action":"handsfree","op":"commit"}` | Explicit conversion + save; `ok:false` with `faultCode:"STORAGE"` on a failed write |
| `handsfree legacy` | `{"action":"handsfree","op":"legacy"}` | Saves the explicit compatibility choice; also repairs CONFIG_INVALID |
| `handsfree switchless on\|off` | `{"action":"handsfree","op":"switchless","enabled":bool}` | Stages the alternative-to-switch qualification for the next commit |

Malformed IDs, booleans, counts, names and nonfinite or out-of-range numbers are rejected by both
the bridge (`ValueError` → HTTP 400) and the native/firmware parsers (`ok:false`); firmware lines
stay bounded to 80 bytes.

**Telemetry** `handsFree` object: `mode` (`LEGACY_SWITCH|HANDS_FREE|CONFIG_INVALID`), `config`
(`MISSING|VALID|CORRUPT|UNSUPPORTED|OUT_OF_BOUNDS`), `configId` (8 hex digits of the CRC of the
configuration *content*, `00000000` if none), `switch{present,on,permitted,switchless,switchlessStaged}`,
`gesture{state,last,lastReject,candidates,rejected,executed,refused,suppressing}`, `drag`,
`training{phase,gesture,accepted,required,rejects,validated,reason}`, `staged[2]`, `stored[2]`,
`blocked` (why a resume would be refused, empty if allowed). `training.validated` means only that the helper's validation repeat was recognised by the software; it is **not** hardware or user validation, and every learned value stays a START value. Strings are static, quote-free text and
all numbers are integers, so the JSON stays valid and finite in every state. System state `TRAINING`
and cursor state `GESTURE` are new.

**Configuration record** (`hf0.bin`/`hf1.bin`, NVS keys `hf0`/`hf1`): 136 bytes, little-endian words:
magic `0x4846444e`, version 1, generation, flags (bit0 enabled, bit1 switchlessQualified), neutralRate
(float), two templates of 14 words each (stroke count, six stroke slots `axis | 0x100 if sign +`,
enterRate, peakMin, peakMax, strokeMin/Max, gapMax, totalMax), CRC32 of the preceding bytes. The
two-slot rule matches profiles: newest valid generation wins, the older/invalid slot is rewritten and
read back. Records that fail length, magic or CRC are CORRUPT; valid CRC with another version is
UNSUPPORTED; non-finite, out-of-range, unused-slot or indistinct content is OUT_OF_BOUNDS. Only an
empty pair of slots is MISSING (= legacy).

**Raw trials** gain `interactionMode`, `gestureConfigId` and `gestureInterruptions` (recognition
candidates opened during the attempt); the block context freezes the first two and the server rejects a
trial that differs from its context. Aborted rows keep `abortReason` (for example
`gesture configuration changed`, `drag active`, `gesture command executed`).

**Replay CSV** may carry a ninth `enable` column (0/1, default 1). Native recordings now write it.

Hardware status frames also carry `axes` `{valid, gyro, gyroSigns, accel, accelSigns}` (the active axis mapping: mapped yaw, pitch, roll take sensor gyro axis indices 0=X, 1=Y, 2=Z). The status frame buffer is 3072 bytes; a frame that does not fit is dropped, so the firmware tests keep it below 2300.

Status frames also carry `mapping` `{phase, cue, step, steps, direction, validation, example, perDirection,
retries, interruptions, cueMs, stillMs, windowMs, progress, reason, preview{x,y,angleX,angleY}, bias, noise,
learnedValid, unsaved, stored, mode, blocked, saveResult}`. `mode` is OFF, UNCALIBRATED_DEMO or CONFIGURED;
`stored` is MISSING, VALID or CORRUPT (a corrupt record is never used or overwritten); `unsaved` is true while
the active mapping exists only in RAM; `blocked` is why configured control cannot start. New system state
`TEACHING`.

Learned-settings record (separate from the 84-byte profile and the hands-free record; NVS namespace
`nodx-ctl`, keys `ctl0`/`ctl1`; files `ctl0.bin`/`ctl1.bin` in the simulator): 120 bytes, little-endian, magic
`CNTL`, schema 1, generation, horizontal row (3 floats), vertical row, gyro bias, noise sigma, gravity, gain
L/R/U/D, One Euro min cutoff / beta / derivative cutoff, deadzone enter x2, deadzone exit x2, CRC-32.
Decoding rejects a wrong size, magic, schema or checksum, non-finite values, rows that are not unit and
orthogonal, and out-of-range parameters.

Status frames carry `click` `{phase, cue, step, steps, validation, retries, cueMs, progress, reason, activityMs,
ready, enabled, frame, state, suppressing, accepted, rejected, candidates, clicks, lastReject, blocked}`.
`state` is OFF, ARMED, CANDIDATE (pointer output paused, movement discarded, never replayed) or NEUTRAL_WAIT.

`handsFree.uncalDemo.permission` is NONE, WEBSITE_START (the temporary fallback) or ENABLE_BUTTON (configured
control). `mapping` also reports `mountingDeg` (angle between the gravity direction now and while teaching)
and `mountingWarning` (above 35 degrees).
