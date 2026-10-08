# EVIDENCE · v0.2.0 pre-hardware release

Verified 8 October 2026 on this macOS host. Source version 0.2.0; local release tag `v0.2.0-prehardware`. Prior tag `v0.1.0-prehardware` is preserved. This document distinguishes software checks from physical qualification.

| Item | Evidence / status |
|---|---|
| Shared C++ engine | Built with AppleClang 17, C++17; core warnings treated as errors |
| Core invariants | **42 passed, 0 failed**; [named results](../evidence/control-tests.log); includes 100,000 simulated ticks, fault and persistence checks |
| Desktop/serial protocol | **19 Python checks passed**; full save/restart/replay integration plus simulated serial acknowledgement correlation and virtual-input isolation |
| Lab metric/export tests | **13 JavaScript checks passed**; empty/miss/abort cases, summed nominal rate, nested-profile CSV |
| Combined software run | **74 named checks passed** (all original 51 retained); [software log](../evidence/software-checks.log); undefined-behavior sanitizer enabled for C++ |
| Address sanitizer | Runtime stalled on this macOS host; stopped; **not counted as passed**. Optional ASAN configuration retained |
| ESP32-S3 cross-build | **Both real-sensor and simulated-sensor environments built successfully** with pinned PlatformIO/NimBLE; [build log](../evidence/firmware-build.log) |
| Browser setup | Actual native calibration generated/saved a valid synthetic asymmetric profile (left≈50, right≈25, up≈60, down≈30 px/degree); no real-person inference |
| Browser control | Observed ACTIVE → injected disconnect → SAFE_STATE → healthy recovery READY; explicit resume; dwell ARMING and LOCKOUT observed; pause exercised; switch-driven drag moved a practice target 3px and logged selection/release |
| Browser lab | 12-target automated host-pointer block completed; a separate deliberate miss/user abort and a viewport-change abort retained raw; [automated rows](../evidence/automated-browser-trials.jsonl) are **UI smoke data, never participant performance** |
| Visual checks | Desktop 1280×900 and mobile 390×844 at 130% scale inspected, with reduced motion/glow controls; generated profile, motion trace, state labels and responsive controls; final [setup screenshot](../evidence/setup-v02.png) |
| Formatting/lint | Pinned clang-format, Ruff, Prettier and ESLint checks pass; runtime remains framework-free |
| Release integrity | Regression rejects dirty/stale source and modified build artifacts; full gate stamps source plus native/firmware hashes |
| Transport | Two-second native/serial and three-second browser deadlines tested; partial replies, malformed/nonfinite JSON, dead processes and missing acknowledgements checked |
| Versioning | Independent local repository; no remote publication; build/runtime outputs ignored |

## Hardware-required status

Physical upload/boot, exact GPIO assignment, MPU6050 axis/gravity signs and rate/noise, NVS power-loss transactions, BLE pairing/encryption/subscription/report ordering, switch electrical debounce, buzzer circuit, USB bridge transport, host disconnect-release behavior, watchdog/brownout recovery, latency, user comfort and performance all remain **UNVALIDATED**. No battery/Wi-Fi/cloud feature was added.

The firmware image is compile-ready with disabled GPIOs; physical integration still requires assigning qualified pins and measuring the actual mount/parameters. No percentage-complete hardware claim is made. A standard HID host uses its own cursor outside the companion. The lab displays observed nominal target metrics, not certified Fitts throughput or adaptation benefit.

## Primary implementation references

MPU6050 register settings and conversion constants follow the manufacturer's [MPU-6000/MPU-6050 register map](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf). The BLE adapter was checked against the pinned [NimBLE-Arduino 2.3.6 server example](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/examples/NimBLE_Server/NimBLE_Server.ino), [server API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLEServer.h) and [characteristic API](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLECharacteristic.h). These references support implementation choices, not physical validation.

Reproduce: `sh scripts/check.sh`, then `pio run -e esp32s3 -e esp32s3-sim`. Build outputs and verification logs are separate from the source. Firmware hashes are recorded in the delivered artifact manifest. The GitHub workflow is prepared but has not run remotely.

