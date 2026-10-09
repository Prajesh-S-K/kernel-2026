// Pure companion logic for the EXPERIMENTAL dwell action palette. No DOM access (unit tested).
// The palette is a separate companion window the user places beside the target application. It is NOT
// a system-wide overlay: it only exists while that window is open and visible.
export const ACTIONS_LABEL = 'EXPERIMENTAL DWELL ACTION PALETTE';
export const PALETTE_NOTE =
  'Prototype: this palette is a normal companion window. Keep it visible beside the application you ' +
  'are using; it is not a system-wide overlay and it is not on top of other windows.';
export const SCROLL_EXIT_TEXT = 'HOLD STILL TO EXIT SCROLLING';
export const SCROLL_NOTE =
  'Scroll: dwell on the content to begin; the pointer then freezes and vertical head movement scrolls. ' +
  `${SCROLL_EXIT_TEXT}: hold still for the dwell time (shown as a bar on the Exit control), because a ` +
  'frozen pointer cannot travel to a control. Pausing to read ALSO exits Scroll, so use a longer dwell ' +
  'for reading. The website Stop and the physical button always work.';
export const LINK_NOTE =
  'The palette tells the device where the pointer is. Without a fresh report nothing acts on a target. ' +
  'A report that is late by more than the dwell plus the 150 ms commit wait (about 1.6 s) cannot be ' +
  'detected, and a click could then reach the target. This is NOT a guarantee against click-through.';
const num = (value) => (Number.isFinite(Number(value)) ? Number(value) : 0);
const clamp01 = (value) => Math.min(1, Math.max(0, num(value)));

// id = the firmware target name, which is also what the page reports on hover.
export const CONTROLS = [
  { id: 'left', label: 'Left-click', hint: 'Default. One click, stays selected.' },
  { id: 'right', label: 'Right-click', hint: 'One right click, then back to Left-click.' },
  { id: 'double', label: 'Double-click', hint: 'One double-click, then back to Left-click.' },
  { id: 'drag', label: 'Drag', hint: 'Dwell to press and hold, move, dwell again to release.' },
  { id: 'scroll', label: 'Scroll', hint: 'Dwell on content, then move your head up or down.' },
  { id: 'stop', label: 'Stop', hint: 'Ends control. Also: website Stop, physical button.' },
];

export function actionsOf(device) {
  const a = device?.actions;
  return {
    reported: !!a && typeof a === 'object',
    enabled: a?.enabled === true,
    mode: typeof a?.mode === 'string' ? a.mode : 'LEFT',
    dragging: a?.dragging === true,
    scroll: typeof a?.scroll === 'string' ? a.scroll : 'OFF',
    frozen: a?.frozen === true,
    hover: typeof a?.hover === 'string' ? a.hover : 'NONE',
    inPalette: a?.inPalette === true,
    last: typeof a?.last === 'string' ? a.last : 'NONE',
    selections: num(a?.selections),
    dwellMs: num(a?.dwellMs),
    tolerance: num(a?.tolerance),
    neutral: num(a?.neutral),
    dwell: {
      state: typeof a?.dwell?.state === 'string' ? a.dwell.state : 'IDLE',
      progress: clamp01(a?.dwell?.progress),
    },
    exitProgress: clamp01(a?.exit?.progress),
    counts: {
      left: num(a?.counts?.left),
      right: num(a?.counts?.right),
      double: num(a?.counts?.double),
      dragStart: num(a?.counts?.dragStart),
      dragRelease: num(a?.counts?.dragRelease),
      scrollStart: num(a?.counts?.scrollStart),
      scrollExit: num(a?.counts?.scrollExit),
      wheel: num(a?.counts?.wheel),
    },
    link: {
      reporting: a?.link?.reporting === true,
      ageMs: num(a?.link?.ageMs),
      pending: a?.link?.pending === true,
      commitMs: num(a?.link?.commitMs),
      inhibited: num(a?.link?.inhibited),
      cancelled: num(a?.link?.cancelled),
      paletteReleases: num(a?.link?.paletteReleases),
    },
    blocked: typeof a?.blocked === 'string' ? a.blocked : '',
  };
}

const MODE_TEXT = {
  LEFT: 'LEFT-CLICK',
  RIGHT: 'RIGHT-CLICK (one shot)',
  DOUBLE: 'DOUBLE-CLICK (one shot)',
  DRAG: 'DRAG (dwell to press)',
  SCROLL: 'SCROLL (dwell on content)',
};

// What the big banner says. DRAGGING and SCROLLING are deliberately loud: a held button or a frozen
// pointer must never be a surprise.
export function bannerOf(device) {
  const a = actionsOf(device);
  if (!a.enabled)
    return { kind: 'off', text: 'ACTION PALETTE OFF', detail: a.blocked || 'Enable it first.' };
  if (a.dragging)
    return {
      kind: 'dragging',
      text: 'DRAGGING',
      detail: 'The left button is held down. Move, then dwell again to release.',
    };
  if (a.scroll === 'ACTIVE')
    return {
      kind: 'scrolling',
      text: 'SCROLLING',
      detail: 'Pointer frozen. Move your head up or down. Pausing to read also exits.',
      exitHint: SCROLL_EXIT_TEXT,
    };
  if (!a.link.reporting)
    return {
      kind: 'noreport',
      text: 'PALETTE WINDOW NOT REPORTING',
      detail:
        'Nothing will click until the palette window is open and reporting. Open it beside the target.',
    };
  if (a.scroll === 'ARMED')
    return {
      kind: 'armed',
      text: 'SCROLL ARMED',
      detail: 'Move over the content and hold still to start scrolling.',
    };
  return { kind: 'ready', text: MODE_TEXT[a.mode] ?? a.mode, detail: '' };
}

