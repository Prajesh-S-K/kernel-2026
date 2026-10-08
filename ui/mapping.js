// Pure companion logic for guided movement mapping and configured control. No DOM access.
export const CONFIGURED_BANNER = 'CONFIGURED CONTROL — LIVE SENSOR';
export const CONFIGURED_DWELL_BANNER = 'CONFIGURED CONTROL — DWELL CLICK';
const HINTS = {
  RIGHT: 'turn the whole assembly to the right',
  LEFT: 'turn the whole assembly to the left',
  UP: 'tilt the front end up',
  DOWN: 'tilt the front end down',
};
const num = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);

export function mappingOf(device) {
  const m = device?.mapping;
  return {
    reported: !!m && typeof m === 'object',
    phase: m?.phase ?? 'IDLE',
    cue: m?.cue ?? 'NONE',
    step: num(m?.step),
    steps: num(m?.steps),
    direction: m?.direction ?? 'NONE',
    validation: m?.validation === true,
    example: num(m?.example),
    perDirection: num(m?.perDirection) || 3,
    retries: num(m?.retries),
    interruptions: num(m?.interruptions),
    cueMs: num(m?.cueMs),
    stillMs: num(m?.stillMs),
    windowMs: num(m?.windowMs),
    progress: Math.min(1, Math.max(0, num(m?.progress))),
    reason: typeof m?.reason === 'string' ? m.reason : '',
    preview: {
      x: num(m?.preview?.x),
      y: num(m?.preview?.y),
      angleX: Math.max(-45, Math.min(45, num(m?.preview?.angleX))),
      angleY: Math.max(-45, Math.min(45, num(m?.preview?.angleY))),
    },
    learnedValid: m?.learnedValid === true,
    unsaved: m?.unsaved === true,
    stored: typeof m?.stored === 'string' ? m.stored : 'MISSING',
    mode: typeof m?.mode === 'string' ? m.mode : 'OFF',
    blocked: typeof m?.blocked === 'string' ? m.blocked : '',
    saveResult: typeof m?.saveResult === 'string' ? m.saveResult : '',
  };
}

export function mappingView(device) {
  const m = mappingOf(device);
  const hardware = device?.source === 'HARDWARE' && m.reported;
  const teaching = hardware && ['STILL', 'EXAMPLE', 'PREVIEW'].includes(m.phase);
  const running = hardware && m.mode === 'CONFIGURED';
  const controlActive = hardware && m.mode !== 'OFF';
  const seconds = Math.ceil(m.cueMs / 1000);
  const dir = m.direction;
  const label = m.validation
    ? `Check: one more ${dir.toLowerCase()}`
    : `${dir.toLowerCase()} · example ${m.example + 1} of ${m.perDirection}`;
  let title = 'Guided setup is off';
  let instruction = 'Press Start guided setup. It teaches the movements; nothing moves the pointer.';
  let big = '';
  if (teaching && m.phase === 'STILL') {
    title = 'Step 1 of 2: hold still';
    instruction = `Hold the assembly completely still: ${(m.stillMs / 1000).toFixed(1)} of 2.0 s${
      m.interruptions ? ` · interrupted ${m.interruptions}×: ${m.reason}` : ''
    }. Window ${(m.windowMs / 1000).toFixed(0)} of 10 s.`;
    big = 'HOLD STILL';
  } else if (teaching && m.phase === 'EXAMPLE') {
    title = `Step 2 of 2: teach ${label}`;
    if (m.cue === 'COUNTDOWN') {
      instruction = `Get ready: ${HINTS[dir] ?? 'move'} when it says GO. Start from the centre.`;
      big = String(Math.max(1, seconds));
    } else if (m.cue === 'GO') {
      instruction = `Now ${HINTS[dir] ?? 'move'}: one steady turn, then stop.`;
      big = 'GO';
    } else if (m.cue === 'RECORDING') {
      instruction = 'Recording. Finish the turn, then stop and stay still.';
      big = '●';
    } else {
      instruction = `Return slowly to the centre (not measured). ${m.reason}`;
      big = 'CENTRE';
    }
    if (m.retries) instruction += ` Retry ${m.retries} of 3 for this example.`;
  } else if (teaching && m.phase === 'PREVIEW') {
    title = 'Preview';
    instruction =
      'Move the assembly: the dot shows what the pointer would do. Accept if it feels right, or cancel.';
    big = 'PREVIEW';
  } else if (hardware && m.phase === 'FAILED') {
    title = 'Teaching did not finish';
    instruction = m.reason;
  }
  const settings = !m.learnedValid
    ? 'No learned mapping.'
    : m.unsaved
      ? 'A learned mapping is active in memory only (not saved).'
      : 'A learned mapping is saved.';
  const stored =
    m.stored === 'CORRUPT'
      ? 'The stored mapping record is CORRUPT: it is not used and not overwritten.'
      : m.stored === 'VALID'
        ? 'Stored mapping record: valid.'
        : 'Stored mapping record: none.';
  return {
    hardware,
    teaching,
    running,
    title,
    instruction,
    big,
    settings,
    stored,
    saveResult: m.saveResult,
    progress: m.progress,
    preview: m.preview,
    canStart: hardware && !teaching && !controlActive,
    canCancel: teaching,
    canAccept: teaching && m.phase === 'PREVIEW',
    canSave: hardware && m.learnedValid && m.unsaved && !controlActive && !teaching,
    canClear: hardware && m.learnedValid && !teaching,
    canControl: hardware && m.learnedValid && !teaching && !controlActive && m.blocked === '',
    controlBlocked: hardware && m.learnedValid && !controlActive ? m.blocked : '',
    m,
  };
}
