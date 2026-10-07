# CRISIS_PLAN

| Trigger | Expected response | Recovery |
|---|---|---|
| Invalid/NaN/extreme sensor, timeout, non-increasing timestamp | SAFE_STATE; zero X/Y/wheel; release report when transport available; dwell cancelled; filter/remainder reset | Healthy samples ≥20; READY; explicit Resume |
| BLE loss / failed notification | Inhibit output; cancel pending selection; cannot release to disconnected host | Reconnect/subscription/encryption; released report; explicit Resume; verify host release behavior |
| Missing/corrupt profile at boot | No ACTIVE state; CALIBRATION_REQUIRED after healthy recovery | New valid calibration, transactional save, explicit Resume |
| Failed/cancelled calibration / torn save | Candidate rejected; prior valid profile retained | Load saved profile or retry comfortable calibration |
| Pause while dragging/dwelling | Zero movement/wheel, released selection; held switch blocked | Release switch; explicit Resume; re-press deliberately |
| Bad axes / calculation / stalled/backward loop | SAFE_STATE with reason | Repair configuration/timing; verify health; explicit Resume |
| Companion offline/hidden or focus lost | Browser pauses native control and aborts a running trial; raw completed rows remain on disk | Restart companion if needed; load profile; explicitly Resume |

For physical demonstration trouble: press pause first, confirm host button release, place the sensor safely on the bench and inspect serial reason. If BLE disconnect prevents release, check the host and recover its pointer independently before continuing. Do not keep wearing the assembly while chasing electrical or bus faults.

Use the native simulator as the fallback demo. State plainly that it is simulated and show the same shared engine, generated profile, crisis bank and raw lab logging. Do not present simulated cursor behavior as a successful hardware demonstration.

The core is cooperative software, not an independent safety controller. A total MCU hang can prevent any release report or buzzer transition; watchdog/reset and host timeout behavior must be physically qualified. Buzzer electrical behavior and brownouts remain untested. A sensor with frozen numbers but valid advancing timestamps is not reliably distinguishable from rest without additional physical checks.
