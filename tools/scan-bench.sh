#!/usr/bin/env bash
# Prospero Disk - Builds and runs the host scan benchmark (host/scan_bench.cpp).
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: tools/scan-bench.sh <path> [threads]

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$root/build/scan-bench"
mkdir -p "$out"
cxx=${HOST_CXX:-$(command -v clang++-18 || command -v clang++)}
"$cxx" -std=c++20 -O2 -Wall -Wextra -I"$root/src" -pthread \
    "$root/host/scan_bench.cpp" "$root"/src/disk/scanner.cpp "$root"/src/disk/tree.cpp \
    "$root"/src/disk/posix_filesystem.cpp "$root"/src/disk/cache.cpp "$root"/src/disk/format.cpp \
    "$root"/src/core/save_file.cpp -o "$out/scan_bench"
exec "$out/scan_bench" "$@"
