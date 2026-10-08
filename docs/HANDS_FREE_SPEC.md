# HANDS_FREE_SPEC · implementation specification (hands-free revision, unreleased)

Status: written before implementation and kept as the authority for the code, tests and companion.
All numbers are **START values, unvalidated** until measured on hardware. Nothing here claims an
accidental-trigger rate, comfort or suitability for any user.

## 1. Scope and invariants

Everyday operation uses no physical button. Hardware has at most **one maintained control-enable
switch**. A helper may pair, calibrate and train. The frozen pipeline is unchanged:
`… → InteractionEngine/SelectionManager → SafetyManager → HIDManager → HIDTransport`.
Unchanged: 84-byte `UserProfile` and CRC, two-slot repository, existing command names and fields
(extensions are additive), sensor abstraction, bounded output, disabled GPIO defaults, buzzer
disconnected (`NODX_BUZZER=-1`), no battery/Wi-Fi/cloud/ML/framework.

Priority, highest first: **inhibit/stop** (enable OFF, fault, disconnect, pause, calibration,
training, profile/config change) → **drag release** → **recognition suppression** → **dwell click**
→ **pointer/scroll**. A lower item can never override a higher one.

## 2. Interaction modes and the hands-free configuration record

| Mode | Meaning |
|---|---|
| `LEGACY_SWITCH` | Compatibility mode and test fixture: physical selection/pause/calibration inputs and the existing dwell opt-in behave exactly as v0.2.0. No enable switch gating. |
| `HANDS_FREE` | Dwell selection, enable-switch gating, trained gestures for pause/resume and drag. Physical selection, pause and calibration inputs are ignored. |
| `CONFIG_INVALID` | A hands-free record exists but cannot be used. Output is inhibited; no mode is silently assumed. Helper recovery: retrain/commit, or `handsfree legacy`. |

Mode is derived only from the separate record `HandsFreeConfig` (never from the profile):

| Stored record state | Result |
|---|---|
| Both slots empty (**MISSING**) | `LEGACY_SWITCH` (nothing was ever converted) |
| Valid newest record, `enabled=1` | `HANDS_FREE` |
| Valid newest record, `enabled=0` | `LEGACY_SWITCH` (explicit helper choice) |
| Non-empty, bad length/magic/CRC (**CORRUPT**), valid CRC but version≠1 (**UNSUPPORTED**), valid CRC but out-of-bounds/indistinct gestures (**OUT_OF_BOUNDS**) | `CONFIG_INVALID` |

The two-slot repository returns the newest valid record. If neither slot is valid and at least one is
non-empty the worst failure is reported. A save never overwrites the newest valid slot with a
failed/torn write (the older/invalid slot is written, then read back and compared), mirroring
`ProfileRepository`.

Record `HandsFreeConfig` v1, little-endian, fixed length, CRC32 (same polynomial as profiles):
magic `0x4846444e`, version, generation, flags (bit0 enabled, bit1 switchlessQualified),
`neutralRate`, two gesture templates, CRC. Hands-free requires **profile.dwellEnabled** at
activation; a profile reload cannot leave a hands-free session without dwell (`resume` refuses and
reports why).

A new hands-free profile is created by explicit conversion only. Existing profiles are never changed
by boot, load or status. Calibration performed while hands-free is active forces `dwellEnabled`.

## 3. Maintained control-enable switch

* Semantics: ON *permits* control; OFF *inhibits*. It never cuts power and never resumes anything.
* `EnableGate` debounce is asymmetric: **OFF is accepted at the first OFF reading** (no wait);
  ON is accepted only after 30 ms continuously ON. Unknown input at boot is OFF (fail closed).
* The adapter feeds the gate **every loop iteration** (not only on fresh sensor samples) through
  `System::setControlSwitch(on, now)`. On a permitted→inhibited edge in hands-free mode the system
  emits a released stationary report through SafetyManager→HIDManager **at once** (no new sample),
  drops drag, resets recognition/dwell and moves ACTIVE→PAUSED. ON afterwards leaves PAUSED/READY.
* Unconfigured switch (`present=false`): in hands-free mode control is **inhibited** unless setup
  explicitly stored `switchlessQualified=1`. Firmware: `NODX_ENABLE` default −1 ⇒ not present.
  Proposed bench pin GPIO4 is a START value only. The desktop simulator has a simulated switch
  (default ON); source labels remain SIMULATED / FIRMWARE_SIMULATED / HARDWARE.
