#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
# Save logs only after a successful check; failed runs cannot create a gate stamp.
rm -f build/verification.json
gate_directory=$(mktemp -d)
trap 'rm -rf "$gate_directory"' EXIT HUP INT TERM
sh scripts/check.sh > "$gate_directory/software.log" 2>&1 || { cat "$gate_directory/software.log"; exit 1; }
cat "$gate_directory/software.log"
pio run -e esp32s3 -e esp32s3-sim > "$gate_directory/firmware.log" 2>&1 || { cat "$gate_directory/firmware.log"; exit 1; }
cat "$gate_directory/firmware.log"
cp "$gate_directory/software.log" evidence/software-checks.log
cp "$gate_directory/firmware.log" evidence/firmware-build.log
./build/nodx_tests > evidence/control-tests.log
python3 scripts/release.py
