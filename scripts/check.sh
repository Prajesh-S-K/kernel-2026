#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
cmake -S . -B build -DNODX_SANITIZE=ON -DNODX_ASAN=OFF
cmake --build build -j 4
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
node --test tests/metrics.test.mjs
