#!/usr/bin/env bash
# Prospero Disk - Fetches the pinned SQLite amalgamation into .deps/.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Read-only lookups of title names in the console's app.db (docs/PRD.md 5.4);
# src/third_party/sqlite/sqlite.c compiles it. sqlite.org publishes SHA3
# sums; the SHA-256 below was taken from this archive on 2026-11-17.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=3460100
sha256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
url="https://www.sqlite.org/2024/sqlite-amalgamation-$version.zip"
target="$root/.deps/sqlite"

if [[ ! -f $target/sqlite3.c || $(cat "$target/VERSION" 2>/dev/null) != "$version" ]]; then
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    printf '==> [sqlite] Downloading SQLite %s\n' "$version" >&2
    curl -fsSL --retry 3 -o "$work/sqlite.zip" "$url"
    echo "$sha256  $work/sqlite.zip" | sha256sum --check --status || {
        echo "SQLite archive hash mismatch" >&2
        exit 1
    }
    unzip -q "$work/sqlite.zip" -d "$work"
    rm -rf "$target"
    mkdir -p "$target"
    cp "$work/sqlite-amalgamation-$version/sqlite3.c" "$work/sqlite-amalgamation-$version/sqlite3.h" "$target/"
    echo "$version" > "$target/VERSION"
fi
echo "$target"
