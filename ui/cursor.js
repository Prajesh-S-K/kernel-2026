export function drawCursor(cursor, label, point, rect, hidden, state, progress) {
  cursor.hidden = hidden;
  if (hidden) return;
  cursor.style.left = `${rect.left + point.x}px`;
  cursor.style.top = `${rect.top + point.y}px`;
  cursor.className = `nodx-cursor ${state}`;
  label.textContent = state.replaceAll('_', ' ');
  cursor.querySelector('.dwell-ring').style.strokeDashoffset = 126 * (1 - progress);
}
