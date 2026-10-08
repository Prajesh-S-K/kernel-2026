// A block owns a frozen context. Changes require a new block, never pooled evidence.
export function freezeContext(device, inputSource, condition, geometry) {
  const profile = structuredClone(device.profile);
  const selectionMethod =
    inputSource === 'HOST_POINTER' && device.source === 'SIMULATED'
      ? 'HOST_CLICK'
      : profile.dwellEnabled
        ? 'SWITCH_OR_DWELL'
        : 'SWITCH';
  const context = {
    deviceSource: device.source,
    inputSource,
    condition,
    profile,
    selectionMethod,
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
export function invalidates(context, device, geometry) {
  if (device.state === 'SAFE_STATE' || device.hasProfile === false) return 'device fault';
  if (context.deviceSource !== device.source) return 'input source changed';
  if (JSON.stringify(context.profile) !== JSON.stringify(device.profile)) return 'profile changed';
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
