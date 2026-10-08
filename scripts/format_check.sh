#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
clang-format --dry-run --Werror core/include/nodx/*.hpp core/src/*.cpp desktop/*.cpp firmware/src/*.cpp firmware/src/*.hpp tests/*.cpp tests/*.hpp
ruff check .
ruff format --check .
npm --prefix ui run format:check
npm --prefix ui run lint
