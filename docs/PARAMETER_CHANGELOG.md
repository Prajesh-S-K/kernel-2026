# PARAMETER_CHANGELOG

| Version/date | Change | Evidence / qualification |
|---|---|---|
| 0.2.0 docs correction | Telemetry period stated as 200 ms (5 Hz), not 500 ms; buzzer deferred with `NODX_BUZZER=-1`; no value changed in code | Documentation only; provisional `esp32s3-n16r8-bench` is compile-only and sets no GPIO |
| 0.2.0 / 2026-10-08 | No control tuning; recovery qualified by 20 healthy system checks plus neutral delivery; telemetry 200ms, command/reply deadlines 2s/3s | Software regressions and cross-build; physical values remain START |
| 0.1.0 / 2026-10-08 | Initial START registry, bounds and identity desktop mapping; disabled firmware GPIOs | Synthetic/native tests and firmware compile only |
| 0.1.0 / 2026-10-08 | Explicit gyro and gravity axis mappings; 2ms physical data-ready polling, 10ms simulated samples | Axis/conversion software tests; physical mount/cadence still required |

For future tuning record: parameter + old/new value + units + sensor/mount/profile/software version + raw recording + comparative result + who approved the measured setting. Never silently relabel a START value as final.
