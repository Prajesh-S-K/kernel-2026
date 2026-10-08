// DOM presentation for hands-free setup, daily status and recovery guidance.
import {
  handsFreeOf,
  isHandsFree,
  modeLabel,
  recoveryGuidance,
  setupSteps,
  switchLabel,
  buttonLabel,
  isButton,
  trainingView,
} from './handsfree.js';

const LEGACY_ARM_MS = 5000;

export function createHandsFreeView({ $, action, toast }) {
  let legacyArmedAt = 0,
    latest = null,
    chipKey = '',
    buttonHeld = false,
    buttonUntil = 0;
  // Live regions are announced on every DOM change, so write only when the text really changed.
  const setText = (id, value) => {
    if ($(id).textContent !== value) $(id).textContent = value;
  };

  const canDrive = (device) => ['SIMULATED', 'FIRMWARE_SIMULATED'].includes(device.source);

  function chip(label, value, tone) {
    const node = document.createElement('span');
    node.className = `hf-chip ${tone}`;
    const strong = document.createElement('b');
    strong.textContent = value;
    node.append(label, strong);
    return node;
  }
  function renderChips(device, hf) {
    const stateTone =
      { ACTIVE: 'good', SAFE_STATE: 'bad', TRAINING: 'warn' }[device.state] || 'warn';
    const modeTone = { HANDS_FREE: 'good', CONFIG_INVALID: 'bad' }[hf.mode] || 'warn';
    const switchTone = !(hf.mode === 'HANDS_FREE' || hf.mode === 'CONFIG_INVALID')
      ? 'warn'
      : hf.switch.permitted
        ? 'good'
        : 'bad';
    const chips = [
      chip('State ', device.state.replaceAll('_', ' '), stateTone),
      chip('Mode ', modeLabel(hf.mode), modeTone),
      chip(isButton(hf) ? 'Control ' : 'Control switch ', switchLabel(hf), switchTone),
      ...(isButton(hf) && hf.mode === 'HANDS_FREE'
        ? [chip('Button ', buttonLabel(hf), hf.switch.pressed ? 'warn' : 'good')]
        : []),
      chip('Drag ', hf.drag ? 'ON' : 'off', hf.drag ? 'good' : 'warn'),
      ...(hf.demoMovementOnly ? [chip('Demo ', 'MOVEMENT ONLY (not saved)', 'warn')] : []),
      chip(
        'Recognition ',
        hf.gesture.suppressing ? 'RECOGNISING' : hf.gesture.state.replaceAll('_', ' '),
        'warn',
      ),
    ];
    const key = chips.map((c) => c.textContent).join('|');
    if (key === chipKey) return;
    chipKey = key;
    for (const id of ['hfChips', 'studioChips'])
      $(id).replaceChildren(...chips.map((c) => c.cloneNode(true)));
  }
  function renderSteps(device) {
    const list = $('hfSteps');
    list.replaceChildren();
    for (const step of setupSteps(device)) {
      const item = document.createElement('li');
      item.className = step.state;
      const mark = document.createElement('span');
      mark.className = 'mark';
      mark.textContent = step.done ? '✓' : step.state === 'current' ? '›' : '';
      mark.setAttribute('aria-hidden', 'true');
      const title = document.createElement('span');
      title.className = 'title';
      title.textContent =
        step.title + (step.done ? ' — done' : step.state === 'current' ? ' — next' : '');
      const detail = document.createElement('span');
      detail.textContent = step.detail;
      item.append(mark, title, detail);
      list.append(item);
    }
  }
  function renderTraining(device, hf) {
    const view = trainingView(hf);
    $('hfTrainTag').textContent = hf.training.phase;
    $('hfPhase').textContent = view.headline;
    setText(
      'hfReason',
      view.detail +
        (view.rejects ? ` (${view.rejects} rejected attempt${view.rejects > 1 ? 's' : ''})` : ''),
    );
    $('hfProgress').style.width = `${Math.round(view.progress * 100)}%`;
    $('hfProgressText').textContent =
      view.active || view.failed
        ? `${view.accepted} of ${view.required} examples accepted`
        : `${view.required} examples are needed per gesture`;
    const driven = canDrive(device);
    const setupBlocked = !device.hasProfile || ['CALIBRATING', 'SAFE_STATE'].includes(device.state);
    const busy = device.state === 'TRAINING' && !view.failed;
    $('trainPause').disabled = setupBlocked || busy;
    $('trainDrag').disabled = setupBlocked || busy;
    $('hfExample').disabled = !driven || !['EXAMPLE', 'VALIDATE'].includes(hf.training.phase);
    $('hfAccept').disabled = !view.canAccept;
    $('hfCancel').disabled = !view.canCancel && device.state !== 'TRAINING';
    for (const id of ['patternPause', 'patternDrag']) $(id).closest('label').hidden = !driven;
  }
  function renderGuidance(device) {
    const guide = recoveryGuidance(device);
    $('hfGuideTitle').textContent = guide.title;
    const list = $('hfGuideLines');
    list.replaceChildren(
      ...guide.lines.map((line) => {
        const item = document.createElement('li');
        item.textContent = line;
        return item;
      }),
    );
    const failed = device.faultCode === 'STORAGE';
    list.classList.toggle('error', failed);
    $('hfSaveTag').hidden = !failed;
  }
  function renderStudio(device, hf) {
    const driven = canDrive(device);
    const handsFree = isHandsFree(device);
    const button = isButton(hf);
    $('enableSwitchRow').hidden = button;
    $('enableButton').hidden = !button;
    $('enableSwitch').disabled = !driven;
    $('enableButton').disabled = !driven;
    $('enableButton').textContent = hf.switch.latched
      ? 'Hold to press the enable button (control permitted)'
      : 'Hold to press the enable button (control disabled)';
    $('gesturePause').disabled = !driven || !handsFree;
    $('gestureDrag').disabled = !driven || !handsFree;
    $('switch').disabled = !(device.source === 'SIMULATED') || handsFree;
    $('dwell').disabled = handsFree;
    const last =
      hf.gesture.last === 'NONE' ? 'none yet' : hf.gesture.last.replaceAll('_', ' ').toLowerCase();
    $('studioHfNote').textContent = !handsFree
      ? 'Hands-free mode is not active; legacy compatibility controls apply.'
      : hf.gesture.suppressing
        ? 'Recognising a pattern: pointer, scroll and dwell are paused until it resolves.'
        : `Last gesture: ${last}. Rejected attempts: ${hf.gesture.rejected}` +
          (hf.gesture.lastReject === 'NONE'
            ? '.'
            : ` (last: ${hf.gesture.lastReject.replaceAll('_', ' ').toLowerCase()}).`) +
          (hf.gesture.refused ? ` Refused actions: ${hf.gesture.refused}.` : '');
  }
  function render(device) {
    latest = device;
    const hf = handsFreeOf(device);
    $('hfMode').textContent = modeLabel(hf.mode);
    renderChips(device, hf);
    renderSteps(device);
    renderTraining(device, hf);
    renderGuidance(device);
    renderStudio(device, hf);
    const learned = hf.stored.map((stored, i) => stored || hf.staged[i]);
    const unsaved =
      hf.staged.some(Boolean) ||
      hf.mode !== 'HANDS_FREE' ||
      hf.switch.switchlessStaged !== hf.switch.switchless ||
      hf.switch.kindStaged !== hf.switch.kind;
    $('hfCommit').disabled =
      !device.hasProfile ||
      !learned.every(Boolean) ||
      !unsaved ||
      ['CALIBRATING', 'TRAINING', 'SAFE_STATE'].includes(device.state);
    $('hfCommit').textContent =
      hf.mode === 'HANDS_FREE' ? 'Save changes' : 'Convert and save setup';
    $('hfLegacy').disabled = ['CALIBRATING', 'TRAINING', 'SAFE_STATE'].includes(device.state);
    $('hfLegacy').textContent =
      Date.now() - legacyArmedAt < LEGACY_ARM_MS
        ? 'Press again to confirm legacy mode'
        : 'Return to legacy compatibility mode';
    $('hfSwitchless').checked = hf.switch.switchlessStaged;
    $('hfDemo').checked = !!hf.demoMovementOnly;
    if (document.activeElement !== $('hfEnableKind'))
      $('hfEnableKind').value = hf.switch.kindStaged === 'MAINTAINED' ? 'maintained' : 'momentary';
    $('hfHelperResume').disabled = device.state === 'ACTIVE';
  }
  async function run(request) {
    const result = await request;
    if (result && !result.ok) toast(`Not done: ${result.reason}`);
    return result;
  }
  function bind() {
    $('trainPause').onclick = () => run(action('train', { op: 'start', gesture: 'pause' }));
    $('trainDrag').onclick = () => run(action('train', { op: 'start', gesture: 'drag' }));
    $('hfExample').onclick = () => {
      const drag = handsFreeOf(latest).training.gesture === 'DRAG';
      return run(action('gesture', { name: $(drag ? 'patternDrag' : 'patternPause').value }));
    };
    $('hfAccept').onclick = () => run(action('train', { op: 'accept' }));
    $('hfCancel').onclick = () => run(action('train', { op: 'cancel' }));
    $('hfCommit').onclick = async () => {
      const result = await run(action('handsfree', { op: 'commit' }));
      if (result?.ok) toast('Hands-free setup saved. Resume control when ready.');
    };
    $('hfSwitchless').onchange = () =>
      run(action('handsfree', { op: 'switchless', enabled: $('hfSwitchless').checked }));
    $('hfHelperResume').onclick = () => run(action('resume'));
    $('hfLegacy').onclick = async () => {
      if (Date.now() - legacyArmedAt >= LEGACY_ARM_MS) {
        legacyArmedAt = Date.now();
        $('hfLegacy').textContent = 'Press again to confirm legacy mode';
        return;
      }
      legacyArmedAt = 0;
      const result = await run(action('handsfree', { op: 'legacy' }));
      if (result?.ok) toast('Legacy compatibility mode saved.');
    };
    $('hfDemo').onchange = () =>
      run(action('handsfree', { op: 'demo', enabled: $('hfDemo').checked }));
    $('hfEnableKind').onchange = () =>
      run(action('handsfree', { op: 'enable', kind: $('hfEnableKind').value }));
    $('enableSwitch').onchange = () =>
      run(action('enable', { enabled: $('enableSwitch').checked }));
    // The simulated push button: a press is held for at least 150 ms so that a quick click is
    // always seen by at least two polled steps (the firmware debounces for 30 ms).
    const press = (down) => {
      if (down) buttonHeld = true;
      else if (buttonHeld) {
        buttonHeld = false;
        buttonUntil = performance.now() + 150;
      }
      $('enableButton').classList.toggle('held', buttonHeld);
      $('enableButton').setAttribute('aria-pressed', String(buttonHeld));
    };
    $('enableButton').onpointerdown = (e) => {
      e.preventDefault();
      $('enableButton').setPointerCapture(e.pointerId);
      press(true);
    };
    $('enableButton').onpointerup = () => press(false);
    $('enableButton').onpointercancel = () => press(false);
    $('enableButton').onblur = () => press(false);
    $('enableButton').onkeydown = (e) => {
      if (e.key === ' ' || e.key === 'Enter') {
        e.preventDefault();
        press(true);
      }
    };
    $('enableButton').onkeyup = (e) => {
      if (e.key === ' ' || e.key === 'Enter') {
        e.preventDefault();
        press(false);
      }
    };
    $('gesturePause').onclick = () => run(action('gesture', { name: $('patternPause').value }));
    $('gestureDrag').onclick = () => run(action('gesture', { name: $('patternDrag').value }));
  }
  // Raw enable input for the next simulated step: the push button, or the maintained switch box.
  function enableInput(device) {
    return isButton(handsFreeOf(device))
      ? buttonHeld || performance.now() < buttonUntil
      : $('enableSwitch').checked;
  }
  return { render, bind, enableInput };
}
