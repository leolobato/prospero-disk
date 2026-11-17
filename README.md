# Prospero Disk

Shows where a jailbroken PS5's storage went: a fast scan, drawn as an
interactive sunburst next to a sorted list. Driven by a controller, built on
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui).
Read-only: it shows sizes and never changes your data.

[docs/PRD.md](docs/PRD.md) is the product requirements document.

## Using it

| Screen | Buttons |
| --- | --- |
| Volumes | Cross opens the last scan (or scans), Square rescans, Options shows About |
| Scanning | Circle cancels; what was found so far opens as a partial scan |
| Explore | D-pad moves through the list, the right stick aims at a segment, Cross opens a folder, Circle goes up, Triangle sorts by size or name, Square rescans, Options opens the menu (Rescan, Scan details, About), L1/R1 switch volume |

At the root of a volume, "System & hidden" is the used space the scan cannot
see (statfs used minus everything counted), so the numbers add up. Folders
named after a title ID (also `PPSA01467-app0` and the like) show the title's
name and icon, and so do Playden's game folders (`playden/games/2610`), through
the PS5 title Playden registered for each game. Text in other scripts
(Japanese, Korean, Chinese, Thai, Arabic) is drawn with the console's own
fonts.

On launch the app asks for sandbox elevation through Lapy: a resident Lapy
service, or the ELF loader on port 9021, must be running. Without it the app
shows a "Limited access" banner and offers only the volumes it can read.
Scans are cached in `/data/prospero-disk/cache` (in the title's own storage
when elevation failed). Each launch also writes `probe.txt` next to the cache:
the mounts, which volume and metadata paths are readable, and block counts
for a few installed files (the elevation spike, PRD section 10).

## Building

Needs Linux (or WSL) with clang 18, ninja, ccache, Python 3, clang-format-18
and clang-tidy-18; `make doctor` checks. Every dependency (Payload SDKs,
ps5-opengl, HarfBuzz, the Lapy helper) is fetched at a pinned version and
hash-checked.

```bash
make                       # dist/PPSA93475 and dist/PPSA93475.zip
make test-unit             # GoogleTest, sanitizers on
make lint                  # format, clang-tidy, headers, metadata
APP_LAPY_HELPER=0 make     # without the elevation helper
```

## Previewing on a PC

The app renders off-screen on a PC with the same code (Mesa), and the tour
drives it through Volumes, a scan, Explore, the menu and the dialogs:

```bash
tools/host-snapshots.sh build/snapshots disk      # PNGs in build/snapshots/
```

By default it scans a built-in, invented fixture volume (`src/disk/fixture_filesystem.cpp`)
paced by the tour's clock, so a scan in progress is the same picture on every
run. Two environment variables change that:

- `PD_VOLUMES="Home=/home/me;Usr=/usr"` scans real folders instead
  (`PD_CACHE=<dir>` also caches them, `PD_HOMEBREW=<dir>` names a folder of
  Playden title folders for game names, `PD_APP_DB=<file>` names an app.db
  for title names);
- `PE_SYSTEM_FONTS=<dir>` names a folder with fonts under the console's file
  names (`SSTJpPro-Regular.otf`, `SSTArabic-Medium.otf`, `SSTThai-Medium.otf`,
  ...) so other scripts draw on a PC too, and `PE_LANGUAGE=ja-JP` picks the
  language. Symbolic links to Noto or DejaVu fonts work; never commit
  console fonts.

`tools/scan-bench.sh <path> [threads]` runs the scanner alone on a folder and
prints its time and totals; compare with `du -s --block-size=1 -x <path>`.

## Status

Built and tested on a PC: the scanner, cache, layout, title lookup (param.json
and app.db), text engine and every screen (pictures and tests). On a console
(13.60): the elevation, the probe, volume sizes, scans of the internal disk and
an M.2 drive, the cache, Explore, and 60 fps at 4K. Not yet checked there: the
console's fonts for other scripts, USB drives, and the "System & hidden"
total. Console runs need the owner's go-ahead; read
[docs/platform/CONSOLE_NOTES.md](docs/platform/CONSOLE_NOTES.md) first.

## License

GPL-3.0-or-later, the same as ps5-homebrew-ui and ProsperoEden. See
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
