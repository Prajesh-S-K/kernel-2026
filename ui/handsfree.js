// Pure companion logic for hands-free setup and daily status. No DOM access, so it is unit tested.
export const REQUIRED_EXAMPLES = 4;
export const GESTURES = [
  { id: 'PAUSE_RESUME', key: 'pause', label: 'Pause / resume' },
  { id: 'DRAG', key: 'drag', label: 'Drag on / off' },
];
// Patterns the simulators can perform. Real users choose their own comfortable pattern.
export const SIMULATED_PATTERNS = [
  { id: 'nod2', label: 'Double nod' },
  { id: 'tilt2', label: 'Double sideways tilt' },
  { id: 'turn2', label: 'Double turn' },
  { id: 'nod3', label: 'Triple nod' },
  { id: 'tilt3', label: 'Triple sideways tilt' },
];
const EMPTY = Object.freeze({
  mode: 'LEGACY_SWITCH',
  config: 'MISSING',
  configId: '00000000',
  switch: {
    present: false,
    on: false,
    permitted: false,
    switchless: false,
    switchlessStaged: false,
  },
  gesture: {
    state: 'WAIT_NEUTRAL',
    last: 'NONE',
    lastReject: 'NONE',
    candidates: 0,
    rejected: 0,
    executed: 0,
    refused: 0,
    suppressing: false,
  },
  drag: false,
  training: {
    phase: 'IDLE',
    gesture: 'NONE',
    accepted: 0,
    required: REQUIRED_EXAMPLES,
    rejects: 0,
    validated: false,
    reason: 'idle',
  },
  staged: [false, false],
  stored: [false, false],
  blocked: '',
});

// Older engines and legacy telemetry have no handsFree object: treat them as legacy mode.
export function handsFreeOf(device) {
  return device?.handsFree ? { ...EMPTY, ...device.handsFree } : EMPTY;
}
export function isHandsFree(device) {
  return handsFreeOf(device).mode === 'HANDS_FREE';
}
// What a Lab block must freeze: legacy and hands-free results are never pooled.
export function interactionIdentity(device) {
  const hf = handsFreeOf(device);
  return {
    interactionMode: hf.mode,
    gestureConfigId: hf.mode === 'HANDS_FREE' ? hf.configId : 'NONE',
  };
}
export function modeLabel(mode) {
  return (
    {
      LEGACY_SWITCH: 'LEGACY SWITCH MODE (compatibility)',
      HANDS_FREE: 'HANDS-FREE',
      CONFIG_INVALID: 'HANDS-FREE CONFIG INVALID',
    }[mode] || 'UNKNOWN MODE'
  );
}
export function switchLabel(hf) {
  if (hf.mode !== 'HANDS_FREE' && hf.mode !== 'CONFIG_INVALID') return 'Not used in legacy mode';
  if (!hf.switch.present)
    return hf.switch.switchless ? 'Qualified without a switch' : 'NOT CONFIGURED';
  return hf.switch.permitted ? 'ON · control permitted' : 'OFF · control inhibited';
}

export function setupSteps(device) {
  const hf = handsFreeOf(device);
  const learned = (index) => !!(hf.stored[index] || hf.staged[index]);
  const detail = (index) =>
    hf.staged[index] ? 'Staged, not saved yet' : hf.stored[index] ? 'Saved' : 'Not trained';
  const switchReady = !!(hf.switch.present || hf.switch.switchless || hf.switch.switchlessStaged);
  const steps = [
    {
      id: 'calibrate',
      title: 'Calibrate the motion profile',
      done: !!device?.hasProfile,
      detail: device?.hasProfile ? 'A valid profile is saved' : 'Calibration is required first',
    },
    {
      id: 'pause',
      title: 'Train the pause / resume gesture',
      done: learned(0),
      detail: detail(0),
    },
    { id: 'drag', title: 'Train the drag gesture', done: learned(1), detail: detail(1) },
    {
      id: 'switch',
      title: 'Confirm the control-enable switch',
      done: switchReady,
      detail: hf.switch.present
        ? 'A maintained switch is configured'
        : hf.switch.switchless || hf.switch.switchlessStaged
          ? 'Alternative without a switch qualified by the helper'
          : 'No switch configured: control stays inhibited unless an alternative is qualified',
    },
    {
      id: 'commit',
      title: 'Save the hands-free setup',
      done: hf.mode === 'HANDS_FREE' && !hf.staged.some(Boolean),
      detail:
        hf.mode === 'HANDS_FREE'
          ? hf.staged.some(Boolean)
            ? 'Changes staged: save again to keep them'
            : 'Saved (dwell selection enabled)'
          : 'Converts the profile to dwell selection; nothing changes until you save',
    },
    {
      id: 'resume',
      title: 'Resume control (helper, once)',
      done: device?.state === 'ACTIVE',
      detail: device?.state === 'ACTIVE' ? 'Control is active' : 'Daily use resumes by gesture',
    },
  ];
  const current = steps.findIndex((step) => !step.done);
  return steps.map((step, index) => ({
    ...step,
    state: step.done ? 'done' : index === current ? 'current' : 'todo',
  }));
}

