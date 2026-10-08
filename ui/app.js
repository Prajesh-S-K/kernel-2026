import { DeviceClient } from './transport.js';
import { sourceLabels } from './source.js';
import { createPerformanceLab } from './lab-view.js';
import { drawCursor } from './cursor.js';
import { createHandsFreeView } from './handsfree-view.js';
import { mappingView } from './mapping.js';
import { clickView } from './click.js';
import { quickView } from './quick.js';
import {
  BANNER,
  DWELL_NOTE,
  ROTATION_GUIDE,
  mappingLines,
  pauseOnBlur,
  uncalView,
} from './uncal.js';
const client = new DeviceClient();
const $ = (id) => document.getElementById(id);
let device = null,
  view = 'setup',
  busy = false,
  held = false,
  priorDown = false;
let point = { x: 160, y: 160 },
  keys = new Set(),
  selections = 0,
  scrollTotal = 0,
  dragDistance = 0,
  dragTarget = null;
let traces = [],
  lastCal = 'IDLE',
  toastTimer,
  recording = false;

const instructions = {
  REST: ['Remain comfortably still', 'Learning resting variation and gyro bias.'],
  LEFT: ['Turn comfortably left', 'Measuring comfortable leftward control.'],
  RIGHT: ['Turn comfortably right', 'Measuring comfortable rightward control.'],
  UP: ['Look comfortably up', 'Measuring comfortable upward control.'],
  DOWN: ['Look comfortably down', 'Measuring comfortable downward control.'],
  NATURAL: ['Move naturally', 'Collecting a final sample window.'],
  ANALYZE: ['Analyzing motion', 'Calculating deadzones and directional gain.'],
  VALIDATE: ['Validating your profile', 'Checking all parameters against allowed bounds.'],
  PROFILE_SAVE: ['Saving your profile', 'Verifying the new slot before accepting it.'],
  COMPLETE: [
    'NodX adapted. Control ready.',
    'Your simulated motion profile is saved. Resume when you are ready.',
  ],
  FAILED: ['Calibration did not complete', 'Your last valid profile is preserved.'],
};
const request = (data, endpoint = '/api/device') => client.request(data, endpoint);
async function action(action, extra = {}) {
  try {
    const data = await request({ action, ...extra });
    render(data, false);
    if (!data.ok)
      toast(`Action refused: ${data.reason}. Check profile, connection and healthy samples.`);
    return data;
  } catch (error) {
    toast(error.message);
    return null;
  }
}
function toast(text) {
  $('toast').textContent = text;
  $('toast').classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('toast').classList.remove('show'), 4500);
}
function download(name, text, type = 'text/plain') {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function space() {
  return view === 'lab' ? $('arena') : $('studio');
}
function showView(next) {
  if (performanceLab.running) performanceLab.abort('navigation');
  keys.clear();
  held = false;
  view = next;
  document.querySelectorAll('.view').forEach((v) => (v.hidden = v.id !== next));
  document
    .querySelectorAll('.nav')
    .forEach((b) => b.classList.toggle('active', b.dataset.view === next));
  const box = space().getBoundingClientRect();
  point = { x: box.width / 2, y: box.height / 2 };
  renderCursor();
}
function renderCursor() {
  const host =
    view === 'lab' && $('inputSource').value === 'HOST_POINTER' && device?.source === 'SIMULATED';
  drawCursor(
    $('nodxCursor'),
    $('cursorText'),
    point,
    space().getBoundingClientRect(),
    view === 'setup' || view === 'handsfree',
    host ? 'HOST_POINTER' : device?.cursor || 'WARNING',
    device?.dwellProgress || 0,
  );
}
function geometry() {
  return { width: $('arena').clientWidth, height: $('arena').clientHeight, devicePixelRatio };
}
function render(data, applyReports = true) {
  device = data;
  const hardware = data.source !== 'SIMULATED';
  const labels = sourceLabels(data.source);
  $('deviceSource').textContent = labels.title;
  $('motionHeading').textContent = labels.synthetic ? 'SIMULATED MOTION' : 'SENSOR MOTION';
  $('motionSource').textContent = labels.synthetic ? '● SYNTHETIC' : '● LIVE SENSOR';
  $('calSource').textContent = labels.calibration;
  $('deviceDetail').textContent = labels.detail;
  $('calEvidence').textContent = labels.synthetic
    ? 'Synthetic inputs; no participant or physical sensor evidence.'
    : 'Live samples; comfortable movement and hardware parameters still require validation.';
  $('gainEvidence').textContent =
    `Directional gain: output pixels / degree. Generated from ${labels.profile} samples.`;
  for (const id of ['yaw', 'pitch', 'roll', 'switch', 'fault', 'ble', 'corrupt', 'record'])
    $(id).disabled = hardware;
  if (hardware && $('inputSource').value !== 'HOST_POINTER')
    $('inputSource').value = 'HOST_POINTER';
  $('connection').innerHTML = '<i></i>ENGINE CONNECTED';
  $('systemState').textContent = data.state;
  $('profileState').textContent = data.hasProfile ? 'VALID' : 'REQUIRED';
  $('faultCount').textContent = data.faults;
  $('reason').textContent = data.reason;
  $('sessionStatus').textContent = data.state.replaceAll('_', ' ');
  $('cursorLabel').textContent = data.cursor.replaceAll('_', ' ');
  $('dwellLabel').textContent = `Dwell: ${data.dwell} · ${data.cancellations} cancellations`;
  $('dwellBar').style.width = data.dwellProgress * 100 + '%';
  $('pause').textContent = data.state === 'ACTIVE' ? 'Ⅱ Pause' : '▷ Resume';
  const cal = data.calibration;
  if (instructions[cal]) {
    const cue = Math.ceil((data.calibrationCueMs || 0) / 1000);
    $('instruction').textContent = cue
      ? `${cue} — get ready: ${instructions[cal][0].toLowerCase()}`
      : instructions[cal][0];
    $('phaseDescription').textContent = cue
      ? 'Nothing is measured during the countdown.'
      : cal === 'FAILED'
        ? data.calibrationReason
        : cal === 'COMPLETE'
          ? `Your ${labels.profile} profile is saved. Resume when ready.`
          : instructions[cal][1];
    $('phaseLabel').textContent = cal === 'COMPLETE' ? 'PROFILE GENERATED' : 'CALIBRATION / ' + cal;
  }
  $('calibrate').disabled =
    performanceLab.running || ['CALIBRATING', 'TEACHING'].includes(data.state);
  $('cancel').disabled = data.state !== 'CALIBRATING';
  $('calProgress').style.width = data.calibrationProgress * 100 + '%';
  $('calPercent').textContent = Math.round(data.calibrationProgress * 100) + '%';
  document
    .querySelectorAll('.direction')
    .forEach((d) => d.classList.toggle('active', d.textContent === cal));
  $('stepCal').classList.toggle('done', data.state === 'CALIBRATING' || cal === 'COMPLETE');
  $('stepProfile').classList.toggle('done', data.hasProfile);
  if (cal === 'COMPLETE' && lastCal !== 'COMPLETE')
    toast(`Calibration complete. Your ${labels.profile} profile is saved.`);
  lastCal = cal;
  const p = data.profile;
  $('profileBadge').textContent = data.hasProfile ? 'VALID PROFILE' : 'NOT CALIBRATED';
  for (const [i, id] of ['gainLeft', 'gainRight', 'gainUp', 'gainDown'].entries())
    $(id).textContent = data.hasProfile ? p.gain[i].toFixed(1) : '—';
  $('deadzone').textContent = data.hasProfile
    ? p.deadzone.map((x) => x.toFixed(2)).join(' / ') + ' °/s'
    : '—';
  $('alpha').textContent = data.hasProfile ? p.alpha.toFixed(2) : '—';
  $('dwell').checked = p.dwellEnabled;
  $('scroll').checked = p.scrollEnabled;
  $('stabilityValue').textContent = Math.round(data.stability * 100) + '%';
  $('stabilityBar').style.width = data.stability * 100 + '%';
  $('motionValues').textContent =
    data.motion
      .slice(0, 2)
      .map((x) => x.toFixed(1))
      .join(' / ') + ' °/s';
  $('headGraphic').style.transform =
    `rotate(${Math.max(-25, Math.min(25, data.motion[2]))}deg) translate(${data.motion[0] * 0.3}px,${data.motion[1] * 0.3}px)`;
  traces.push(data.motion.slice(0, 2));
  if (traces.length > 100) traces.shift();
  drawTrace();
  if (
    applyReports &&
    (view === 'control' || (view === 'lab' && $('inputSource').value === 'SIMULATED'))
  ) {
    const bounds = space().getBoundingClientRect();
    for (const [dx, dy, wheel, down] of data.reports) {
      const old = { ...point };
      point.x = Math.max(0, Math.min(bounds.width - 1, point.x + dx));
      point.y = Math.max(0, Math.min(bounds.height - 1, point.y + dy));
      scrollTotal += wheel;
      if (down && !priorDown && view === 'control') {
        dragTarget =
          [...document.querySelectorAll('.practice-target')].find((t) => insideElement(t, point)) ||
          null;
      }
      if (down && dragTarget) {
        dragTarget.style.left = dragTarget.offsetLeft + point.x - old.x + 'px';
        dragTarget.style.top = dragTarget.offsetTop + point.y - old.y + 'px';
        dragDistance += Math.hypot(point.x - old.x, point.y - old.y);
      }
      if (!down && priorDown && data.state === 'ACTIVE') selectAt(point.x, point.y);
      if (!down) dragTarget = null;
      priorDown = !!down;
    }
    $('studioResult').textContent =
      `Selections: ${selections} · Scroll: ${scrollTotal} · Drag distance: ${dragDistance.toFixed(0)} px`;
  }
  renderUncal(data);
  renderMapping(data);
  renderClick(data);
  renderQuick(data);
  handsFree.render(data);
  performanceLab.check(data);
  renderCursor();
}
function insideElement(element, p) {
  return (
    p.x >= element.offsetLeft &&
    p.x <= element.offsetLeft + element.offsetWidth &&
    p.y >= element.offsetTop &&
    p.y <= element.offsetTop + element.offsetHeight
  );
}
function drawTrace() {
  const c = $('trace'),
    ctx = c.getContext('2d');
  const width = c.clientWidth || 320;
  const ratio = devicePixelRatio || 1;
  if (c.width !== Math.round(width * ratio)) {
    c.width = Math.round(width * ratio);
    c.height = 105 * ratio;
  }
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, 105);
  ctx.strokeStyle = '#24363c';
  ctx.lineWidth = 1;
  for (let y = 15; y < 105; y += 25) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }
  ['#64ead7', '#b0bdff'].forEach((color, axis) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    traces.forEach((p, i) => {
      const x = (i * width) / 99,
        y = 52 - Math.max(-40, Math.min(40, p[axis]));
      if (i) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
    });
    ctx.stroke();
  });
}
function selectAt(x, y) {
  if (view === 'control') {
    if ([...document.querySelectorAll('.practice-target')].some((t) => insideElement(t, { x, y })))
      selections++;
    return;
  }
  if (view === 'lab') performanceLab.select(x, y);
}
const performanceLab = createPerformanceLab({
  $,
  request,
  action,
  toast,
  renderCursor,
  getDevice: () => device,
  getPoint: () => point,
  setPoint: (value) => {
    point = value;
  },
  download,
  geometry,
});
const handsFree = createHandsFreeView({ $, action, toast });
handsFree.bind();
document.querySelectorAll('.nav').forEach((b) => (b.onclick = () => showView(b.dataset.view)));
$('calibrate').onclick = () => action('calibrate', { guided: true });
$('cancel').onclick = () => action('cancel');
$('resume').onclick = () => action('resume');
$('pause').onclick = () => action(device?.state === 'ACTIVE' ? 'pause' : 'resume');
$('load').onclick = () => action('load');
$('exportProfile').onclick = () => {
  if (!device?.hasProfile) {
    toast('Create a valid profile first.');
    return;
  }
  download(
    'nodx-profile.json',
    JSON.stringify(
      { source: device.source, softwareVersion: '0.2.0', parameters: device.profile },
      null,
      2,
    ),
    'application/json',
  );
};
$('dwell').onchange = () => {
  performanceLab.resetComparison();
  return action('dwell', { enabled: $('dwell').checked });
};
$('scroll').onchange = () => {
  performanceLab.resetComparison();
  return action('scroll', { enabled: $('scroll').checked });
};
$('corrupt').onclick = () => action('corrupt');
$('record').onclick = async () => {
  const r = await action('record', { enabled: !recording });
  if (r?.ok) {
    recording = !recording;
    $('record').textContent = recording ? 'Stop raw recording' : 'Record raw samples';
    toast(
      recording
        ? 'Recording every native sample to runtime/samples.csv.'
        : 'Raw recording saved in runtime/samples.csv.',
    );
  }
};
for (const axis of ['yaw', 'pitch', 'roll'])
  $(axis).oninput = () => ($(axis + 'Out').textContent = $(axis).value + ' °/s');