## Browser verification scope

The v0.2.0 walkthrough observed native calibration/save, pointing, switch-driven drag/release,
scroll output, dwell arming/lockout, pause, HID-failure Safe State, qualified recovery, separate Lab
blocks, matched temporary selection settings and viewport abort. Desktop/mobile screenshots are
stored in `evidence/*-v02.png`. Mobile setup measured content width equal to the 390px viewport.
The final browser error log was empty. CSV generation, frozen context and persistence-failure export
are covered by JS tests; the browser export button was invoked without a console error. The in-app
browser's download-event observer timed out, so filesystem retrieval of that browser download is
not counted as verified. Raw JSONL and a reproducible smoke CSV are included instead.

Firmware UART/USB saturation, actual acknowledgements, electrical stopping and BLE notification
behavior remain physical tests. Cross-compilation does not establish real-time hardware behavior.

# Hands-free revision (unreleased) — evidence

Verified on the committed branch `claude/hands-free-revision` (on top of `8454580`) in a clean clone under a neutral path.
The logs below were produced from the commit `f4be75f`; the only later commit adds those logs. This revision is **software and simulation only**.
Nothing below establishes hardware behaviour, accidental-trigger rates, comfort, training burden or suitability.

## Checks

| Group | Baseline (unchanged, still passing) | Added by this revision | Now |
|---|---:|---:|---:|
| C++ named checks (`nodx_tests` / `nodx_handsfree_tests`) | 42 | 137 | 179 |
| Python (`unittest`) | 19 | 29 | 48 |
| JavaScript (`node --test`) | 13 | 20 | 33 |
| **Subtotal** | **74** | **186** | **260** |
| Firmware command path, host-compiled (`nodx_firmware_sim_tests` / `nodx_firmware_hw_tests`) | 0 | 170 | 170 |
| **Total named checks** | **74** | **356** | **430** |

Each named check counts once; a test executable is not counted as one check. Logs:
[software gate](../evidence/handsfree-software-checks.log) (formatting, lint, UBSAN native build, Python,
JavaScript), [named C++ results](../evidence/handsfree-tests.log), [firmware builds](../evidence/handsfree-firmware-build.log).
`ctest` reports 4 test programs; the 179 named C++ checks are inside the first two. The firmware command-path
program is built twice from the same test file (simulated and hardware configuration): 157 + 56 check
*executions*; counting the 43 checks that run identically in both configurations (42 parser checks and the
boot-banner check) once gives the 170 in the table. Some check names repeat (a helper used at several call sites), so
the table counts executed checks, not unique strings. Its log: [firmware command tests](../evidence/firmware-command-tests.log).

Firmware: `esp32s3` (775,465 B flash), `esp32s3-sim` (782,773 B) and `esp32s3-n16r8-bench` (779,145 B) all built;
GPIO defaults remain disabled and `NODX_ENABLE`/`NODX_BUZZER` default to -1. Compilation is not hardware
qualification, and the DIO/QIO image-header question from the N16R8 note remains open.

## Enable push button (momentary, four-pin tactile)

The control-enable input is now, by default, one momentary push button (GPIO4 and GND on different contact
pairs, START assignment, no 3V3/5V) whose debounced presses toggle a permission latch; the maintained switch
remains as an explicit stored option. Rules: [HANDS_FREE_SPEC §3](HANDS_FREE_SPEC.md). Everything below is
software and simulation; no real button, contact pair, bounce or wiring was measured.

* **Core gate tests** (1 ms resolution, `tests/test_handsfree.cpp` section K): boot released, boot held for 5 s
  (never enables; a 20 ms release is not a stable release), press and release bounce (24 ms of 2 ms chatter on
  the press, the release and the disabling press: exactly one toggle each), a 30 s hold (one toggle) and 10
  repeated presses (one toggle per press, permission alternates), a second press needing a stable release, a
  one-sample glitch only ever disabling, clearing the latch (fault) needing a release and a new press.
