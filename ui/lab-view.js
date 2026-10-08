import { summarize, csv } from './metrics.js';
import { freezeContext, invalidates, groupBlocks, comparisonSettings } from './lab.js';
import { handsFreeOf } from './handsfree.js';

export function createPerformanceLab({
  $,
  request,
  action,
  toast,
  renderCursor,
  getDevice,
  getPoint,
  setPoint,
  download,
  geometry,
}) {
  let starting = false,
    generation = 0;
  let lab = null,
    comparison = null;
  const rows = [];
  const sessionId = crypto.randomUUID();
  function select(x, y) {
    if (!lab) return;
    const hit = Math.hypot(x - lab.target.x, y - lab.target.y) <= lab.target.width / 2;
    if (lab.waitingStart) {
      if (hit) {
        lab.waitingStart = false;
        lab.last = { x, y };
        nextTarget();
      } else toast('Select the center starting target first.');
      return;
    }
    logAttempt(x, y, hit, false);
    lab.last = { x, y };
    lab.index++;
    if (lab.index >= 12) finishLab();
    else nextTarget();
  }
  function placeTarget(target) {
    const el = $('labTarget');
    el.hidden = false;
    el.style.width = target.width + 'px';
    el.style.height = target.width + 'px';
    el.style.left = target.x - target.width / 2 + 'px';
    el.style.top = target.y - target.width / 2 + 'px';
  }
  function nextTarget() {
    const bounds = $('arena').getBoundingClientRect();
    const i = lab.index;
    const angle = i * Math.PI * 0.75;
    const radius = Math.min(bounds.width * 0.32, bounds.height * 0.32) * (i % 2 ? 0.85 : 1);
    lab.target = {
      x: bounds.width / 2 + Math.cos(angle) * radius,
      y: bounds.height / 2 + Math.sin(angle) * radius,
      width: [32, 56, 80][i % 3],
    };
    lab.distance = Math.hypot(lab.target.x - lab.last.x, lab.target.y - lab.last.y);
    lab.started = performance.now();
    lab.startWall = new Date().toISOString();
    lab.startDevice = getDevice()?.timeMs || 0;
    lab.cancelStart = getDevice()?.cancellations || 0;
    lab.gestureStart = handsFreeOf(getDevice()).gesture.candidates;
    placeTarget(lab.target);
    $('trialLabel').textContent = `TRIAL ${i + 1} / 12 · ${lab.condition} · ${lab.source}`;
  }
  function logAttempt(x, y, hit, aborted, reason = '') {
    const time = Math.max(0.001, performance.now() - lab.started);
    const row = {
      sessionId,
      blockId: lab.id,
      trial: lab.index + 1,
      condition: lab.condition,
      inputSource: lab.source,
      deviceSource: lab.context.deviceSource,
      selectionMethod: lab.context.selectionMethod,
      interactionMode: lab.context.interactionMode,
      gestureConfigId: lab.context.gestureConfigId,
      blockContext: lab.context,
      targetX: lab.target.x,
      targetY: lab.target.y,
      width: lab.target.width,
      startX: lab.last.x,
      startY: lab.last.y,
      endX: x,
      endY: y,
      distance: lab.distance,
      startTime: lab.startWall,
      selectionTime: new Date().toISOString(),
      movementTimeMs: time,
      nominalId: Math.log2(lab.distance / lab.target.width + 1),
      hit,
      aborted,
      abortReason: reason,
      deviceStartMs: lab.startDevice,
      deviceEndMs: getDevice()?.timeMs || 0,
      dwellCancellations: (getDevice()?.cancellations || 0) - lab.cancelStart,
      // Recognition candidates opened during this attempt (each suppresses output while open).
      gestureInterruptions: handsFreeOf(getDevice()).gesture.candidates - lab.gestureStart,
      profile: structuredClone(lab.profile),
      viewportWidth: lab.context.geometry.width,
      viewportHeight: lab.context.geometry.height,
      devicePixelRatio: lab.context.geometry.devicePixelRatio,
      softwareVersion: '0.2.0',
    };
    rows.push(row);
    row.persistence = request(row, '/api/trial')
      .then((r) => {
        row.profileHash = r.profileHash;
        row.persisted = true;
      })
      .catch((e) => {
        row.persisted = false;
        toast('Trial is in CSV memory; disk logging failed: ' + e.message);
      });
    renderMetrics();
  }
  async function startLab() {
    if (lab || starting) return;
    if (!getDevice()?.hasProfile) {
      toast('Calibrate and save a valid profile first.');
      return;
    }
    starting = true;
    const ticket = ++generation;
    $('startLab').disabled = true;
    try {
      const condition = $('condition').value,
        source = $('inputSource').value;
      comparison ||= comparisonSettings(getDevice().profile);
      let result = await action(condition === 'GENERIC' ? 'generic' : 'load');
      if (ticket !== generation) {
        await action('pause');
        return;
      }
      if (!result?.ok) {
        toast('Calibrate and save an adaptive profile first.');
        return;
      }
      result = await action('settings', comparison);
      if (ticket !== generation) {
        await action('pause');
        return;
      }
      if (!result?.ok) return;
      if (source === 'SIMULATED' || result.source !== 'SIMULATED') {
        result = await action('resume');
        if (!result?.ok) return;
      }
      if (ticket !== generation) {
        await action('pause');
        return;
      }
      lab = {
        id: crypto.randomUUID(),
        index: 0,
        waitingStart: true,
        condition,
        source,
        profile: structuredClone(result.profile),
        context: freezeContext(result, source, condition, geometry()),
        baseline: { executed: handsFreeOf(result).gesture.executed },
      };
      const box = $('arena').getBoundingClientRect();
      lab.target = { x: box.width / 2, y: box.height / 2, width: 48 };
      setPoint({ x: box.width / 2, y: box.height / 2 });
      placeTarget(lab.target);
      $('labWelcome').hidden = true;
      $('trialLabel').textContent = 'SELECT THE CENTER TARGET TO BEGIN';
      $('labStatus').textContent = source;
      $('startLab').disabled = true;
      $('stopLab').disabled = false;
      for (const id of ['condition', 'inputSource', 'calibrate', 'load', 'dwell', 'scroll'])
        $(id).disabled = true;
      $('arena').classList.toggle('host', source === 'HOST_POINTER');
      renderCursor();
    } finally {
      starting = false;
      $('startLab').disabled = !!lab;
    }
  }
  function finishLab() {
    lab = null;
    action('pause');
    $('labTarget').hidden = true;
    $('labWelcome').hidden = false;
    $('startLab').disabled = false;
    $('stopLab').disabled = true;
    for (const id of ['condition', 'inputSource', 'calibrate', 'load', 'dwell', 'scroll'])
      $(id).disabled = false;
    $('labStatus').textContent = 'BLOCK COMPLETE';
    renderMetrics();
  }
  function abortLab(reason = 'user abort') {
    if (starting) {
      ++generation;
      starting = false;
      $('startLab').disabled = false;
      action('pause');
    }
    if (!lab) return;
    if (!lab.waitingStart) logAttempt(getPoint().x, getPoint().y, false, true, reason);
    finishLab();
    $('labStatus').textContent = 'ABORTED';
  }
  function renderMetrics() {
    const node = $('metrics');
    node.replaceChildren();
    for (const block of groupBlocks(rows)) {
      const subset = block,
        m = summarize(subset),
        condition = block[0].condition;
      const card = document.createElement('div');
      card.className = 'metric-card';
      const header = document.createElement('h3');
      header.textContent =
        condition +
        ' / ' +
        block[0].deviceSource +
        ' / ' +
        block[0].selectionMethod +
        ' / ' +
        (block[0].interactionMode === 'HANDS_FREE'
          ? 'HANDS-FREE ' + block[0].gestureConfigId
          : 'LEGACY SWITCH') +
        ' / BLOCK ' +
        block[0].blockId.slice(0, 8);
      card.append(header);
      const fields = [
        ['Attempts', m.attempts],
        ['Hit rate', m.accuracy === null ? '—' : (100 * m.accuracy).toFixed(1) + '%'],
        [
          'Mean selection time',
          m.meanTimeMs === null ? '—' : (m.meanTimeMs / 1000).toFixed(3) + ' s',
        ],
        ['Misses', m.errors],
        [
          'Nominal successful ID / time',
          m.nominalRate === null ? '—' : m.nominalRate.toFixed(2) + ' bits/s',
        ],
      ];
      for (const [label, value] of fields) {
        const row = document.createElement('div');
        row.className = 'metric-row';
        const l = document.createElement('span'),
          v = document.createElement('b');
        l.textContent = label;
        v.textContent = value;
        row.append(l, v);
        card.append(row);
      }
      node.append(card);
    }
  }

  return {
    get running() {
      return !!lab || starting;
    },
    start: startLab,
    abort: abortLab,
    select,
    renderMetrics,
    resetComparison() {
      comparison = null;
    },
    check(device) {
      if (!lab) return;
      const reason = invalidates(lab.context, device, geometry(), lab.baseline);
      if (reason) abortLab(reason);
    },
    async exportTrials() {
      if (!rows.length) {
        toast('No raw trials yet.');
        return;
      }
      await Promise.all(rows.map((row) => row.persistence));
      const exported = rows.map(({ persistence: _persistence, ...row }) => row);
      download('nodx-trials.csv', csv(exported), 'text/csv');
    },
  };
}