const setHeld = (value) => {
  held = value;
  $('switch').classList.toggle('held', value);
  $('switch').setAttribute('aria-pressed', String(value));
};
$('switch').onpointerdown = (e) => {
  e.preventDefault();
  $('switch').setPointerCapture(e.pointerId);
  setHeld(true);
};
$('switch').onkeydown = (event) => {
  if (event.key === ' ' || event.key === 'Enter') {
    event.preventDefault();
    setHeld(true);
  }
};
$('switch').onkeyup = (event) => {
  if (event.key === ' ' || event.key === 'Enter') {
    event.preventDefault();
    setHeld(false);
  }
};
$('switch').onblur = () => setHeld(false);
$('switch').onpointerup = () => setHeld(false);
$('switch').onpointercancel = () => setHeld(false);
$('reduced').checked = matchMedia('(prefers-reduced-motion:reduce)').matches;
$('reduced').onchange = () => document.body.classList.toggle('reduced', $('reduced').checked);
$('reduced').onchange();
$('glow').onchange = () => document.body.classList.toggle('no-glow', !$('glow').checked);
$('scale').onchange = () => {
  document.documentElement.style.setProperty('--scale', $('scale').value);
  if (performanceLab.running) performanceLab.abort('scale changed');
};
$('startLab').onclick = performanceLab.start;
$('stopLab').onclick = () => performanceLab.abort();
$('inputSource').onchange = () => {
  performanceLab.renderMetrics();
  renderCursor();
};
$('exportTrials').onclick = performanceLab.exportTrials;
$('arena').onclick = (e) => {
  if (performanceLab.running && $('inputSource').value === 'HOST_POINTER') {
    const box = $('arena').getBoundingClientRect();
    selectAt(e.clientX - box.left, e.clientY - box.top);
  }
};
for (const element of [$('studio'), $('arena')])
  element.addEventListener('pointermove', (e) => {
    if (
      device?.source !== 'SIMULATED' ||
      (view === 'lab' && $('inputSource').value === 'HOST_POINTER')
    ) {
      const box = element.getBoundingClientRect();
      point = { x: e.clientX - box.left, y: e.clientY - box.top };
      renderCursor();
    }
  });
