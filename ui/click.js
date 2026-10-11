// Pure companion logic for the optional gesture click. No DOM access.
const num = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);

export function clickOf(device) {
  const c = device?.click;
  return {
    reported: !!c && typeof c === 'object',
    phase: c?.phase ?? 'IDLE',
    cue: c?.cue ?? 'NONE',
    step: num(c?.step),
    steps: num(c?.steps) || 7,
    validation: c?.validation === true,
    retries: num(c?.retries),
    cueMs: num(c?.cueMs),
    progress: Math.min(1, Math.max(0, num(c?.progress))),
    reason: typeof c?.reason === 'string' ? c.reason : '',
    activityMs: num(c?.activityMs),
    ready: c?.ready === true,
    enabled: c?.enabled === true,
    frame: c?.frame === 'CONFIGURED' ? 'CONFIGURED' : 'FALLBACK',
    state: typeof c?.state === 'string' ? c.state : 'OFF',
    suppressing: c?.suppressing === true,
    accepted: num(c?.accepted),
    rejected: num(c?.rejected),
    candidates: num(c?.candidates),
    clicks: num(c?.clicks),
    lastReject: typeof c?.lastReject === 'string' ? c.lastReject : 'NONE',
    blocked: typeof c?.blocked === 'string' ? c.blocked : '',
  };
}

export function clickView(device) {
  const c = clickOf(device);
  const hardware = device?.source === 'HARDWARE' && c.reported;
  const training = hardware && ['REST', 'EXAMPLE', 'CONFIRMATION', 'CONFUSION_CHECK', 'READY'].includes(c.phase);
  const seconds = Math.ceil(c.cueMs / 1000);
  const what = c.validation ? 'Check' : `Example ${Math.min(c.step + 1, 5)} of 5`;
  let title = 'Gesture click is off';
  let instruction =
    'Teach one deliberate gesture (default: a quick side tilt and return). Ordinary pointing must never click.';
  let big = '';
  if (training && c.phase === 'REST') {
    title = 'Hold still';
    instruction = 'Hold the assembly completely still for about 1.5 seconds.';
    big = 'HOLD STILL';
  } else if (training && c.phase === 'EXAMPLE') {
    title = `${what}: your click gesture`;
    if (c.cue === 'COUNTDOWN') {
      instruction = 'Get ready. When it says GO, make the same short gesture each time (about one second).';
      big = String(Math.max(1, seconds));
    } else if (c.cue === 'GO') {
      instruction = 'Now: the gesture, then stop.';
      big = 'GO';
    } else if (c.cue === 'RECORDING') {
      instruction = 'Recording. Finish the gesture and stay still.';
      big = '●';
    } else {
      instruction = `Return to centre (not measured). ${c.reason}`;
      big = 'CENTRE';
    }
    if (c.retries) instruction += ` Retry ${c.retries} of 3.`;
  } else if (training && c.phase === 'CONFUSION_CHECK') {
    title = 'Check against ordinary pointing';
    instruction = `Move the pointer as you normally would: ${(c.activityMs / 1000).toFixed(1)} of 5.0 s. If it is mistaken for the gesture the training fails.`;
    big = 'POINT';
  } else if (training && c.phase === 'READY') {
    title = 'Gesture ready';
    instruction = 'Ordinary pointing did not trigger it. Accept to use it (kept in memory only).';
    big = 'READY';
  } else if (hardware && c.phase === 'FAILED') {
    title = 'Training did not finish';
    instruction = c.reason;
  } else if (hardware && c.ready) {
    title = 'A gesture is taught';
    instruction = `Taught for ${c.frame === 'CONFIGURED' ? 'configured control' : 'the uncalibrated fallback'}. Enable it while control runs.`;
  }
  const stats = c.enabled
    ? `Gesture clicks: ${c.clicks} · accepted ${c.accepted} · rejected ${c.rejected}${
        c.lastReject !== 'NONE' ? ` (last: ${c.lastReject.toLowerCase().replaceAll('_', ' ')})` : ''
      } · state ${c.state.replaceAll('_', ' ').toLowerCase()}`
    : '';
  return {
    hardware,
    training,
    title,
    instruction,
    big,
    stats,
    progress: c.progress,
    enabled: c.enabled,
    ready: c.ready,
    canTrainFallback: hardware && !training,
    canTrainConfigured: hardware && !training && device?.mapping?.learnedValid === true,
    canCancel: training,
    canAccept: training && c.phase === 'READY',
    canClear: hardware && c.ready && !training,
    canEnable: hardware && c.ready && !c.enabled && c.blocked === '',
    enableBlocked: hardware && c.ready && !c.enabled ? c.blocked : '',
    c,
  };
}