// The six palette controls with their live state. While scrolling, the Scroll control becomes the Exit
// control and shows the exit progress instead of the selection dwell.
export function controlsOf(device) {
  const a = actionsOf(device);
  const hovered = a.enabled ? a.hover.toLowerCase() : 'none';
  return CONTROLS.map((control) => {
    const isScrollExit = a.enabled && a.scroll === 'ACTIVE' && control.id === 'scroll';
    const selected = a.enabled && control.id !== 'stop' && a.mode.toLowerCase() === control.id;
    const isHovered = hovered === control.id;
    return {
      ...control,
      label: isScrollExit ? 'Exit Scroll' : control.label,
      hint: isScrollExit ? 'Hold still for the dwell time to leave Scroll.' : control.hint,
      selected,
      hovered: isHovered,
      exit: isScrollExit,
      progress: isScrollExit ? a.exitProgress : isHovered ? a.dwell.progress : 0,
      active: control.id === 'drag' ? a.dragging : isScrollExit,
    };
  });
}

export function statsLine(device) {
  const a = actionsOf(device);
  const c = a.counts;
  return (
    `Dwell ${a.dwellMs} ms, tolerance ${a.tolerance} · left ${c.left}, right ${c.right}, ` +
    `double ${c.double}, drag ${c.dragStart}/${c.dragRelease} (press/release), scroll ${c.scrollStart} started, ` +
    `${c.scrollExit} exited, ${c.wheel} wheel notches · palette selections ${a.selections}`
  );
}

export function panelView(device) {
  const a = actionsOf(device);
  const hardware = !!device && device.source !== 'SIMULATED';
  const canEnable = hardware && a.reported && a.blocked === '';
  const link = a.enabled
    ? a.link.reporting
      ? `Palette report age ${a.link.ageMs} ms · target actions inhibited ${a.link.inhibited}× · cancelled by a late palette entry ${a.link.cancelled}× · drags released on palette entry ${a.link.paletteReleases}×`
      : 'NO fresh palette report: nothing acts on a target until the palette window reports.'
    : '';
  return {
    hardware,
    reported: a.reported,
    enabled: a.enabled,
    canEnable,
    enableBlocked: a.enabled ? '' : a.blocked,
    banner: bannerOf(device),
    link,
    stats:
      a.enabled || a.counts.left + a.counts.right + a.counts.double + a.counts.dragStart > 0
        ? statsLine(device)
        : '',
    a,
  };
}

// The page reports which palette control the pointer is over. It refreshes while hovering so the
// device can expire a stale report (a closed window never leaves the palette "occupied"), and always
// reports leaving. `send(target)` is the transport; `now` values are milliseconds.
export class HoverReporter {
  // The page repeats its state: every refreshMs while over a control, every idleMs while outside. The
  // device treats a report older than 2.5 s as missing and then acts on nothing.
  constructor(send, refreshMs = 500, idleMs = 1000) {
    this.send = send;
    this.refreshMs = refreshMs;
    this.idleMs = idleMs;
    this.current = 'none';
    this.sentAt = -Infinity;
  }
  kick() {
    this.sentAt = -Infinity; // report again at the next tick (the palette was just enabled)
  }
  enter(target, now) {
    if (this.current === target) return;
    this.current = target;
    this.sentAt = now;
    this.send(target);
  }
  leave(target, now) {
    if (this.current !== target) return; // leaving a control we are no longer on changes nothing
    this.current = 'none';
    this.sentAt = now;
    this.send('none');
  }
  reset(now) {
    if (this.current === 'none') return;
    this.current = 'none';
    this.sentAt = now;
    this.send('none');
  }
  tick(now) {
    const every = this.current === 'none' ? this.idleMs : this.refreshMs;
    if (now - this.sentAt >= every) {
      this.sentAt = now;
      this.send(this.current);
    }
  }
}

// Round-trip time of the palette's own reports (browser -> companion -> device -> acknowledgement), so the
// real delay is measured in use rather than assumed. A failed or refused report counts separately.
export class LatencyStats {
  constructor(keep = 200) {
    this.keep = keep;
    this.samples = [];
    this.failed = 0;
    this.total = 0;
  }
  add(ms) {
    this.total += 1;
    this.samples.push(ms);
    if (this.samples.length > this.keep) this.samples.shift();
  }
  fail() {
    this.total += 1;
    this.failed += 1;
  }
  summary() {
    const sorted = [...this.samples].sort((x, y) => x - y);
    const at = (p) =>
      sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0;
    return {
      count: this.total,
      failed: this.failed,
      last: this.samples.length ? this.samples[this.samples.length - 1] : 0,
      median: at(0.5),
      p95: at(0.95),
      worst: sorted.length ? sorted[sorted.length - 1] : 0,
    };
  }
  line() {
    const s = this.summary();
    if (s.count === 0) return 'Link: no reports yet.';
    const risky = s.worst > 1000 || s.failed > 0;
    return (
      `Link: last ${Math.round(s.last)} ms · median ${Math.round(s.median)} · 95% ${Math.round(s.p95)} · ` +
      `worst ${Math.round(s.worst)} ms · lost ${s.failed} of ${s.count}` +
      (risky
        ? ' — slow or lost reports: a click could reach the target (limit about 1600 ms).'
        : '')
    );
  }
}