window.addEventListener('keydown', (e) => {
  if (['INPUT', 'SELECT', 'TEXTAREA'].includes(e.target.tagName)) return;
  const key = e.key.toLowerCase();
  if (view !== 'setup' && ['w', 'a', 's', 'd', 'q', 'e', ' '].includes(key)) {
    e.preventDefault();
    keys.add(key);
  }
  if (key === 'p' && !e.repeat) action(device?.state === 'ACTIVE' ? 'pause' : 'resume');
});
window.addEventListener('keyup', (e) => keys.delete(e.key.toLowerCase()));
window.addEventListener('blur', () => {
  keys.clear();
  setHeld(false);
  if (pauseOnBlur(device)) action('pause');
  if (performanceLab.running) performanceLab.abort('window lost focus');
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    keys.clear();
    setHeld(false);
    if (pauseOnBlur(device)) action('pause');
    if (performanceLab.running) performanceLab.abort('page hidden');
  }
});
window.addEventListener('resize', () => {
  if (performanceLab.running) performanceLab.abort('viewport changed');
  renderCursor();
});
window.addEventListener('scroll', renderCursor);
function renderQuick(data) {
  const view = quickView(data);
  $('quickPanel').hidden = !view.hardware;
  if (!view.hardware) return;
  $('quickTag').textContent = view.enabled ? 'ENABLED' : view.practising ? 'PRACTISING' : 'OFF';
  $('quickTitle').textContent = view.title;
  $('quickBig').textContent = view.big;
  $('quickBar').style.width = `${Math.round(view.progress * 100)}%`;
  $('quickInstruction').textContent = view.instruction;
  $('quickPracticeLine').textContent = view.practiceLine;
  $('quickDirection').textContent = view.directionLine;
  $('quickPracticeFallback').disabled = !view.canPracticeFallback;
  $('quickPracticeConfigured').disabled = !view.canPracticeConfigured;
  $('quickAccept').disabled = !view.canAccept;
  $('quickRetry').disabled = !view.canRetry;
  $('quickCancel').disabled = !view.canCancel;
  $('quickClear').disabled = !view.canClear;
  $('quickEnable').disabled = !view.canEnable && !view.enabled;
  $('quickEnable').checked = view.enabled;
  for (const [id, value] of [
    ['quickSens', view.q.sensitivity],
    ['quickTol', view.q.returnTolerance],
    ['quickAngle', view.q.directionTolerance],
  ])
    if (document.activeElement !== $(id)) $(id).value = value;
  $('quickStats').textContent = view.stats;
  $('quickBlocked').textContent = view.enableBlocked ? `Enabling: ${view.enableBlocked}.` : '';
  if (view.enabled) {
    // exclusive with the other click recognisers: the device turns them off, the page shows it
    $('clickEnable').checked = false;
    $('uncalDwell').checked = false;
  }
}
function renderClick(data) {
  const view = clickView(data);
  $('clickPanel').hidden = !view.hardware;
  if (!view.hardware) return;
  $('clickTag').textContent = view.enabled ? 'ENABLED' : view.training ? 'TEACHING' : 'OFF';
  $('clickTitle').textContent = view.title;
  $('clickBig').textContent = view.big;
  $('clickBar').style.width = `${Math.round(view.progress * 100)}%`;
  $('clickInstruction').textContent = view.instruction;
  $('clickTrainFallback').disabled = !view.canTrainFallback;
  $('clickTrainConfigured').disabled = !view.canTrainConfigured;
  $('clickAccept').disabled = !view.canAccept;
  $('clickCancel').disabled = !view.canCancel;
  $('clickClear').disabled = !view.canClear;
  $('clickEnable').disabled = !view.canEnable && !view.enabled;
  $('clickEnable').checked = view.enabled;
  $('clickStats').textContent = view.stats;
  $('clickBlocked').textContent = view.enableBlocked ? `Enabling: ${view.enableBlocked}.` : '';
}
function renderMapping(data) {
  const view = mappingView(data);
  $('mapPanel').hidden = !view.hardware;
  if (!view.hardware) return;
  $('mapTag').textContent = view.running ? 'CONFIGURED' : view.teaching ? 'TEACHING' : 'OFF';
  $('mapTitle').textContent = view.title;
  $('mapBig').textContent = view.big;
  $('mapBar').style.width = `${Math.round(view.progress * 100)}%`;
  $('mapInstruction').textContent = view.instruction;
  $('mapPreview').hidden = !(view.teaching && view.m.phase === 'PREVIEW');
  $('mapDot').style.left = `${50 + (view.preview.angleX / 45) * 45}%`;
  $('mapDot').style.top = `${50 + (view.preview.angleY / 45) * 45}%`;
  $('mapStart').disabled = !view.canStart;
  $('mapAccept').disabled = !view.canAccept;
  $('mapCancel').disabled = !view.canCancel;
  $('mapSave').disabled = !view.canSave;
  $('mapClear').disabled = !view.canClear;
  $('mapControl').disabled = !view.canControl;
  $('mapControlStop').disabled = !view.running;
  $('mapSettings').textContent = `${view.settings} ${view.saveResult === 'SAVE_FAILED_RAM_ONLY' ? 'Saving failed: it works in memory only.' : view.saveResult === 'SAVED' ? 'Saved.' : ''}`;
  $('mapStored').textContent = view.stored;
  $('mapBlocked').textContent = [
    view.controlBlocked ? `Configured control: ${view.controlBlocked}.` : '',
    view.mountingNote,
  ]
    .filter(Boolean)
    .join(' ');
}
function renderUncal(data) {
  const view = uncalView(data);
  $('uncalPanel').hidden = !view.hardware;
  $('uncalBanner').hidden = !view.active;
  $('uncalBannerText').textContent = view.banner || BANNER;
  $('uncalPermissionNote').textContent =
    view.permissionNote || 'Movement only. Not a calibration. Nothing is saved.';
  document.title = view.active ? `${view.banner} · NodX Adapt` : baseTitle;
  if (!view.hardware) return;
  $('uncalTag').textContent = view.active ? 'ACTIVE' : 'OFF';
  $('uncalStatus').textContent = view.status;
  $('uncalCalibration').textContent = view.calibration;
  $('uncalStart').disabled = !view.canStart;
  $('uncalStop').disabled = !view.active;
  $('uncalStopBanner').disabled = !view.active;
  if (document.activeElement !== $('uncalRevX')) $('uncalRevX').checked = view.u.reverseX;
  if (document.activeElement !== $('uncalRevY')) $('uncalRevY').checked = view.u.reverseY;
  $('uncalDwell').disabled = !view.canEnableDwell;
  $('uncalDwell').checked = view.dwellOn;
  $('uncalDwellBar').style.width = `${Math.round(view.u.dwell.progress * 100)}%`;
  $('uncalDwellStatus').textContent = view.dwellStatus;
  $('uncalDwellNote').textContent = DWELL_NOTE;
  $('uncalClicks').textContent = view.dwellOn ? `Clicks this run: ${view.u.dwell.clicks}` : '';
  for (const [id, value] of [
    ['uncalDwellMs', view.u.dwell.ms],
    ['uncalDwellTol', view.u.dwell.tolerance],
  ])
    if (document.activeElement !== $(id) && value) $(id).value = value;
  $('uncalRotations').replaceChildren(
    ...ROTATION_GUIDE.map((line) => Object.assign(document.createElement('li'), { textContent: line })),
  );
  $('uncalMapping').replaceChildren(
    ...mappingLines(data).map((line) => Object.assign(document.createElement('li'), { textContent: line })),
  );
  $('uncalParams').textContent =
    `Fixed START values: ${view.u.gain} px/° gain, ${view.u.deadzone} °/s deadzone, ` +
    `at most ${view.u.maxStep} px per report.`;
}
const baseTitle = document.title;
for (const id of ['uncalRevX', 'uncalRevY'])
  $(id).onchange = () =>
    action('handsfree', {
      op: 'uncalreverse',
      horizontal: $('uncalRevX').checked,
      vertical: $('uncalRevY').checked,
    });
