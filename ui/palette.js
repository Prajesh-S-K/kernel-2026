// The action palette window. It shows the device's palette state (relayed by the main companion
// window, so there is a single status poll) and reports which control the pointer is over. Selection
// itself is done by the device's dwell: this page has no click handlers on purpose, so an operating
// system click that reaches it can never select anything.
import { HoverReporter, PALETTE_NOTE, bannerOf, controlsOf } from './actions.js';

const $ = (id) => document.getElementById(id);
const channel = typeof BroadcastChannel === 'function' ? new BroadcastChannel('nodx-status') : null;
let device = null,
  lastData = 0;
let enabled = false;

const send = (target) => {
  if (!enabled && target !== 'none') return;
  fetch('/api/device', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action: 'actions', op: 'hover', target }),
  }).catch(() => {});
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
  element.addEventListener('mouseleave', () => reporter.leave(control.id, performance.now()));
  $('grid').append(element);
  buttons.set(control.id, element);
}
// Leaving the window, hiding it or losing the page always reports "left the palette".
document.addEventListener('mouseleave', () => reporter.reset(performance.now()));
window.addEventListener('blur', () => reporter.reset(performance.now()));
document.addEventListener('visibilitychange', () => {
  if (document.hidden) reporter.reset(performance.now());
});
window.addEventListener('pagehide', () => reporter.reset(performance.now()));

function render() {
  const stale = !device || Date.now() - lastData > 2500;
  const banner = stale
    ? { kind: 'off', text: 'NO DATA', detail: 'Keep the main companion window open and connected.' }
    : bannerOf(device);
  enabled = !stale && device?.actions?.enabled === true;
  if (!enabled) reporter.reset(performance.now());
  $('banner').className = `banner ${banner.kind}`;
  $('bannerText').textContent = banner.text;
  $('bannerDetail').textContent = banner.detail;
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
  $('note').textContent = PALETTE_NOTE;
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
