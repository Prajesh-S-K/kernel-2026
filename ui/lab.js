import { interactionIdentity, handsFreeOf } from './handsfree.js';

// A block owns a frozen context. Changes require a new block, never pooled evidence.
export function freezeContext(device, inputSource, condition, geometry) {
  const profile = structuredClone(device.profile);
  const identity = interactionIdentity(device);
  const selectionMethod =
    inputSource === 'HOST_POINTER' && device.source === 'SIMULATED'
      ? 'HOST_CLICK'
      : identity.interactionMode === 'HANDS_FREE'
        ? 'DWELL'
        : profile.dwellEnabled
          ? 'SWITCH_OR_DWELL'
          : 'SWITCH';
  const context = {
    deviceSource: device.source,
    inputSource,
    condition,
    profile,
    selectionMethod,
    ...identity,
    geometry: { ...geometry },
    softwareVersion: '0.2.0',
  };
  Object.freeze(profile.bias);
  Object.freeze(profile.deadzone);
  Object.freeze(profile.gain);
  Object.freeze(profile);
  Object.freeze(context.geometry);
  return Object.freeze(context);
}
// `baseline` holds counters sampled when the block began (gesture commands executed so far).
export function invalidates(context, device, geometry, baseline = {}) {
  if (device.state === 'SAFE_STATE' || device.hasProfile === false) return 'device fault';
  if (context.deviceSource !== device.source) return 'input source changed';
  const identity = interactionIdentity(device);
  if (context.interactionMode !== identity.interactionMode) return 'interaction mode changed';
  if (context.gestureConfigId !== identity.gestureConfigId) return 'gesture configuration changed';
  if (JSON.stringify(context.profile) !== JSON.stringify(device.profile)) return 'profile changed';
  const hf = handsFreeOf(device);
  if (hf.drag) return 'drag active';
  if (baseline.executed !== undefined && hf.gesture.executed > baseline.executed)
    return 'gesture command executed';
  if (JSON.stringify(context.geometry) !== JSON.stringify(geometry)) return 'geometry changed';
  if (
    (context.inputSource === 'SIMULATED' || device.source !== 'SIMULATED') &&
    device.state !== 'ACTIVE'
  )
    return 'device stopped';
  return '';
}
export function groupBlocks(rows) {
  const groups = new Map();
  for (const row of rows) {
    const key = JSON.stringify([
      row.blockId,
      row.deviceSource,
      row.inputSource,
      row.condition,
      row.selectionMethod,
      row.interactionMode,
      row.gestureConfigId,
      row.profile,
      row.blockContext?.geometry,
    ]);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(row);
  }
  return [...groups.values()];
}
export function comparisonSettings(profile) {
  return { dwellEnabled: profile.dwellEnabled, scrollEnabled: profile.scrollEnabled };
}
