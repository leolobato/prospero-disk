# Prospero Disk

A disk-usage explorer for jailbroken PS5s (title `PPSA93475`), forked from
ps5-homebrew-ui (remote `kit`). What to build is in
[docs/PRD.md](docs/PRD.md); the MVP scope is its section 11 and the milestones section 12.

The kit's instructions for coding agents are in [AGENTS.md](AGENTS.md); they
apply here and are imported below. Its gallery designs were removed: the app's
screens live in `src/concepts/disk*.cpp`, and the Component Library
(`components`) stays as a reference until milestone M3.

## Read first

**[docs/platform/CONSOLE_NOTES.md](docs/platform/CONSOLE_NOTES.md)**, before
any code that touches files, the kernel or threads, and before any console run.
It explains how to read the app's log, what the Lapy grant does to `/app0` and
`/download0`, which libc calls end the process, how to write under `/data`, and
how to deploy. Each rule says whether this app saw it or Playden did.

## Where things stand (2026-11-27)

Runs on the console (13.60): elevation, the probe, volume sizes, the volume
list (mounted drives only), scans of Internal, M.2 and `/data`, the cache,
Explore, title names and icons, 60 fps at 4K, and collect and delete: plain
folders, PS5/PS4 uninstalls, ShadowMountPlus games with their source, Playden
games (runs 1-4 in `docs/plans/collect-delete.md`). Not yet checked on the console:
scanning a USB drive, the console fonts for other scripts, and whether the
"System & hidden" total adds up exactly. There is no music: the tracks and
`snd0.at9` were removed on purpose; the UI sound effects stay.

## Where it runs

| Part | Where | Notes |
| --- | --- | --- |
| `make`, `make test-unit`, `make lint`, `tools/host-snapshots.sh` | a Linux x86-64 host with clang 18 (`make doctor` checks) | Not macOS: the Makefile and the PS5 toolchain need Linux. `make test-unit` builds with sanitizers (slow the first time). `make lint` runs clang-tidy over the whole tree (25+ minutes on 4 cores): while you work, use `tools/lint-changed.sh` (about 2 minutes) and `LINT_SKIP_TIDY=1 make lint`. |
| The app | the console, installed at `/data/homebrew/PPSA93475` | Put `PS5_HOST=<console address>` in `.env` (ignored by git). |

Keep a separate checkout per task on the build host rather than editing a
checkout another agent left with uncommitted changes. Copy `.deps/` into the
new checkout instead of fetching it again.

## The loop

1. Change code. For UI, render on the PC and **look at the pictures**:
   `tools/host-snapshots.sh build/snapshots disk`.
2. `make test-unit`. Add a test for a bug you fix, and check that it fails
   without the fix.
3. `make`, then `tools/lint-changed.sh` and `LINT_SKIP_TIDY=1 make lint`.
   Format with `clang-format-18 -i <files>` on the build host, and bring the
   formatted files back to wherever you edit.
4. On the console, only with the owner's go-ahead: deploy what changed
   (usually `eboot.bin`, over FTP on port 2120, then `SITE CHMOD 755`), check
   the SHA-256, launch, look at the screen, and read `app.log`. The steps and
   the commands are in CONSOLE_NOTES.md section 6.
5. Say which console behaviour you did not test.

Changing `sce_sys/icon0.png`, the backgrounds or `snd0.at9`: the home screen
uses copies made when the title was registered (`/user/appmeta/PPSA93475`,
`/user/app/PPSA93475/` and its `sce_sys/`). On 2026-11-18 the new icon was
written to the install folder and to all three copies, and the tile still
showed the old icon. Whether a console restart or registering the title
again refreshes it has not been checked.

## Console etiquette

The console is the owner's and is shared with other agents.

- Ask before the first run. Claim the console (ps5 MCP `claim_console`), and
  wait for the answer before any FTP or file-manager write.
- Take a snapshot before you close or launch anything. If an app you did not
  start is in front, ask first: launching replaces it.
- Test one new risk per run, and release the claim at the end.

## Commands

```bash
tools/host-snapshots.sh build/snapshots <design id|all>   # PNG frames in build/snapshots/
make test-unit                                            # tests, sanitizers on
make                                                      # the PS5 app folder in dist/
make lint                                                 # format, static analysis, metadata
tools/scan-bench.sh <path> [threads]                      # the scanner alone, on a PC folder
```

The preview renders the same UI code as the console, off-screen, with no
console and no GPU ("Local preview" in AGENTS.md has the options, the output
names and what it cannot show).

## Code map (app-specific)

| Path | What |
| --- | --- |
| `src/main.cpp` | Startup order: title ID from param.json, Lapy grant, the paths after the grant (`AppPaths`), the probe, display, fonts, audio, the frame loop |
| `src/disk/` | Scanner, tree, sunburst layout, cache, title lookup, volumes (`detect_volumes`, `is_mounted`), the file system (`posix_filesystem.cpp`, `_fstatfs` on the console) |
| `src/concepts/disk*.cpp` | The Volumes, Scanning and Explore screens |
| `src/platform/ps5/elevation/` | The Lapy client; `lapy.elf` is built by `tools/build-lapy-helper.py` |
| `src/platform/ps5/storage_probe.cpp` | Writes `/data/prospero-disk/probe.txt` at each launch |
| `src/runtime/runtime_shims.c` | Opens `app.log` before `main` |

@AGENTS.md
