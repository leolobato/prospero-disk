#!/usr/bin/env bash
# Prospero Disk - Fetches the pinned HarfBuzz release (text shaping) into .deps/.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The same release ProsperoEden pins (tools/deps.json there). Only src/ and
# COPYING are kept; src/third_party/harfbuzz/harfbuzz.cpp compiles it as one
# unit. Prints the include directory.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=12.3.2
sha256=6f6db164359a2da5a84ef826615b448b33e6306067ad829d85d5b0bf936f1bb8
url="https://github.com/harfbuzz/harfbuzz/releases/download/$version/harfbuzz-$version.tar.xz"
target="$root/.deps/harfbuzz-$version"

if [[ ! -f $target/src/harfbuzz.cc ]]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    printf '==> [harfbuzz] Downloading HarfBuzz %s\n' "$version" >&2
    curl -fsSL --retry 3 -o "$work/harfbuzz.tar.xz" "$url"
    echo "$sha256  $work/harfbuzz.tar.xz" | sha256sum --check --status || {
        echo "HarfBuzz archive hash mismatch" >&2
        exit 1
    }
    tar -xJf "$work/harfbuzz.tar.xz" -C "$work"
    rm -rf "$target"
    mkdir -p "$target"
    cp -a "$work/harfbuzz-$version/src" "$target/src"
    cp "$work/harfbuzz-$version/COPYING" "$target/COPYING"
fi
echo "$target/src"
