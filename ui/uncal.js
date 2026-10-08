// Pure companion logic for the temporary UNCALIBRATED pointer demo. No DOM access (unit tested).
// The demo is a bench aid: real sensor, conservative RAM-only settings, movement only. It is never a
// calibration, is never saved and never starts by itself.
export const BANNER = 'UNCALIBRATED DEMO — LIVE SENSOR';
export const BANNER_DWELL = 'UNCALIBRATED DEMO — DWELL CLICK';
const AXIS = ['X', 'Y', 'Z'];
const SIGN = (value) => (value < 0 ? '−' : '+');

export function uncalOf(device) {
  const raw = device?.handsFree?.uncalDemo;
  return {
    reported: !!raw,
    active: raw?.active === true,
    needsEnable: raw?.needsEnable !== false,
    present: raw?.present === true,
    permitted: raw?.permitted === true,
    reverseX: raw?.reverseX === true,
    reverseY: raw?.reverseY === true,
    blocked: typeof raw?.blocked === 'string' ? raw.blocked : '',
    profileState: typeof raw?.profileState === 'string' ? raw.profileState : 'MISSING',
    gain: Number(raw?.gain) || 0,
    deadzone: Number(raw?.deadzone) || 0,
    maxStep: Number(raw?.maxStep) || 0,
    dwell: {
      enabled: raw?.dwell?.enabled === true,
      ms: Number(raw?.dwell?.ms) || 0,
      tolerance: Number(raw?.dwell?.tolerance) || 0,
      state: typeof raw?.dwell?.state === 'string' ? raw.dwell.state : 'IDLE',
      progress: Math.min(1, Math.max(0, Number(raw?.dwell?.progress) || 0)),
      clicks: Number(raw?.dwell?.clicks) || 0,
    },
  };
}
// The demo exists only on a live hardware device; a simulator never offers it.
export function uncalView(device) {
  const u = uncalOf(device);
  const hardware = device?.source === 'HARDWARE' && u.reported;
  const active = hardware && u.active;
  const canStart = hardware && !active && u.blocked === '' && device.connected === true;
  let status;
  if (!hardware) status = 'Available only with the live hardware device.';
  else if (active) status = 'Active: the pointer follows the real sensor. Movement only.';
  else if (u.blocked) status = `Not started: ${u.blocked}.`;
  else status = 'Ready: press Start without calibration to begin. Nothing starts by itself.';
  const calibration =
    u.profileState === 'CORRUPT'
      ? 'The saved profile is CORRUPT. The demo does not use or repair it.'
      : u.profileState === 'VALID'
        ? 'A saved profile exists; the demo does not use it.'
        : 'No saved profile. This is not a calibration and nothing is saved.';
  const dwellOn = active && u.dwell.enabled;
  const dwellStatus = !active
    ? 'Dwell clicking needs the demo running.'
    : !dwellOn
      ? 'Movement only: no clicks. Enable dwell clicking to demonstrate a click.'
      : u.dwell.state === 'LOCKOUT'
        ? 'Clicked. Move deliberately away before the next click can arm.'
        : u.dwell.state === 'PROGRESS'
          ? `Dwelling: ${Math.round(u.dwell.progress * 100)}%`
          : u.dwell.state === 'ARMING'
            ? 'Arming: hold still.'
            : 'Dwell armed: hold still on the target.';
  return {
    hardware,
    active,
    canStart,
    status,
    calibration,
    banner: active ? (dwellOn ? BANNER_DWELL : BANNER) : '',
    dwellOn,
    canEnableDwell: active,
    dwellStatus,
    u,
  };
}
// What each board rotation does to the pointer, taken from the mapping the firmware reports. The
// mounting orientation is measured on the bench (scripts/bench_axes.py), never guessed from the
// sensor identity.
export function mappingLines(device) {
  const map = device?.axes;
  if (!map || !Array.isArray(map.gyro) || !Array.isArray(map.gyroSigns)) {
    return ['The firmware did not report its axis mapping.'];
  }
  const names = ['Yaw (turn left/right)', 'Pitch (tilt up/down)', 'Roll (sideways tilt)'];
  const lines = names.map(
    (name, i) =>
      `${name}: sensor gyro ${AXIS[map.gyro[i]] ?? '?'} axis, sign ${SIGN(map.gyroSigns[i])}`,
  );
  lines.push(
    map.valid ? 'Mapping valid.' : 'Mapping INVALID: the demo cannot start and samples are rejected.',
  );
  return lines;
}
export const ROTATION_GUIDE = [
  'Turn the board to the LEFT → pointer moves left.',
  'Turn the board to the RIGHT → pointer moves right.',
  'Tilt the front end UP → pointer moves up.',
  'Tilt the front end DOWN → pointer moves down.',
  'Sideways roll does nothing in this demo (no scrolling).',
];
export const DWELL_NOTE =
  'Dwell time and tolerance are temporary START values held in memory only. Distance is measured in ' +
  'accumulated outgoing HID movement units; the Mac applies pointer acceleration, so these are NOT ' +
  'verified screen pixels. The first dwell also includes a short arming delay.';
// The simulator-only "pause when the window loses focus" rule must not end a hardware bench demo
// just because you look at another window; the physical button and Stop demo remain in force.
export function pauseOnBlur(device) {
  return !uncalView(device).active;
}
// Performance Lab runs and comparisons never include the uncalibrated demo.
export function labBlocked(device) {
  return uncalView(device).active;
}