* Boot, reconnect, fault recovery and switch-ON all end in READY/PAUSED, never ACTIVE.

## 4. Gesture patterns

A **pattern** is an ordered list of 2–6 *strokes*. A stroke is one fast, single-axis excursion of the
smoothed, bias-corrected angular rate: axis ∈ {yaw, pitch, roll}, sign ±. Examples a helper may
choose (not universal): double nod = pitch +,−,+,− ; double sideways tilt = roll +,−,+,− ;
double turn = yaw +,−,+,−. The helper trains whichever comfortable, distinct pattern suits the user;
the recogniser stores only generic stroke parameters.

Two command gestures exist: `PAUSE_RESUME` (id 0) and `DRAG` (id 1).

**Distinctness** (config invalid otherwise): sequences must differ, and neither may be a prefix of the
other (a prefix would fire early inside the longer pattern).

Units: gyro °/s (semantic yaw/pitch/roll after `AxisTransform`, minus `profile.bias`); time ms from
sample timestamps (unsigned subtraction, rollover safe); recognition filter: fixed EMA
`s += 0.5·(g − s)`. Sample intervals may be irregular (upstream already faults >100 ms).

Template parameters and START bounds (decode/validation rejects anything outside):

| Field | Meaning | Bounds |
|---|---|---|
| `neutralRate` (config-wide) | all axes below this = neutral | 1–10 °/s |
| `strokes`, `axis[i]`, `sign[i]` | sequence | 2–6; axis 0–2; ±1 |
| `enterRate` | stroke start threshold; exit = 0.5·enter | 8–100 °/s |
| `peakMin`, `peakMax` | accepted stroke peak | `enterRate`≤min≤150; min<max≤240 |
| `strokeMinMs`, `strokeMaxMs` | accepted stroke duration | 20–300; min+20–1000 |
| `gapMaxMs` | max time between a stroke end and the next start | 20–1000 |
| `totalMaxMs` | first start to last end | 200–4000 |

## 5. Training (helper only)

Needs a valid calibrated profile (bias). Commands: `train start pause|drag`, `train cancel`,
`train accept`. `train start` **stops and releases output immediately** and enters state `TRAINING`.
Phases (telemetry `training.phase`): `REST` → `EXAMPLE` (×4) → `ANALYZE` → `VALIDATE` → `READY` /
`FAILED`.

1. **REST** 1000 ms and ≥80 samples: per-axis σ and mean. Reject if σ>4 °/s or |mean|>10 °/s
   (not at rest / too noisy). `neutralRate = clamp(3·maxσ + 1.5, 2, 8)`.
2. **EXAMPLE** (4 accepted examples required, at most 8 rejected attempts): wait ≤6 s for motion
   (dominant axis ≥ `max(3·neutralRate, 15)`), capture until neutral for 300 ms or 3 s cap. Segment
   strokes with the §6 segmentation. An example is accepted only if: ≥2 and ≤6 strokes; every
   stroke peak ≥ 20 °/s; every stroke single-axis (cross-axis peak ≤ 0.6×peak); finite data.
   Otherwise the example is rejected with a reason and may be retried. Invalid/nonfinite/stale
   samples fail training at once.
3. **ANALYZE**: every accepted example must share the same (axis,sign) sequence (each example is
   compared with the first as it is captured, so a different pattern is rejected at once and
   retried; analysis checks again). Learn
   `peakMin = 0.5·min peak`, `peakMax = min(240, 1.5·max peak)`, `enterRate = clamp(0.5·peakMin, 8,
   100)`, stroke min/max = 0.5×min / 2×max observed duration (clamped), `gapMaxMs = 2×max gap`
   (clamped), `totalMaxMs = 1.5×max total` (clamped). The result must pass template validation and be
   distinct from the other gesture (stored or staged) and enough motion must remain
   (`peakMin ≥ 20`).
4. **VALIDATE**: recognition runs live with only the candidate; the helper performs it once more. It
   must be recognised once (nothing is executed) before `READY`. 20 s timeout → `FAILED`.
