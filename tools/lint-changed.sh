#!/usr/bin/env bash
# Prospero Disk - clang-format and clang-tidy on the C/C++ files that differ from a git revision.
# Copyright (C) 2026 Leonardo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `make lint` analyses the whole tree (about 25 minutes on 4 cores). While you
# work, check only what you changed, with the same flags as
# tools/run_clang_tidy.sh; run `make lint` once before a release.
#
#   tools/lint-changed.sh            # changes against HEAD, staged or not
#   tools/lint-changed.sh develop    # changes against another revision

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
base=${1:-HEAD}
tidy=${CLANG_TIDY:-$(command -v clang-tidy-18 || command -v clang-tidy)}
format=${CLANG_FORMAT:-$(command -v clang-format-18 || command -v clang-format)}
jobs=${TIDY_JOBS:-$(nproc)}

mapfile -t changed < <({
    git diff --name-only --diff-filter=d "$base" -- src tests tooling/native
    git ls-files --others --exclude-standard -- src tests tooling/native
} | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '^src/third_party/' | sort -u)
if (( ${#changed[@]} == 0 )); then
    echo "No C/C++ changes against $base."
    exit 0
fi

"$format" --dry-run --Werror "${changed[@]}"
echo "format ok: ${#changed[@]} files"

sdk="$root/.deps/native/ps5-payload-sdk"
harfbuzz=$(bash tools/fetch-harfbuzz.sh)
bash tools/setup-native-dependencies.sh >/dev/null
bash tools/prepare-opengl.sh >/dev/null
app_flags=(-std=c++20 -fno-exceptions -fno-rtti --target=x86_64-sie-ps5 -DGL_GLEXT_PROTOTYPES=1
    -I"$root/src" -isystem "$root/.deps/ps5-opengl/current/include" -isystem "$harfbuzz"
    -isystem "$sdk/target/include/c++/v1" -isystem "$sdk/target/include")
gtest=$(bash tools/setup-test-dependencies.sh)
test_flags=(-std=c++20 "-DHUI_SOURCE_DIR=\"$root\"" -I"$root/src" -I"$root/tests"
    -isystem "$gtest/googletest/include" -isystem "$harfbuzz")

# A changed header is checked through every source that includes it.
sources=("${changed[@]}")
for file in "${changed[@]}"; do
    case $file in
        *.h | *.hpp)
            mapfile -t users < <(grep -rlF --include='*.c' --include='*.cc' --include='*.cpp' \
                "$(basename "$file")\"" src tests tooling/native | grep -v '^src/third_party/' || true)
            sources+=("${users[@]}") ;;
    esac
done
mapfile -t sources < <(printf '%s\n' "${sources[@]}" | sort -u)

zlib="$root/.deps/native/zlib/root/usr/include"
app=() tests=() c=() host=()
for file in "${sources[@]}"; do
    case $file in
        tests/*.cpp) tests+=("$file") ;;
        src/*.cpp | src/*.cc | tooling/native/app_crt.cpp | tooling/native/app_cpp_runtime.cpp)
            app+=("$file") ;;
        tooling/native/*.cpp) host+=("$file") ;;
        src/*.c) c+=("$file") ;;
    esac
done
run() {
    local -n group=$1
    shift
    (( ${#group[@]} )) || return 0
    printf '%s\0' "${group[@]}" |
        xargs -0 -P "$jobs" -I{} "$tidy" {} --quiet --warnings-as-errors='*' -- "$@"
}
run app "${app_flags[@]}"
run tests "${test_flags[@]}"
run c -std=c11 -isystem "$sdk/target/include"
run host -std=c++20 -I"$zlib"
echo "clang-tidy ok: $(( ${#app[@]} + ${#tests[@]} + ${#c[@]} + ${#host[@]} )) files (changed headers through the sources that include them)"