const PHASE_TEXT = {
  IDLE: ['No training in progress', 'Choose a gesture to train.'],
  REST: ['Stay still', 'Measuring your resting movement.'],
  ANALYZE: ['Analysing', 'Learning the pattern limits.'],
  VALIDATE: ['Repeat once to validate', 'Perform the pattern one more time.'],
  READY: ['Pattern recognised', 'Accept it to stage it. It is not saved yet.'],
  FAILED: ['Training stopped', ''],
};
export function trainingView(hf) {
  const t = hf.training;
  const required = t.required || REQUIRED_EXAMPLES;
  let [headline, detail] = PHASE_TEXT[t.phase] || ['Unknown phase', ''];
  if (t.phase === 'EXAMPLE') {
    headline = `Example ${Math.min(t.accepted + 1, required)} of ${required}`;
    detail = t.reason;
  } else if (t.phase === 'FAILED') {
    detail = t.reason;
  }
  const progress =
    t.phase === 'IDLE'
      ? 0
      : ['VALIDATE', 'READY', 'ANALYZE'].includes(t.phase)
        ? 1
        : t.accepted / required;
  return {
    headline,
    detail,
    progress,
    accepted: t.accepted,
    required,
    rejects: t.rejects,
    canAccept: t.phase === 'READY' && t.validated,
    canCancel: t.phase !== 'IDLE',
    failed: t.phase === 'FAILED',
    active: t.phase !== 'IDLE' && t.phase !== 'FAILED',
    gesture: t.gesture,
  };
}

export function recoveryGuidance(device) {
  const hf = handsFreeOf(device);
  const lines = [];
  let title;
  if (device?.faultCode === 'STORAGE') {
    title = 'Saving failed';
    lines.push(
      'The last save was not completed and the earlier configuration was kept.',
      'Check free storage and the connection, then try again.',
    );
  } else if (device?.state === 'SAFE_STATE') {
    title = 'Control stopped for safety';
    lines.push(
      `Reason: ${device.reason}.`,
      'Fix the cause (connection, sensor or profile) and hold still.',
      'Control returns to READY after 20 healthy samples. Nothing resumes by itself.',
      'Then resume with your gesture or the helper Resume button.',
    );
  } else if (hf.mode === 'CONFIG_INVALID') {
    title = 'Hands-free configuration cannot be used';
    lines.push(
      `Stored configuration state: ${hf.config}.`,
      'A helper must retrain both gestures and save, or choose legacy compatibility mode.',
      'Control stays inhibited until then.',
    );
  } else if (device?.state === 'TRAINING') {
    title = 'Training in progress';
    lines.push('Output is stopped while training. Cancel at any time; nothing is changed.');
  } else if (device?.state === 'CALIBRATING') {
    title = 'Calibrating';
    lines.push('Output is stopped while calibrating.');
  } else if (hf.mode === 'HANDS_FREE' && !hf.switch.present && !hf.switch.switchless) {
    title = 'No enable switch configured';
    lines.push(
      'Hands-free control stays inhibited without a configured control-enable switch.',
      'Wire the switch, or have a helper qualify an alternative during setup.',
    );
  } else if (hf.mode === 'HANDS_FREE' && hf.switch.present && !hf.switch.permitted) {
    title = 'Control switch is OFF';
    lines.push(
      'Switch it ON to permit control.',
      'Control stays paused after switching ON until you resume with your gesture.',
    );
  } else if (device?.state === 'ACTIVE') {
    title = 'Active';
    lines.push(
      'Pointing follows your head; dwell clicks automatically after you hold still.',
      hf.drag
        ? 'Dragging: perform the drag gesture to release.'
        : 'Perform the drag gesture to start a drag.',
      'Perform the pause gesture to stop, or switch the control switch OFF.',
    );
  } else if (device?.state === 'PAUSED' || device?.state === 'READY') {
    title = device.state === 'PAUSED' ? 'Paused' : 'Ready';
    lines.push(
      hf.mode === 'HANDS_FREE'
        ? 'Perform your pause / resume gesture to start control.'
        : 'Use Resume to start control (legacy compatibility mode).',
    );
    if (hf.blocked) lines.push(`Resume is blocked: ${hf.blocked}.`);
  } else if (!device?.hasProfile) {
    title = 'Calibration required';
    lines.push('A helper must calibrate a motion profile first.');
  } else {
    title = 'Waiting';
    lines.push('Waiting for the device.');
  }
  return { title, lines };
}