5. `train accept` copies the candidate into the *staged* slot; nothing is persisted yet.
   `train cancel` at any time discards the candidate and keeps every stored/staged template.

Training works with the enable switch OFF (no output is possible in `TRAINING`). Sensor, timing or
transport fault during training cancels it (prior configuration preserved) via the Safe State path.

## 6. Recognition

Runs on healthy samples in READY, PAUSED and ACTIVE (so resume works while logically paused) **only**
when mode is HANDS_FREE and the gate permits. Otherwise it is reset to `WAIT_NEUTRAL`
(CALIBRATING, TRAINING, SAFE_STATE, CALIBRATION_REQUIRED, switch OFF, CONFIG_INVALID, LEGACY).

States: `WAIT_NEUTRAL` → (neutral for 300 ms) → `ARMED` → (a stroke starts) → `CANDIDATE`
(in-stroke / in-gap) → executes or rejects → `WAIT_NEUTRAL`. Reset (resume, fault, calibration,
profile or config change, switch edge) always returns to `WAIT_NEUTRAL`: **neutral is required
before rearming** after every completion, rejection and reset.

* **Start**: in ARMED, a stroke starts when the dominant axis rate has the sign and axis of a
  template's first stroke and reaches that template's `enterRate`. The set of still-viable templates
  is tracked.
* **Stroke end**: rate on the stroke axis falls below `0.5·enterRate`. Accepted only if duration
  within [`strokeMinMs`,`strokeMaxMs`], peak within [`peakMin`,`peakMax`], and cross-axis peak ≤
  0.6×peak.
* **Next stroke**: must start within `gapMaxMs` and match the next (axis,sign); motion on any other
  axis/sign above `enterRate` = wrong order = reject.
* **Complete**: last stroke accepted within `totalMaxMs` ⇒ exactly one gesture event, then
  `WAIT_NEUTRAL`. If two templates would complete on the same sample, **no** event (ambiguous).
* **Reject reasons** (counted, shown): WRONG_ORDER, TOO_SLOW, TOO_FAST, TOO_WEAK, TOO_STRONG,
  NOT_SINGLE_AXIS, GAP_TIMEOUT, TOTAL_TIMEOUT, AMBIGUOUS.
* **Held posture / repetition**: a steady posture has near-zero rate so it creates no stroke; a
  continuous shake is one candidate that completes or is rejected, then neutral must be held again.

### Distinguishing normal movement from command candidates

A candidate requires **all** of: ≥300 ms of neutral beforehand; a stroke that is fast (≤`strokeMaxMs`)
and peaks above `enterRate`; cleanly single-axis; an alternating repeat of the learned sequence with
bounded gaps and total time; then neutral again. Slow or sustained pointing (a stroke longer than
`strokeMaxMs`), diagonal motion, scroll tilts held at an angle and drift reject the candidate.
The corpus tests show *no execution on the listed synthetic examples*; they are not an
accidental-trigger rate. Real rates need physical measurement.

**Cost**: while a candidate is open (first stroke start until completion/rejection) pointer, scroll
and dwell are suppressed. A rejected candidate therefore costs up to roughly one stroke-duration of
lost pointing at the start of a fast move; the suppressed movement is **discarded, never replayed**.
Pointer control resumes immediately after rejection, but recognition does not re-arm until neutral.

## 7. Arbitration and actions

Per tick, in order: (1) fault/health checks (typed faults → Safe State); (2) gate permit check;
(3) recognition update; (4) action execution; (5) pointer/scroll/dwell composition with
suppression; (6) SafetyManager; (7) HIDManager.

* `PAUSE_RESUME`: ACTIVE → `pause()` (releases drag). READY/PAUSED → `resume()`, which succeeds only
  if every condition holds: valid profile, valid hands-free configuration, dwell enabled, gate
  permits, ≥20 consecutive healthy samples since the last fault, transport connected, stationary
  delivery succeeds. Failure is counted and displayed; nothing else happens. SAFE_STATE/CALIBRATING/
  TRAINING: not recognised.
* `DRAG`: only when ACTIVE. Toggles `dragging`: press on start, release on end. During drag dwell
  clicks are suppressed. A drag release (gesture) and any resume start a **dwell lockout**: a
  meaningful movement is required before the next dwell click (no click merely because the user
  paused after a gesture).
