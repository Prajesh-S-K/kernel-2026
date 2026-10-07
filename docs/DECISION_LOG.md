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
