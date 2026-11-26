#!/usr/bin/env bash
# Prospero Disk - Builds the one-shot title remover payload (tools/title-remover) that the app ships.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: tools/build-title-remover.sh <output.elf>
#
# Compiled with the payload SDK that builds the Lapy helper
# (tools/build-lapy-helper.py fetches it into .deps/lapy), by relative name
# from its own folder, so the ELF does not depend on the checkout path.
# PS5_PAYLOAD_SDK names another SDK.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:?usage: tools/build-title-remover.sh <output.elf>}
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/lapy/ps5-payload-sdk-v0.42}
[[ -x $sdk/bin/prospero-clang ]] || {
    echo "build-title-remover: no payload SDK at $sdk (run tools/build-lapy-helper.py first)" >&2
    exit 1
}
mkdir -p "$(dirname -- "$out")"
out=$(cd -- "$(dirname -- "$out")" && pwd)/$(basename -- "$out")
cd "$root/tools/title-remover"
PS5_PAYLOAD_SDK=$sdk "$sdk/bin/prospero-clang" -Wall -Werror -O2 -ffile-prefix-map="$root"=. \
    -o "$out" main.c -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService
