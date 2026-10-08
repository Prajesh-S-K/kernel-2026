# DECISION_LOG

| Date | Decision | Reason / limit |
|---|---|---|
| 2026-10-08 | C++17 core shared by native simulator and Arduino ESP32-S3 | One algorithm implementation; UI does not duplicate control math |
| 2026-10-08 | Small dependency-free local HTML/CSS/JS companion and Python standard-library bridge | Readable code and offline use; no server deployment required |
| 2026-10-08 | Immediate output suppression on any invalid sample | Conservative simulation baseline; physical timeout/noise behavior must be tuned |
| 2026-10-08 | Relative yaw/pitch gyro pointing; complementary roll estimate | No yaw magnetometer required; roll uses qualified gravity/gyro mapping; dynamic acceleration can bias roll |
| 2026-10-08 | Frozen timestamp detection; no equality-of-values freeze heuristic | A stationary user can legitimately produce identical values |
| 2026-10-08 | Continuous precision/normal/travel gain curve | Avoid a gain jump at thresholds; the constants are START values |
| 2026-10-08 | Failed small/noisy calibration rejected | Do not suppress all controllable movement with a huge deadzone; real limited-mobility tuning remains required |
| 2026-10-08 | Dwell based on bounded relative output; opt-in; switch priority | No OS position feedback is available through BLE HID; host acceleration/edges remain a limitation |
| 2026-10-08 | 84-byte explicit little-endian profile encoding with CRC32 and two generations | Avoid C++ struct padding; retain prior slot under torn writes; CRC detects corruption, not malicious alteration |
| 2026-10-08 | No automatic resume after fault/pause | Recovery cannot unexpectedly start a pointer or reactivate a held switch |
| 2026-10-08 | Hand-built standard BLE HID report service with pinned NimBLE | Avoid automatic battery service and unrelated features; report protocol only; BLE validation remains physical |
| 2026-10-08 | Disabled GPIO defaults | A compile-ready image must not assume unverified wiring |
| 2026-10-08 | USB setup telemetry plus BLE pointing | Companion can show live state later without modifying basic host HID behavior |
| 2026-10-08 | Exploratory circular-target lab; nominal successful ΣID/Σtime | Honest metric name; no effective-width/ISO assertion; raw misses and aborted trials retained |
| 2026-10-08 | Local Git baseline and tag; no remote created | Clean versioned handoff without publishing code or choosing a remote account |

## 0.2.0 professional hardening

- Split responsibilities without adding parallel control algorithms or a JS framework. Read-only
  System views preserve existing readers; all accepted profile changes use validation.
- Typed faults drive logic. Readable reasons are presentation, not condition checks.
- Stop commands release before storage and before another sample. Failed delivery inhibits output;
  recovery requires consecutive healthy checks plus successful neutral delivery and explicit resume.
- Bound all transport waits; discard a failed native session rather than accepting a late stale reply.
  Persist trials separately from device commands. Finish telemetry lines before acknowledgement priority
  applies, so JSON lines cannot interleave.
- Freeze and group Lab context by block/settings. Identical temporary selection overrides make
  condition changes explicit; host-pointer smoke trials cannot establish adaptation benefit.
- Formatting/lint tools are pinned development dependencies. Gate stamps bind all source bytes and
  built artifact hashes; packaging refuses dirty/stale inputs. GPIOs and START parameters stay unchanged.

## Hands-free revision (unreleased)

| Decision | Reason / limit |
|---|---|
| Hands-free is a separate, versioned, checksummed two-slot record; the 84-byte profile is untouched | Existing profiles stay compatible; mode is never inferred from the profile |
| Missing record = legacy; corrupt/unsupported/out-of-bounds = CONFIG_INVALID (inhibited) | A damaged setup must not silently fall back to buttons or to an enabled mode |
| Conversion order: profile (dwell on) first, configuration record last as the single commit point; failures restore the profile | A hands-free setup is never partially enabled; interrupted saves are testable with a power-loss model |
| Gestures are generic stroke sequences trained per user, matched by a deterministic state machine; no classifier, no ML | Small, auditable, bounded; users choose comfortable patterns |
| Two commands only: pause/resume and drag toggle | Everything else stays dwell/pointer; fewer patterns to confuse |
| Patterns must differ and neither may be a prefix of the other | A prefix would fire early inside the longer pattern |
| A candidate suppresses pointer, scroll and dwell and its movement is discarded; neutral is required before re-arming | Prevents command motion from moving the pointer or replaying; costs up to about one stroke of pointing on a rejection |
| 2026-10-08 | No recognition threshold or timing is changed after the Lab block showed rejected candidates on keyboard pointing | Measured: candidates open only on sustained pitch+ / roll+ movement of about 20 deg/s or more after 300 ms of stillness, always end `TOO_SLOW` after 310 ms, cost 259-1035 px of movement at 20-60 deg/s, and execute nothing. Raising the entry rate only moves the cost to faster pointing and risks missing gentle gestures; shortening the stroke limit or ending suppression early changes safety-adjacent behaviour. Both need real gyro data first. See EVIDENCE "Pointing and rejected candidates" |
| Dwell lockout after resume and drag release | A user who is still after a gesture must not get a surprise click |
| Enable switch: OFF immediate, ON debounced 30 ms, unknown/unconfigured = OFF; ON never resumes | Fail closed; the switch permits, it is not a power switch |
| `setControlSwitch` is driven every loop pass, not per sensor sample | OFF must release without waiting for a sample |
| Alternative to a switch only by explicit helper qualification stored in the record | "Disabled switch configuration inhibits hands-free hardware mode" |
| Legacy physical-switch logic kept as a compatibility mode and test fixture, ignored in hands-free | Existing regressions stay valid |
| New protocol fields/commands are additive (`protocolRevision` 3) | Existing clients keep working |
| Version strings are not changed in this revision | Release/version handling is a separate task |
