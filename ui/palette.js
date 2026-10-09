// The action palette window. It shows the device's palette state (relayed by the main companion
// window, so there is a single status poll) and reports which control the pointer is over. Selection
// itself is done by the device's dwell: this page has no click handlers on purpose, so an operating
// system click that reaches it can never select anything.
import {
  HoverReporter,
  LINK_NOTE,
  LatencyStats,
  PALETTE_NOTE,
  bannerOf,
  controlsOf,
} from './actions.js';

const $ = (id) => document.getElementById(id);
const channel = typeof BroadcastChannel === 'function' ? new BroadcastChannel('nodx-status') : null;
let device = null,
  lastData = 0;
let enabled = false;

const stats = new LatencyStats();
// Every report is timed from the browser to the device's acknowledgement, so the real delay is measured.
const send = (target) => {
  if (!enabled) return;
  const started = performance.now();
  fetch('/api/device', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action: 'actions', op: 'hover', target }),
  })
    .then((response) => response.json())
    .then((reply) => (reply?.ok ? stats.add(performance.now() - started) : stats.fail()))
    .catch(() => stats.fail());
};
const reporter = new HoverReporter(send);

const buttons = new Map();
for (const control of controlsOf(null)) {
  const element = document.createElement('div');
  element.className = `control ${control.id}`;
  element.dataset.id = control.id;
  element.setAttribute('role', 'img');
  element.innerHTML = '<strong></strong><small></small><span class="fill"></span>';
  element.addEventListener('mouseenter', () => reporter.enter(control.id, performance.now()));
  // leaving a control onto the window background is still "on the palette"
  element.addEventListener('mouseleave', () => reporter.enter('frame', performance.now()));
  $('grid').append(element);
  buttons.set(control.id, element);
}
// The whole page is the palette: anywhere on it inhibits target actions. Only a real exit from the
// page (or the page being hidden or closed) reports "outside". There is deliberately NO reset on window
// blur: a window can lose focus while the pointer is still over it.
document.documentElement.addEventListener('mouseenter', () =>
  reporter.enter('frame', performance.now()),
);
document.documentElement.addEventListener('mouseleave', () => reporter.reset(performance.now()));
// A window that opens under a resting pointer sends no enter event: any movement on the page counts.
document.addEventListener('mousemove', () => {
  if (reporter.current === 'none') reporter.enter('frame', performance.now());
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) reporter.reset(performance.now());
});
window.addEventListener('pagehide', () => reporter.reset(performance.now()));

function render() {
  const stale = !device || Date.now() - lastData > 2500;
  const banner = stale
    ? { kind: 'off', text: 'NO DATA', detail: 'Keep the main companion window open and connected.' }
    : bannerOf(device);
  const nowEnabled = !stale && device?.actions?.enabled === true;
  if (nowEnabled && !enabled) reporter.kick(); // just enabled: report at once
  enabled = nowEnabled;
  $('banner').className = `banner ${banner.kind}`;
  $('bannerText').textContent = banner.text;
  $('bannerDetail').textContent = banner.detail;
  const scrolling = !stale && !!banner.exitHint;
  $('exit').hidden = !scrolling;
  if (scrolling) {
    $('exitText').textContent = banner.exitHint;
    $('exitBar').style.width = `${Math.round((device.actions.exit?.progress ?? 0) * 100)}%`;
  }
  for (const control of controlsOf(stale ? null : device)) {
    const element = buttons.get(control.id);
    element.className = `control ${control.id}${control.selected ? ' selected' : ''}${
      control.hovered ? ' hovered' : ''
    }${control.active ? ' active' : ''}${control.exit ? ' exit' : ''}`;
    element.children[0].textContent = control.label;
    element.children[1].textContent = control.hint;
    element.children[2].style.width = `${Math.round(control.progress * 100)}%`;
    element.setAttribute('aria-label', `${control.label}. ${control.hint}`);
  }
  $('link').textContent = stats.line();
  $('note').textContent = PALETTE_NOTE;
  $('limits').textContent = LINK_NOTE;
}
channel?.addEventListener('message', (event) => {
  device = event.data;
  lastData = Date.now();
  render();
});
setInterval(() => {
  reporter.tick(performance.now());
  render();
}, 200);
render();
