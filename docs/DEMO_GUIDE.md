# DEMO_GUIDE · 5 minutes

Start `python3 desktop/server.py`; open http://127.0.0.1:8765. Use one tab. For a fresh demonstration use `--runtime /tmp/nodx-demo-session`.

1. **Explain scope:** “This is the pre-hardware software running the same control engine built for the ESP32-S3. These sensor inputs are simulated.”
2. **Calibrate:** Start calibration. Show REST, LEFT, RIGHT, UP, DOWN, NATURAL and profile completion. The synthetic user moves less left/up, so those gains become greater. This verifies arithmetic adaptation, not clinical suitability.
3. **Point and select:** Control studio → Resume. WASD moves the cyan geometric cursor. Hold Space to drag, release to stop. Enable dwell → Resume; remain still to see ARMING → PROGRESS → CLICK → LOCKOUT. Move before completion to cancel; move meaningfully after a click to re-arm.
4. **Scroll and pause:** Q/E rolls until scrolling appears. Pause releases selection and inhibits output; explicit Resume restarts. Reset sliders to zero before resuming.
5. **Safety:** Inject sensor disconnect/NaN. Show SAFE_STATE, zero movement and cancelled dwell. Restore healthy input; show READY remains stationary until Resume. Optional profile corruption requires recalibration; saved files used in this demo are isolated runtime files.
6. **Evidence:** Performance lab → condition/source → Start. Select center, then 12 targets. Show misses, selection time and clearly named nominal rate. Export CSV; inspect raw geometry, condition, input source and profile snapshot/hash. Run both conditions in alternating order for a later real study. Do not turn a software smoke run into a user-performance claim.

Close with what remains: verify wiring and axes, read real MPU data, validate NVS/BLE on physical hardware, measure timing, tune START parameters and collect honest user trials.

## v0.2.0 handoff

Use the new module walkthrough to read the code. `sh scripts/release_gate.sh` runs format/lint,
74 software checks and both firmware builds. `python3 scripts/package.py` accepts only the clean
committed source matching that gate. The v0.1.0 tag stays available for comparison.

Lab blocks freeze source/profile/selection/geometry and show separate summaries. Compare generic
and adaptive using the same temporary selection settings; saved profiles stay intact. Desktop
HOST_CLICK smoke trials exercise the UI, not adaptive pointing. Look for explicit persistence
status in CSV; raw trials remain in runtime JSONL. Changing viewport or an accepted profile aborts
a running block. Pausing or starting calibration requests release without waiting for a new sample.

## Hands-free demonstration (unreleased, simulated)

Use `--runtime /tmp/nodx-demo-handsfree` for a fresh session. Say plainly that the head motion is
scripted and that nothing here is hardware or user evidence.

1. **Helper setup**: calibrate; *Hands-free setup* → *Train pause / resume*; click *Perform example* four
   times, once more to validate, *Accept*; repeat for drag with a different pattern; *Convert and
   save setup*. Show the mode chip change and that the dwell checkbox is locked on.
2. **Daily use, no buttons**: *Control studio*. Perform the pause/resume gesture (state ACTIVE). Press
   `D` briefly and hold still: ARMING → PROGRESS → click → LOCKOUT with no selection button. Perform the
   drag gesture (cursor DRAG), move, perform it again to release.
3. **Switch**: untick *Control-enable switch ON*: control stops at once and a drag would be dropped.
   Tick it again: nothing resumes. Perform the resume gesture.
4. **Faults**: inject a sensor fault; show SAFE_STATE, release and READY after recovery with no
   automatic resume or re-pressed button.
5. **Errors**: block configuration writes on disk and *Save changes*: the UI shows the failure and that
   the earlier setup was kept. Cancel a training run: nothing changes.
6. **Lab**: start a block; a pause gesture, drag, or a configuration change aborts it and the raw
   log keeps the reason. Legacy and hands-free results are never pooled.

Close with what remains: real sensor, N16R8 memory/USB, BLE, enable-switch wiring, per-user gesture
comfort and accidental-activation measurement.