* Drag is released at once, without waiting for a sample, by: pause, enable OFF, calibration,
  training, profile/config change, sensor/timing/profile/calculation fault, transport failure and
  disconnect. After recovery nothing is re-pressed and the system is READY, not ACTIVE.
* Recognition beginning during a drag: the button stays held, pointer/scroll are suppressed (zero
  movement reports with `down=true`) until the candidate resolves; PAUSE_RESUME releases the drag,
  DRAG releases it, rejection continues the drag. Dwell stays suppressed throughout.
* Cancelled recognition cancels pending dwell (counted); the dwell timer restarts from idle after
  resolution, so no delayed click occurs from the discarded motion.

## 8. Configuration persistence and transactional setup

Separate adapter interface `ConfigStorage` (two slots, bytes ≤256): memory (tests), POSIX files
(`hf0.bin`/`hf1.bin`, temp+fsync+rename+directory fsync) and NVS keys `hf0`/`hf1`.

`handsfree commit` (explicit helper action; both templates must be stored or staged, valid, distinct;
profile must be valid):

1. `stop()` — release output immediately.
2. If the profile has `dwellEnabled=false`: save a converted profile copy with dwell on (existing
   84-byte format). Failure ⇒ abort, nothing else written.
3. Save the config record (`enabled=1`) — **this is the single commit point**. Failure ⇒ best-effort
   restore of the previous profile (only if step 2 changed it), report failure, keep the prior valid
   config (the two-slot write never replaced it) and the in-memory prior profile.
4. Only after both succeeded: adopt the new mode in memory. Result READY, never ACTIVE.

An interruption (power loss) before step 3 leaves no hands-free record or the old valid one ⇒ mode
unchanged (legacy, possibly with the legacy-valid dwell option turned on). After step 3 the setup is
complete. A hands-free setup is therefore never partially enabled. If an adapter without atomic
writes leaves a **torn first record** and no older valid record exists, the next boot classifies it
CORRUPT ⇒ `CONFIG_INVALID` (inhibited, fail closed); the helper repairs it with a new commit or
`handsfree legacy`. A torn write never replaces an older valid record (it goes to the other slot). Failures use FaultCode `STORAGE`
and `ok:false` acknowledgements; telemetry never reports success for a failed save.

`handsfree legacy` saves `enabled=0` (explicit return to compatibility mode; also repairs
CONFIG_INVALID). `handsfree switchless on|off` stages the alternative-qualification flag used by the
next commit. Existing `dwell`, `scroll`, `settings`, `generic`, `load` commands keep working;
turning dwell **off** persistently is refused in hands-free mode (reported `ok:false`).

## 9. Protocol additions (additive; `protocol:1` unchanged, `protocolRevision:3`)

New commands: `enable 0|1` (simulators only), `gesture <name> [scale]` (simulators only),
`train start pause|drag | cancel | accept`, `handsfree commit | legacy | switchless on|off`;
`step` accepts an optional ninth integer `enable`. New telemetry object `handsFree` (mode, config
state, `configId`, switch, gesture counters, drag, training, staged/stored flags, blocked reason).
All numbers in telemetry are sanitised so the JSON stays valid and finite in every state.

## 10. Companion and Lab

Helper-guided setup (calibrate → train pause/resume → train drag → qualify switch → commit →
resume), live status (READY, ACTIVE, PAUSED, SAFE_STATE, drag, switch), retry/cancel/error states
and recovery guidance. Helper controls for calibration, pause and recovery remain. Legacy
switch-hold control is labelled compatibility-only and disabled in hands-free mode. Lab blocks freeze
`interactionMode` and `gestureConfigId` with the profile/source/geometry; a change aborts the block;
legacy and hands-free blocks are never pooled; raw rows keep gesture interruption counts and abort
reasons. A drag toggle or any executed gesture during a block aborts it.

## 11. Verification boundary

Software tests prove deterministic logic on synthetic and replayed samples. They do **not** prove
accidental-trigger rates, comfort, training burden or suitability. Hardware-required: sensor
initialisation/data-ready, exact N16R8 memory and USB, BLE behaviour (including disconnect during
drag), electrical enable-switch wiring and bounce, gesture comfort and accidental activation with
real users, and all values above.
