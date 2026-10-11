#!/usr/bin/env bash
# Launch the NodX desktop overlay (macOS only). Creates a SEPARATE virtual environment for the native UI
# dependencies on first use, so the firmware and browser-companion requirements are never touched.
#
#   scripts/run_overlay.sh                  start the overlay (the companion must already be running)
#   scripts/run_overlay.sh --edge left      first position on the left edge
#   scripts/run_overlay.sh --selftest DIR   check the panel's safety properties off screen, render PNGs to DIR
set -euo pipefail
cd "$(dirname "$0")/.."
VENV="${NODX_OVERLAY_VENV:-.venv-overlay}"
if [ "$(uname -s)" != "Darwin" ]; then
  echo "The NodX overlay is macOS only." >&2
  exit 2
fi
if [ ! -x "$VENV/bin/python" ]; then
  PY="${NODX_PYTHON:-$(command -v python3.14 || command -v python3.13 || command -v python3.12 || command -v python3)}"
  echo "Creating $VENV with $PY (one time)..."
  "$PY" -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --disable-pip-version-check -r overlay/requirements.txt
fi
exec "$VENV/bin/python" -m overlay "$@"
