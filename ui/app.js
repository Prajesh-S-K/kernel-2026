import { DeviceClient } from './transport.js';
import { sourceLabels } from './source.js';
import { createPerformanceLab } from './lab-view.js';
import { drawCursor } from './cursor.js';
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
    if (!data.ok) toast('Action refused. Check profile, connection and healthy samples.');
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
    view === 'setup',
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
    $('instruction').textContent = instructions[cal][0];
    $('phaseDescription').textContent =
      cal === 'FAILED'
        ? data.calibrationReason
        : cal === 'COMPLETE'
          ? `Your ${labels.profile} profile is saved. Resume when ready.`
          : instructions[cal][1];
    $('phaseLabel').textContent = cal === 'COMPLETE' ? 'PROFILE GENERATED' : 'CALIBRATION / ' + cal;
  }
  $('calibrate').disabled = performanceLab.running || data.state === 'CALIBRATING';
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
document.querySelectorAll('.nav').forEach((b) => (b.onclick = () => showView(b.dataset.view)));
$('calibrate').onclick = () => action('calibrate');
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
  action('pause');
  if (performanceLab.running) performanceLab.abort('window lost focus');
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    keys.clear();
    setHeld(false);
    action('pause');
    if (performanceLab.running) performanceLab.abort('page hidden');
  }
});
window.addEventListener('resize', () => {
  if (performanceLab.running) performanceLab.abort('viewport changed');
  renderCursor();
});
window.addEventListener('scroll', renderCursor);
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