* **System tests**: pressed and latched reported separately (and in the telemetry JSON); reboot with the button
  released and held starts disabled and never resumes; enabling never resumes and the resume gesture is a
  separate step; the disabling press stops pointer movement, scrolling, a dwell in progress and a drag *at the
  press edge with no further sample*; a failed neutral delivery is a fault, the latch is cleared and no number of
  presses recovers it; a sensor fault is not bypassed by presses while the sensor is still bad; an invalid stored
  configuration and an unwired input stay inhibited whatever the button does; a saved maintained-switch setup
  keeps its kind across reboot and retraining; a record written before the button existed decodes as
  maintained and re-encodes byte-identically (golden record from the previous revision's encoder); changing
  the kind at commit starts disabled again.
* **Firmware host harness** (real `runtime/commands/telemetry.cpp`): the new setup starts as the button kind,
  disabled; a gesture and the helper resume are refused while disabled; one press latches without resuming;
  raw pressed and latched permission differ after release; the disabling press pauses, releases a drag and
  clears the latch in its own acknowledgement; holding after that does not toggle again; reboot with the
  button held keeps the saved setup but not the permission; release alone enables nothing, a new press does;
  a fault drops the latch and presses during it do not bypass it; the maintained kind can be staged, committed,
  works without a latch and survives a reboot; malformed `handsfree enable` commands are refused.
* **Python protocol tests** (8) and **companion logic tests** (6) cover the same flows through the native line
  protocol and the Lab/setup view model.
* **Mutation checks** (run, then reverted): enabling while the button is held at boot fails 8 or more checks (the
  run listed the first 8), repeated toggling while held 6 or more, disabling only after the debounce fails 6, not clearing the latch on a fault
  fails 1 core and 1 firmware-harness check, converting a maintained setup to the button on retraining fails 1,
  a gate that starts enabled 6 or more; in the firmware harness not feeding the raw input every pass fails the
  whole button flow, accepting a bogus kind fails 3.
* **Browser** (companion, manual): the Studio shows *Hold to press the enable button*, chips read
  `Control DISABLED · press enable button` / `Button released`; a held press shows `Button PRESSED` and
  `Control PERMITTED · enable button latched`; after release the permission stays; the resume gesture then
  gives ACTIVE; the second press gives PAUSED immediately; a gesture while disabled does nothing. The setup view
  offers the input kind (button default, maintained switch compatibility). No horizontal overflow at 375 px.
  [Screenshot](../evidence/handsfree-enable-button-studio.jpg).
* **Wokwi**: the diagnostic uses one `wokwi-pushbutton-6mm` (its docs: pins `1.l/1.r/2.l/2.r`, `1.x` one contact,
  `2.x` the other, bounce on by default). Held at boot stays disabled, release alone enables nothing, one
  press enables with `toggles=1`, a 5 s hold stays 1, the second press disables, the third enables
  (`toggles=3`): [transcript](../evidence/wokwi-enable-button.txt), [screenshot](../evidence/wokwi-enable-button.jpg).
  The simulator ran at about 3 % speed while the browser pane was not displayed and at about 75-100 % when it
  was; runs were done with the pane displayed.
* **Decisions to review**: (1) the disabling press acts at its first edge (fail safe), so a one-sample glitch can
  disable control; (2) a fault clears the latch, so the button must be pressed again after a fault; (3) the
  simulated raw input now starts released (it used to start ON), so existing maintained-switch flows press or
  tick it explicitly; (4) a new setup stages the button kind, a stored record keeps its own.

## Firmware command path (host-compiled)

`firmware/src/{runtime,commands,telemetry}.cpp` are compiled **unmodified** natively against stub Arduino, Wire,
Preferences and NimBLE headers (`tests/firmware_host/stubs`), and `tests/test_firmware_commands.cpp` drives them
exactly as the device is driven: every command is serial bytes through `serviceRuntime()`, the bounded 80-byte line
parser, `command()` and the telemetry queue, and each result is judged from the bytes the firmware writes back.
This replaces the earlier claim that the firmware parser "mirrors" the tested desktop parser: it is now the code
under test.

* Parser: unknown/empty/trailing-text commands, request-id echo (including the largest id and an out-of-range one),
  an overlong line that must not execute its valid prefix, CRLF, control characters, a burst of 8 commands larger
  than the 2-slot acknowledgement queue (all answered, in order, none dropped).
* Every new command (`enable`, `gesture`, `train`, `handsfree`, `motion`, `fault`, `dwell`, `settings`) with valid and
  invalid arguments, including NaN/inf, out-of-range and trailing text; one gesture plays at a time.
* Hardware configuration (no `NODX_SIMULATED`): `enable`, `gesture`, `motion` and `fault` are refused, the source is
  `HARDWARE`, the unconfigured enable input keeps control inhibited, and with no sensor control is never reached.
* Whole pipeline in the simulated configuration: calibrate, train both gestures, commit (nothing resumes), gesture
  resume, drag, switch OFF pauses and releases at once, switch ON never resumes, reboot never resumes and the saved
  setup persists, an injected fault never resumes, and an oversized stored record is classified corrupt and the next boot
  fails closed as `CONFIG_INVALID`.
* Mutation checks on the real firmware sources (run, then reverted): accepting `enable 2` fails 5 checks; no longer
  feeding the switch every loop pass fails 8; accepting `enable` on hardware fails 2.

Not covered, by construction: real NVS atomicity and wear, I2C, GPIO levels and bounce, the Arduino scheduler, and BLE
pairing, encryption and delivery (the stub link is declared secured and always accepts notifications). The stubs are
stand-ins, not models of that hardware. The `main.cpp` entry and the PlatformIO build itself are covered by the
firmware builds above.

## Mutation spot checks (development-time, not committed)

Each deliberate defect made the suite fail, which shows the tests can detect these regressions:
recognition not suppressing output (2 failures), OFF edge not releasing immediately (2), no neutral
requirement after execution (1), the enable gate ignored on resume (2), drag not cleared on stop (6+). The first
attempt at the suppression and scroll test passed vacuously and was rewritten until the unmodified pattern
demonstrably scrolls and moves the pointer when nothing suppresses it. A real defect was also found by the
tests: the first `configId` hashed its own checksum and was constant.

## Browser walkthrough (manual, in-app browser; not an automated test)

Isolated companion on its own port and runtime directory: calibration; hands-free setup view; training of both
gestures through the UI including a deliberate retry (an idle timeout was rejected and recovered); explicit
conversion and save (dwell off before, on and locked after); daily use with no resume/pause/select button
(gesture resume, dwell ARMING → PROGRESS → click → LOCKOUT, drag on/off, switch OFF paused at once and ON did not
resume, gesture while OFF ignored); save-failure display with the earlier setup kept and the error cleared after a
successful retry; a Lab block aborted by a pause gesture. Layout had no horizontal overflow at a phone-width
viewport; all 14 new controls have names and are tabbable; the reduced-motion switch applies; live regions made 0
DOM mutations in 4 s of idle polling (after a fix: they were being rebuilt every poll). Bugs found and fixed by
this walkthrough: a CSS class collision (`.chip`), a nested telemetry field read at the wrong level, the cursor
showing in the new view, and live-region churn. Screenshots:
[desktop](../evidence/handsfree-setup-desktop.jpg), [mobile](../evidence/handsfree-setup-mobile.jpg).
Not exercised: keyboard Tab traversal by hand and a real screen reader.

## Hands-free Lab block (browser, synthetic head motion)

Through the real companion UI against the native simulator engine (private port, temporary runtime directory), after
helper setup (calibrate, train pause and drag, explicit save): one complete **12-trial** hands-free block, driven by a
closed-loop script that holds the WASD keys the page already supports, then a second block aborted by a
pause/resume gesture. The persisted `trials.jsonl` rows were checked ([summary](../evidence/handsfree-lab-block.json)):

* Block 1: 12 rows, one `blockId`, trials 1-12, all `interactionMode = HANDS_FREE`, `selectionMethod = DWELL`, one
  `gestureConfigId`, one profile hash, `blockContext` equal to the row fields on every row, no aborts. The Lab showed a
  **single** result card for the block (12 attempts, 12 hits, header naming the hands-free configuration).
* Interruptions: one recognition candidate was deliberately injected during trial 5 (`gesture nod1`, rejected with
  `GAP_TIMEOUT`, nothing executed). `gestureInterruptions` per trial was `[0,1,0,0,1,0,1,0,0,1,0,0]`: the injected
  candidate (trial 5, which needed an upward move) and three more that the *keyboard pointing itself* opened in trials
  2, 7 and 10, every one a trial that needed a downward (pitch+) move; constant-rate single-axis movement looks like
  the first stroke to the recogniser. Each was rejected, the block continued and no command executed. This is the suppression
  cost described in the limitations, observed on synthetic input; it says nothing about real heads.
* Block 2: after two completed trials a pause/resume gesture was executed. The block aborted, a raw row was persisted with
  `aborted = true` and `abortReason = "gesture command executed"` (not counted in the metrics), and a second card
  appeared separately from block 1 (distinct `blockId`), never pooled.

The result is the plumbing check the brief asked for. The hit rate and times are synthetic keyboard pointing and are not
a performance result.


## Pointing and rejected candidates

The Lab block showed rejected recognition candidates during ordinary (keyboard) pointing. Investigated with the
learned templates (pause = nod2, drag = tilt2, scale 1.0), deterministic synthetic input, against the same input on a
legacy-mode system that has nothing to suppress. Full tables:
[measurement](../evidence/pointing-candidates-measurement.txt) (one-off harness; the regression cases below pin it).

* **When a candidate opens.** Only when the filtered rate reaches the learned entry rate (16.8 deg/s, i.e. a held
  movement of about 20 deg/s or more), in the direction a pattern *starts*, after at least 300 ms of stillness. With these
  templates that is pitch+ (the nod's first stroke) and roll+ (the tilt's first stroke). Yaw either way, pitch- and roll-
  never open one, nor does anything at or below 15 deg/s. In the Lab the three trials that opened candidates were all
  downward (pitch+) moves; three other downward trials did not (probably because the recogniser was not armed:
  it needs 300 ms of stillness first; this was not traced per trial).
* **How long it suppresses.** Always one rejection, `TOO_SLOW`, after 310 ms (the learned stroke limit of 300 ms plus
  one sample), whatever the speed. A held movement never reaches the neutral rate, so no further candidate opens until
  the user is still for 300 ms.
* **What it costs.** The pointer movement of that first 310 ms is discarded and never replayed (checked: no burst of
  movement afterwards): 259 px at 20 deg/s, 468 at 30, 680 at 40, 1035 at 60, out of 850 / 1576 / 2338 / 3543 px for
  a one-second hold. Roll+ candidates cost scrolling instead. A ramped onset avoids it only when slow enough (20-30
  deg/s over 500 ms); 40-60 deg/s still opens one at a 500 ms ramp. A 300-hold keyed corpus (fixed LCG, 20 deg/s,
  yaw/pitch) opened 57 candidates (57 of the 69 pitch+ holds; the other 12 presumably followed too little stillness), 57 ms
  of suppression per hold on average, and discarded 5.2 % of the pointer movement.
* **Safety outcome.** In every case: **0 commands executed**, no drag, no pause, state stays `ACTIVE`. A command needs
  four correctly ordered single-axis strokes of 75-300 ms with peaks of at least 33.6 deg/s and gaps of at most
  150 ms; a held movement fails the first stroke. A deliberate double nod at 0.5-1.5x the trained amplitude still
  executes, and still does after rejected candidates. Inherent limit: a real double nod made for another reason (for
  example agreeing in conversation) *is* the pause gesture; nothing in software can tell them apart.
* **Are adjustments justified? Not now, and not by raising a threshold.**
  * Raising the entry rate only moves the cost: a higher entry spares 20-25 deg/s pointing but fast pointing, which
    loses the most, is still hit, and gentle gestures (peaks near the 33.6 minimum) would be missed. The entry rate is
    already derived from the user's own examples.
  * Shortening the learned stroke limit (now 2x the longest example) or ending suppression early when a movement is
    plainly held would cut the 310 ms window, but they change behaviour next to a safety rule and need real gyro
    traces of both strokes and pointing to set safely. Recorded as follow-ups, not implemented.
  * What helps without any code change, verified in the regression suite: if no pattern begins with a yaw or pitch
    stroke (for example pause = tilt + - + -, drag = tilt - + - +), yaw/pitch pointing never opens a candidate
    (identical output to the legacy system at 20-60 deg/s in all four directions) and deliberate gestures still work.
    Whether such patterns are comfortable is a per-user hardware question. The setup view could warn when a pattern
    starts in a pointing direction; not done here.
* **Regression cases** (`tests/test_handsfree.cpp`, section J, 8 checks): bounded rejected candidate and discarded
  (never replayed) movement; window independent of speed while the discard grows; roll+ suppression; no candidate
  below the entry rate or in non-starting directions; ramped onset; the keyed corpus with 0 executions, every
  candidate rejected and the discard share bounded below 10 %; gestures still work afterwards; roll-first patterns leave
  pointing untouched. Mutation checks: removing the stroke limit fails 6 checks (4 new), removing the slow-onset disarm
  fails 2 (1 new), lowering the entry rate breaks the existing suite.

## Wokwi diagnostic

`wokwi/handsfree-diagnostic` compiles unmodified with PlatformIO (ESP32-S3, 0 warnings). In the Wokwi web
editor (no sign-in, nothing saved) it loaded without wiring errors and ran: banner, switch ON, `WHO_AM_I = 0x68`,
live accel lines with `fail=0`, a live click printed `CONTROL SWITCH: OFF (inhibited)` then `ON (permitted)`, and
with the SDA wire removed it printed `no I2C reply` / `SENSOR NOT FOUND / WRONG ID` and re-probed every 5 s.
Part types and pins were checked against docs.wokwi.com (MPU6050, slide switch) and Wokwi's published ESP32-S3
DevKitC-1 pin map (`wokwi-boards`); the board *part type* came from that repository and a public project, not a
docs.wokwi.com part page. **Sensor controls verified:** clicking the MPU6050 opens its panel; moving acceleration X to
1.35 g, rotation X to 191 deg/s, rotation Z to -183 deg/s and temperature to 68.4 C changed the serial line from
`accel g: 0.00 0.00 1.00 | gyro dps: 0.0 0.0 0.0 | 24.0 C` to
`accel g: 1.35 0.00 1.00 | gyro dps: 191.0 0.0 -183.0 | 68.4 C`, and the untouched axes did not change
([transcript](../evidence/wokwi-sensor-controls.txt), [screenshot](../evidence/wokwi-sensor-controls.jpg)).
**Not verified:** the 30 ms switch debounce timing and anything physical.
Screenshots: [run](../evidence/wokwi-diagnostic-run.jpg), [missing sensor](../evidence/wokwi-missing-sensor.jpg).

## Known limitations and open hardware-required checks

* Gesture thresholds, the neutral rule, learned margins and the claim that they separate command from normal
  movement are START values tested only on synthetic input. A rejected candidate costs up to about one stroke of
  pointing. The suppression/discard design needs real-user accidental-activation measurement.
* The firmware command path is now exercised on the host (see above), but only against stand-ins for the Arduino core,
  I2C, NVS and BLE. The NVS adapter's size classification is tested; real flash behaviour is not.
* Keyboard pointing in the Lab opened rejected recognition candidates in 3 of 12 trials (above), each costing about
  310 ms of pointing. They were investigated (next section); nothing executed and no parameter was changed.
  Accidental activation and the real pointing cost need real-user measurement.
* A torn first configuration write fails closed to CONFIG_INVALID until a helper repairs it; real NVS atomicity is untested.
* Unchanged open items: MPU6050 data-ready/`INT_ENABLE` initialisation, exact N16R8 memory and USB configuration, BLE
  pairing/report behaviour including drag during disconnect, electrical enable-switch wiring and bounce, gesture comfort
  and accidental activation with real users, and every START value. **This is not V1 hardware readiness.**