$('uncalDwell').onchange = () =>
  action('handsfree', { op: 'uncaldwell', enabled: $('uncalDwell').checked });
$('uncalDwellApply').onclick = () =>
  action('handsfree', {
    op: 'uncaldwellset',
    ms: Number($('uncalDwellMs').value),
    tolerance: Number($('uncalDwellTol').value),
  });
$('reconnect').onclick = async () => {
  try {
    const result = await request({ action: 'reconnect' });
    toast(result.ok ? 'Reconnecting: the board is rebooting (about 5 seconds).' : result.reason);
  } catch (error) {
    toast(`Reconnect failed: ${error.message}`);
  }
};
for (const [id, extra] of [
  ['quickPracticeFallback', { op: 'practice', frame: 'fallback' }],
  ['quickPracticeConfigured', { op: 'practice', frame: 'configured' }],
  ['quickAccept', { op: 'accept' }],
  ['quickRetry', { op: 'retry' }],
  ['quickCancel', { op: 'cancel' }],
  ['quickClear', { op: 'clear' }],
])
  $(id).onclick = () => action('quick', extra);
$('quickEnable').onchange = () =>
  action('quick', { op: 'enable', enabled: $('quickEnable').checked });
$('quickApply').onclick = () =>
  action('quick', {
    op: 'set',
    sensitivity: Number($('quickSens').value),
    returnTolerance: Number($('quickTol').value),
    directionTolerance: Number($('quickAngle').value),
  });
