#!/usr/bin/env python3
# ps5-native-app-boilerplate - Build the pinned Lapy one-shot helper (Playden's 13.60 pin).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch, build, and verify upstream Lapy's exact-title elfldr helper."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".deps/lapy"
# The helper Playden ships on firmware 13.60 (docs/PRD.md 7.2): the
# leolobato/PS5-Lapy-JB-Daemon fork at its owned_title_revision, built with the
# Payload SDK and ps5log client Playden pins. The earlier boilerplate pin
# (mpereiraesaa 54a095c, SDK v0.40) failed its exchange on 13.60.
LAPY_REPOSITORY = "https://github.com/leolobato/PS5-Lapy-JB-Daemon.git"
LAPY_COMMIT = "66ab4f10f9a6c6a5a42d05f1d165230c13ae2ab7"
LAPY = CACHE / f"PS5-Lapy-JB-Daemon-{LAPY_COMMIT[:7]}"
SDK_VERSION = "v0.42"
SDK = CACHE / f"ps5-payload-sdk-{SDK_VERSION}"
SDK_URL = f"https://github.com/ps5-payload-dev/sdk/releases/download/{SDK_VERSION}/ps5-payload-sdk.zip"
SDK_SHA256 = "8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da"
PS5LOG_COMMIT = "d9c00befa0056e7231db29ecc1bd5651303ac4b6"
PS5LOG = CACHE / f"ps5log-{PS5LOG_COMMIT[:8]}"
PS5LOG_URL = f"https://raw.githubusercontent.com/leolobato/prospero-win/{PS5LOG_COMMIT}/native/ps5log/"
PS5LOG_FILES = {
    "ps5log.c": "4ac6dc819ce48bcc4a613f119a53d7cbb5fa7974a77436ccb19b18b4fcf800d2",
    "ps5log.h": "05373177e2c1cd348848014139646e8b35306da673ff5c0a9089c1247eda394f",
    "ps5log_ps5_net.c": "d6ea249349bf2a085efa90adfd55f23f2ec9c10cb3df49bbdb8f26bf62fa37d6",
    "ps5log_ps5_net.h": "90693d535a033991dd201e81ef576eafb0edac55ca7c8e2b329e643b50458637",
}
PROTOCOL_SHA256 = "bb02c4aa814eaba7a7a423a31b29ff41f786212c2953678cf29434e85fa0f869"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url, path, expected):
    if path.is_file() and digest(path) == expected:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".download")
    request = urllib.request.Request(url, headers={"User-Agent": "ps5-native-app-boilerplate"})
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output)
    actual = digest(temporary)
    if actual != expected:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"{url}: SHA-256 {actual}, expected {expected}")
    temporary.replace(path)


def fetch_lapy():
    if LAPY.exists():
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LAPY, text=True).strip()
        if head != LAPY_COMMIT:
            raise RuntimeError(f"{LAPY} is at {head}, expected {LAPY_COMMIT}")
        if subprocess.check_output(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=LAPY,
            text=True,
        ):
            raise RuntimeError(f"{LAPY} has local changes; refusing to build privileged code")
        return
    CACHE.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="lapy-git-", dir=CACHE))
    try:
        subprocess.run(["git", "init", "-q"], cwd=staging, check=True)
        subprocess.run(["git", "remote", "add", "origin",
                        LAPY_REPOSITORY],
                       cwd=staging, check=True)
        subprocess.run(["git", "fetch", "-q", "--depth", "1", "origin", LAPY_COMMIT],
                       cwd=staging, check=True)
        subprocess.run(["git", "-c", "advice.detachedHead=false", "checkout", "-q", "--detach",
                        "FETCH_HEAD"], cwd=staging, check=True)
        staging.rename(LAPY)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def fetch_sdk():
    stamp = SDK / ".archive-sha256"
    if (SDK / "bin/prospero-clang").is_file() and stamp.is_file() and \
            stamp.read_text().strip() == SDK_SHA256:
        return
    if SDK.exists():
        raise RuntimeError(f"{SDK} is incomplete or does not match the pinned archive")
    archive = CACHE / f"ps5-payload-sdk-{SDK_VERSION}.zip"
    download(SDK_URL, archive, SDK_SHA256)
    staging = Path(tempfile.mkdtemp(prefix="lapy-sdk-", dir=CACHE))
    try:
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                parts = Path(member.filename).parts
                if not parts or parts[0] != "ps5-payload-sdk" or ".." in parts:
                    raise RuntimeError(f"unexpected SDK archive path: {member.filename}")
                relative = parts[1:]
                if not relative:
                    continue
                target = staging.joinpath(*relative)
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(member) as input_file, target.open("wb") as output:
                    shutil.copyfileobj(input_file, output)
                mode = member.external_attr >> 16
                if mode:
                    target.chmod(mode)
        (staging / ".archive-sha256").write_text(SDK_SHA256 + "\n")
        staging.rename(SDK)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def fetch_ps5log():
    for name, expected in PS5LOG_FILES.items():
        download(PS5LOG_URL + name, PS5LOG / name, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("title")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"PPSA[0-9]{5}", args.title):
        parser.error("title must use PPSA followed by five digits")

    fetch_lapy()
    fetch_sdk()
    fetch_ps5log()
    environment = os.environ.copy()
    environment.update(PS5_PAYLOAD_SDK=str(SDK))
    subprocess.run([sys.executable, "tools/build_owned_daemon.py", "--sdk", str(SDK),
                    "--logging-client", str(PS5LOG), "--elf-helper", "--title", args.title],
                   cwd=LAPY, env=environment, check=True)

    source = LAPY / f"build/owned_root_helper-{args.title}"
    elf = source / "lapy.elf"
    manifest_path = source / "lapy-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    expected = {
        "schema": "lapy-owned-build/1",
        "target_title": args.title,
        "mode": "elf-helper",
        "max_requests": 1,
        "service": False,
        "require_client_result": False,
        "console_validated": False,
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise RuntimeError(f"Lapy manifest {key}: {manifest.get(key)!r}, expected {value!r}")
    if manifest.get("features") != ["root_layout_probe_retry"]:
        raise RuntimeError("Lapy helper lacks the required root-layout retry feature")
    if manifest.get("elf_sha256") != digest(elf):
        raise RuntimeError("Lapy helper differs from its generated manifest")
    if manifest.get("protocol_sha256") != PROTOCOL_SHA256:
        raise RuntimeError("Lapy helper protocol differs from the pinned upstream protocol")
    if elf.read_bytes()[:6] != b"\x7fELF\x02\x01":
        raise RuntimeError("Lapy helper is not a little-endian ELF64")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(elf, output / "lapy.elf")
    shutil.copy2(manifest_path, output / "lapy-manifest.json")
    shutil.copy2(LAPY / "LICENSE", output / "Lapy-MIT.txt")
    print(f"Lapy helper verified: {manifest['build_id']} {manifest['elf_sha256']}")


if __name__ == "__main__":
    main()
