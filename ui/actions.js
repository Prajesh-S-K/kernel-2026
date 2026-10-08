// Pure companion logic for the EXPERIMENTAL dwell action palette. No DOM access (unit tested).
// The palette is a separate companion window the user places beside the target application. It is NOT
// a system-wide overlay: it only exists while that window is open and visible.
export const ACTIONS_LABEL = 'EXPERIMENTAL DWELL ACTION PALETTE';
export const PALETTE_NOTE =
  'Prototype: this palette is a normal companion window. Keep it visible beside the application you ' +
  'are using; it is not a system-wide overlay and it is not on top of other windows.';
export const SCROLL_NOTE =
  'Scroll: dwell on the content to begin; the pointer then freezes and vertical head movement scrolls. ' +
  'To leave Scroll, hold still for the dwell time (shown as a bar on the Exit control), because a ' +
  'frozen pointer cannot travel to a control. The website Stop and the physical button always work.';
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
      detail: 'Pointer frozen. Move your head up or down. Hold still to exit.',
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
  return {
    hardware,
    reported: a.reported,
    enabled: a.enabled,
    canEnable,
    enableBlocked: a.enabled ? '' : a.blocked,
    banner: bannerOf(device),
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
  constructor(send, refreshMs = 500) {
    this.send = send;
    this.refreshMs = refreshMs;
    this.current = 'none';
    this.sentAt = -Infinity;
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
    if (this.current !== 'none' && now - this.sentAt >= this.refreshMs) {
      this.sentAt = now;
      this.send(this.current);
    }
  }
}