for (const [id, extra] of [
  ['clickTrainFallback', { op: 'train', frame: 'fallback' }],
  ['clickTrainConfigured', { op: 'train', frame: 'configured' }],
  ['clickAccept', { op: 'accept' }],
  ['clickCancel', { op: 'cancel' }],
  ['clickClear', { op: 'clear' }],
])
  $(id).onclick = () => action('click', extra);
$('clickEnable').onchange = () =>
  action('click', { op: 'enable', enabled: $('clickEnable').checked });
for (const [id, action_, extra] of [
  ['mapStart', 'map', { op: 'start' }],
  ['mapAccept', 'map', { op: 'accept' }],
  ['mapCancel', 'map', { op: 'cancel' }],
  ['mapSave', 'map', { op: 'save' }],
  ['mapClear', 'map', { op: 'clear' }],
  ['mapControl', 'control', { op: 'start' }],
  ['mapControlStop', 'control', { op: 'stop' }],
])
  $(id).onclick = () => action(action_, extra);
$('uncalStart').onclick = () => action('handsfree', { op: 'uncal', enabled: true });
for (const id of ['uncalStop', 'uncalStopBanner'])
  $(id).onclick = () => action('handsfree', { op: 'uncal', enabled: false });
async function tick() {
  if (busy || document.hidden) return;
  busy = true;
  try {
    const yaw = Number($('yaw').value) + (keys.has('d') ? 20 : 0) - (keys.has('a') ? 20 : 0);
    const pitch = Number($('pitch').value) + (keys.has('s') ? 20 : 0) - (keys.has('w') ? 20 : 0);
    const roll = Number($('roll').value) + (keys.has('e') ? 30 : 0) - (keys.has('q') ? 30 : 0);
    const result = await request({
      action: 'step',
      count: 5,
      yaw,
      pitch,
      roll,
      pressed: held || keys.has(' '),
      connected: $('ble').checked,
      automatic: true,
      fault: Number($('fault').value),
      enabled: handsFree.enableInput(device),
    });
    render(result);
  } catch (error) {
    $('connection').textContent = 'ENGINE OFFLINE';
    $('reason').textContent = error.message;
    if (performanceLab.running) performanceLab.abort('engine offline');
  } finally {
    busy = false;
  }
}
let lastHardwarePoll = 0;
performanceLab.renderMetrics();
await action('status');
setInterval(() => {
  if (device?.source === 'SIMULATED' || performance.now() - lastHardwarePoll >= 200) {
    lastHardwarePoll = performance.now();
    tick();
  }
}, 50);
tick();
