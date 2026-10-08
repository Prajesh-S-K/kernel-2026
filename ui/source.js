export function sourceLabels(source) {
  if (source === 'SIMULATED')
    return {
      title: 'DESKTOP SIMULATOR',
      calibration: 'SYNTHETIC USER C',
      synthetic: true,
      detail: 'Virtual sensor · Shared firmware engine',
      profile: 'desktop simulated',
    };
  if (source === 'FIRMWARE_SIMULATED')
    return {
      title: 'FIRMWARE SIMULATOR',
      calibration: 'SYNTHETIC SENSOR',
      synthetic: true,
      detail: 'USB telemetry · Synthetic firmware input · BLE HID output',
      profile: 'firmware simulated',
    };
  return {
    title: source === 'HARDWARE' ? 'HARDWARE DEVICE' : 'UNKNOWN SOURCE',
    calibration: source === 'HARDWARE' ? 'LIVE SENSOR' : 'UNKNOWN INPUT',
    synthetic: false,
    detail: 'USB telemetry · BLE HID output',
    profile: 'sensor-derived',
  };
}
