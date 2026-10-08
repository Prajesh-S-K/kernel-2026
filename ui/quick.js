// Pure companion logic for the EXPERIMENTAL quick tilt-and-return click. No DOM access.
export const QUICK_LABEL = 'EXPERIMENTAL QUICK GESTURE CLICK';
const num = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);
const REASONS = {
  NONE: '',
  TOO_SMALL: 'the tilt was too small',
  CROSS_AXIS: 'too much sideways drift',
  NO_RETURN: 'it did not come back',
  NOT_BACK_TO_START: 'it did not return to the start orientation',
  TIMEOUT: 'it took longer than a second',
  INVALID_SAMPLE: 'irregular sensor samples',
};

export function quickOf(device) {
  const q = device?.quick;
  return {
    reported: !!q && typeof q === 'object',
    phase: q?.phase ?? 'IDLE',
    cue: q?.cue ?? 'NONE',
    cueMs: num(q?.cueMs),
    progress: Math.min(1, Math.max(0, num(q?.progress))),
    reason: typeof q?.reason === 'string' ? q.reason : '',
    practice: {
      excursion: num(q?.practice?.excursion),
      residual: num(q?.practice?.residual),
      cross: num(q?.practice?.cross),
      planeShare: num(q?.practice?.planeShare),
    },
    ready: q?.ready === true,
    enabled: q?.enabled === true,
    frame: q?.frame === 'CONFIGURED' ? 'CONFIGURED' : 'FALLBACK',
    state: typeof q?.state === 'string' ? q.state : 'OFF',
    suppressing: q?.suppressing === true,
    accepted: num(q?.accepted),
    rejected: num(q?.rejected),
    candidates: num(q?.candidates),
    clicks: num(q?.clicks),
    suppressedMs: num(q?.suppressedMs),
    lastReject: typeof q?.lastReject === 'string' ? q.lastReject : 'NONE',
    last: {
      excursion: num(q?.last?.excursion),
      residual: num(q?.last?.residual),
      durationMs: num(q?.last?.durationMs),
    },
    sensitivity: num(q?.sensitivity) || 1,
    returnTolerance: num(q?.returnTolerance) || 0.35,
    blocked: typeof q?.blocked === 'string' ? q.blocked : '',
  };
}

const STATE_TEXT = {
  OFF: 'Off',
  NEUTRAL: 'Waiting for a calm moment (neutral)',
  READY: 'Ready',
  OUTWARD: 'Outward movement — pointer paused',
  RETURN: 'Return — pointer paused',
  SETTLING: 'Settling — pointer paused',
};

export function quickView(device) {
  const q = quickOf(device);
  const hardware = device?.source === 'HARDWARE' && q.reported;
  const practising = hardware && ['REST', 'TILT', 'PREVIEW'].includes(q.phase);
  const seconds = Math.ceil(q.cueMs / 1000);
  let title = 'Quick gesture click is off';
  let instruction =
    'EXPERIMENTAL. Practise one comfortable sideways tilt and return (about a second). It is off until you enable it.';
  let big = '';
  if (practising && q.phase === 'REST') {
    title = 'Step 1: hold still';
    instruction = 'Hold the assembly completely still so the sensor noise can be measured.';
    big = 'HOLD STILL';
  } else if (practising && q.phase === 'TILT') {
    title = 'Step 2: one tilt and return';
    if (q.cue === 'COUNTDOWN') {
      instruction = 'Get ready. When it says GO: tilt sideways, then come straight back to the start, in about a second.';
      big = String(Math.max(1, seconds));
    } else if (q.cue === 'GO') {
      instruction = 'Now: tilt sideways, then return to the start.';
      big = 'GO';
    } else {
      instruction = `Recording. ${q.reason}`;
      big = '●';
    }
    if (q.reason && q.cue !== 'RECORDING' && !q.reason.startsWith('stillness')) {
      instruction += ` Last attempt: ${q.reason}`;
    }
  } else if (practising && q.phase === 'PREVIEW') {
    title = 'Preview: try it';
    instruction =
      'Nothing clicks here. Try your tilt and return: the state below shows ready, outward, return, then accepted or rejected. Accept when it responds reliably, or retry.';
    big = STATE_TEXT[q.state]?.split(' —')[0].toUpperCase() ?? '';
  } else if (hardware && q.phase === 'FAILED') {
    title = 'Practice did not finish';
    instruction = q.reason;
  } else if (hardware && q.ready) {
    title = 'A practice is saved in memory';
    instruction = `Practised for ${q.frame === 'CONFIGURED' ? 'configured control' : 'the uncalibrated fallback'}. Enable it while control runs.`;
  }
  const last = q.lastReject !== 'NONE' ? (REASONS[q.lastReject] ?? q.lastReject.toLowerCase()) : '';
  const stats =
    q.enabled || (practising && q.phase === 'PREVIEW')
      ? `State: ${STATE_TEXT[q.state] ?? q.state} · accepted ${q.accepted} · rejected ${q.rejected}${
          last ? ` (last: ${last})` : ''
        } · candidate ${q.last.durationMs.toFixed(0)} ms, excursion ${q.last.excursion.toFixed(1)}°, return residual ${q.last.residual.toFixed(1)}° · clicks ${q.clicks} · pointer paused for ${(q.suppressedMs / 1000).toFixed(1)} s in total`
      : '';
  const practiceLine =
    q.ready || (practising && q.phase === 'PREVIEW')
      ? `Practice: excursion ${q.practice.excursion.toFixed(1)}°, return residual ${q.practice.residual.toFixed(1)}°, wander ${q.practice.cross.toFixed(1)}°, ${(Math.round((1 - q.practice.planeShare) * 100))}% outside the pointing plane.`
      : '';
  return {
    hardware,
    practising,
    title,
    instruction,
    big,
    stats,
    practiceLine,
    progress: q.progress,
    enabled: q.enabled,
    ready: q.ready,
    suppressing: q.suppressing,
    canPracticeFallback: hardware && !practising,
    canPracticeConfigured: hardware && !practising && device?.mapping?.learnedValid === true,
    canCancel: practising,
    canAccept: practising && q.phase === 'PREVIEW',
    canRetry: practising && q.phase === 'PREVIEW',
    canClear: hardware && q.ready && !practising,
    canEnable: hardware && q.ready && !q.enabled && q.blocked === '',
    enableBlocked: hardware && q.ready && !q.enabled ? q.blocked : '',
    q,
  };
}
