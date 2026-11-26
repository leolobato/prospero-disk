# Prospero Disk — Product Requirements

| | |
| --- | --- |
| **Title ID** | `PPSA93475` (3475 = "DISK" on a phone keypad; not in the prosperostore.net index or any local homebrew repo as of 2026-11-17) |
| **Name** | Prospero Disk |
| **Platform** | Jailbroken PS5, native app (`eboot.bin` + `sce_sys/param.json`), OpenGL 4.6 via ps5-opengl |
| **Store category** | `utilities` |
| **License** | GPL-3.0-or-later, like ProsperoEden and the kit (§8) |
| **Status** | Draft v1 — 2026-11-17 |

## 1. Summary

Prospero Disk shows where the PS5's storage went. It scans a volume quickly, then
draws it as an interactive sunburst (concentric rings, one ring per folder depth)
beside a sorted list. It is like DaisyDisk on macOS, driven by a controller and
built with the look of the `ps5-homebrew-ui` kit.

Version 1 shows sizes. Collect and delete (§5.5, added after 1.0.2) lets the
user mark what they don't need and delete it, after two confirmations.

## 2. Goals

1. Answer "what is using my space?" in seconds on internal storage, M.2/extended
   storage and USB drives.
2. Show installed games and apps by their **title name and icon**, not by
   `PPSAxxxxx` folder names.
3. Feel like first-party console software: springs, sound, focus, 60 fps at 4K
   while scanning.
4. Open instantly from a cached scan, then rescan on request.
5. Be listable on ProsperoStore.

## 3. Non-goals (v1)

- Moving or renaming anything.
- Comparing two scans ("what grew?").
- Finding duplicate files.
- A treemap view (a possible later alternative visualization).
- Network or remote volumes.

## 4. Users and context

A jailbroken-PS5 owner whose drive is full. They install games, homebrew, PKG
dumps, emulator ROMs and captures, and they want to find what is big. They hold a
controller, sit 2–3 m from a TV, and have no keyboard or mouse.

## 5. Experience

### 5.1 Launch

1. A splash screen appears. Sandbox elevation runs (see §7.2).
2. **Volumes screen.** There is one card per detected volume: name, kind icon
   (Internal / M.2 / USB), a used/free bar from `statfs`, and "Last scanned 2 h
   ago" when a cache exists.
   - Cross on a card that has a cache opens that cache immediately.
   - Cross on a card without a cache starts a scan.
   - Square starts a rescan of any volume.
3. If elevation failed, a banner explains what is missing ("Start the ELF loader
   on port 9021 or the Lapy service, then relaunch"). Only the volumes and
   folders that the app can read are offered.

### 5.2 Scanning

- A full-screen progress view shows files and folders counted, bytes found, the
  path being read now, and elapsed time.
- The sunburst grows **progressively** as directories finish.
- Circle cancels. A cancelled scan is shown as partial and is not cached.

### 5.3 Explore (main screen)

Layout (1920×1080 virtual canvas, scaled to the surface):

```
┌──────────────────────────────────────────────────────────────────────┐
│ Internal · 825 GB          ▓▓▓▓▓▓▓▓▓▓▓▓▓░░░  602 GB used · 223 free   │
│ Internal › user › app                                                │
│                                   │  NAME                     SIZE   │
│            ╭──────────╮           │  ▣ Astro Bot            48.2 GB  │
│         ╭──┤  user    ├──╮        │  ▣ Gran Turismo 7      110.4 GB  │
│        │   │ 512 GB   │   │       │  ▢ download             12.0 GB  │
│         ╰──┤          ├──╯        │  ▢ (small files)         1.1 GB  │
│            ╰──────────╯           │  ───────────────────────────────  │
│      (sunburst, 4–5 rings)        │  Selected: Gran Turismo 7        │
│                                   │  PPSA01467 · 110.4 GB · 18%      │
├──────────────────────────────────────────────────────────────────────┤
│  ✕ Open   ○ Up   □ Rescan   △ Sort   ≡ Menu   L1/R1 Volume           │
└──────────────────────────────────────────────────────────────────────┘
```

- **Sunburst** (left). The centre disc is the current folder, with its name and
  size. Each ring is one depth level, and a segment's angle is proportional to
  its size. Segments under ~1° merge into one "smaller items" segment per
  parent, drawn as 1° slices in its branch's colour mixed halfway to grey (plain
  grey on the first ring). Colour follows the top-level branch, and lightness
  drops with depth.
- **List** (right). The children of the current folder, sorted by size
  (descending) by default, with a proportional bar behind each row. Triangle
  switches to sorting by name.
- **Navigation**
  - D-pad up/down moves through the list, and the matching segment highlights.
  - The right stick aims at a segment directly, and the list follows.
  - Cross opens the folder: the sunburst re-roots with a spring animation.
  - Circle goes up one level. At the root, it goes back to Volumes.
  - L1/R1 switches volume.
  - Options opens the menu: Rescan, Scan details, About.
- **Detail panel** (bottom right). Full path, size on disk and apparent size,
  item count, modified date. For a title folder it also shows the title name,
  the icon and the title ID.
- **Unaccounted space.** At the volume root, the difference between `statfs`
  "used" and the scanned total shows as a hatched "System & hidden" segment, so
  that the numbers add up.

### 5.4 Title resolution

Folders named after a title ID are relabelled with the installed title's name
and icon. This applies to `/user/app/<ID>`, `/user/patch/<ID>`,
`/user/addcont/<ID>`, `/mnt/ext*/user/app/<ID>`, `/data/homebrew/<ID>` and
similar locations.

- **Primary source:** `/user/appmeta/<ID>/param.json` and `icon0.png`
  (`/user/appmeta/external/<ID>/` for titles on extended storage). Itemzflow
  uses the same paths.
- **Fallback:** `titleName` from `tbl_appbrowse_*` in
  `/system_data/priv/mms/app.db`, read-only.
- Icons load off the frame thread. A placeholder shows first and fades to the
  icon.

### 5.5 Collect and delete

The plan and the owner's decisions: [docs/plans/collect-delete.md](plans/collect-delete.md).

- R2 in Explore collects the focused row or puts it back; R3 opens the review
  anywhere. A bar at the bottom left shows the total and the size per drive.
- The review lists the items by drive, largest first. Cross puts one back,
  Triangle puts all back, Circle closes, **Delete...** asks twice: a summary
  (focus on Cancel), then Cross held for two seconds.
- Deleting holds the app: progress in bytes and the current path; Circle stops
  after the current item. The result shows the space freed per drive (statfs)
  and the items that failed, which stay collected. The trees are updated in
  place and the caches saved.
- Installed PS5 games and Playden's games are uninstalled through the system
  (a payload sent to the ELF loader), never deleted by hand.

## 6. Functional requirements

| ID | Requirement |
| --- | --- |
| F1 | Detect mounted volumes: internal (`/user`, `/data`, `/system_data`), extended (`/mnt/ext0`, `/mnt/ext1`), USB (`/mnt/usb0`–`/mnt/usb7`). Use `statfs` for total, used and free. |
| F2 | Scan one volume recursively with a multi-threaded walker. Use `lstat` and never follow symlinks. |
| F3 | Stay on the volume: do not cross `st_dev` boundaries. Always exclude `/dev`, `/mnt/sandbox`, `/mnt/sandbox/pfsmnt`, `/mnt/shadowmnt` and other overlay or nullfs mounts that re-expose data counted elsewhere. |
| F4 | Count hardlinked files once per `(st_dev, st_ino)` (the diskus rule). |
| F5 | Report **size on disk** (`st_blocks × 512`) by default. Show apparent size (`st_size`) as a secondary value. |
| F6 | When a directory can't be read, count it as "unreadable" with its path. Don't abort the scan. Show an "N folders could not be read" note in Scan details. |
| F7 | The sunburst and list show the current folder and update while a scan runs. |
| F8 | Resolve title IDs to names and icons (§5.4). |
| F9 | Cache each completed scan per volume. Load it at launch and label it with its age. |
| F10 | Cancel a scan at any time. The UI stays responsive (60 fps target) during a scan. |
| F11 | The app writes only its own cache and settings directory, and deletes only what the user collected and confirmed twice (§5.5). |
| F12 | Exit through `sceSystemServiceLoadExec("exit", NULL)`, never `exit()`. |
| F13 | Never collectable: volume roots, "System & hidden", roll-ups, `/system*`, `/preinst`, `/user/appmeta`, the `app.db` folder, Prospero Disk's own folders, and folders that contain any of them or installed games. |
| F14 | Deletion is bottom-up, never follows a link or crosses a mount point, `lstat`s each entry again before removing it, and goes on past errors, logging each with its errno. |
| F15 | An installed game is uninstalled through the system's app-install API, never by deleting its folders. |
| F16 | The collection lasts for the session only. |

## 7. Technical design (outline)

### 7.1 Base

- Fork `ps5-homebrew-ui` ("Start from this app", `docs/ADOPTING.md`). Run
  `make init TITLE_ID=PPSA93475 APP_NAME="Prospero Disk"`. Remove the gallery
  designs and keep the shell, the tour, the tests and the CI.
- Visual language: the **File Browser** and **Pulse Dashboard** designs (dark
  gradient, frosted panels, Inter, coloured legend dots). The sunburst is drawn
  with `gfx::DrawList::arc(...)`, the kit's ring-sector primitive.

### 7.2 Sandbox elevation (Lapy)

Two sources, both on hand in `~/playden-ps5`:

- **Client and packaging: `ps5-native-app-boilerplate`**
  (`third_party/ps5-native-app-boilerplate`, `f98de73`). The kit's own
  foundation. `examples/sandbox-elevation/` holds the cooperative client
  (`elevation::request(elevation::Capability::filesystem)`, GPL-3.0-or-later
  like the kit). The boilerplate `Makefile` has the `APP_LAPY_HELPER=1` path,
  which builds the exact-title helper, checks its manifest and packages it. The
  kit's template doesn't wire `APP_LAPY_HELPER` yet, so porting this is a work
  item. The client first gives a resident Lapy service a bounded chance, then
  sends the packaged helper to the ELF loader on `127.0.0.1:9021`. It returns
  success only after a real `/data` write and read-back.
- **Helper: Playden's pin, as built.** `tools/build-lapy-helper.py` fetches
  `leolobato/PS5-Lapy-JB-Daemon` at `66ab4f1` and builds the exact-title
  helper for `PPSA93475` with Payload SDK **v0.42** and Playden's ps5log
  client; the app itself builds with the kit's SDK. (The boilerplate's pin,
  `54a095c` with SDK v0.40, failed its exchange on 13.60: "transport error".
  This one returned "elevation ok" on 13.60 on 2026-11-17.) `make` packages it
  (`APP_LAPY_HELPER=1` by default) as `lapy.elf`, `lapy-manifest.json` and
  `licenses/Lapy-MIT.txt`, and checks the manifest's hash and title. This is
  the pin the boilerplate qualified with this exact client on 6.02 and 12.70.
  Playden pins a later fork commit (`66ab4f1`, helper SDK v0.42) with its own
  LGPL client; moving to it means moving the client too.
- **Reference integration: Playden**, which ships this flow to users.
  - Startup order (`src/platform/ps5/native_startup.cpp`): bring up the
    network (the loader is reached over TCP), find `lapy.elf` in `/app0` (or
    `/data/homebrew/<ID>`) after checking the package identity, request the
    grant, then open EGL. EGL creates threads, and the helper needs a
    single-threaded process.
  - After the grant, Playden restores the original effective UID, so the files
    it writes aren't root-owned. It then waits about 1 s for the helper to
    settle.
  - Every failure records `stage`, `status` and `error`, and the startup screen
    shows them. We do the same in the degraded-mode banner.
  - Playden's own client (`prospero-win/native/pw_lapy_elevation.c`) is
    LGPL-2.1 and adds `request_for` for child processes. We don't need that;
    use the boilerplate client.
- **Firmware:** upstream Lapy validated the resident service on 12.02. The
  boilerplate qualified the pinned helper on 6.02 and 12.70 (2026-10-04).
  Playden users report it working on firmware up to 13.60. Treat other
  firmware as unverified, and confirm 13.60 in M1.
- Call `elevation::request(filesystem)` **once, single-threaded, before EGL
  and before any scanner thread starts**.
- On failure, run in degraded mode (§5.1). Never retry in a loop.

### 7.3 Scanner

- Model: diskus. A parallel directory walk with a work-stealing queue of
  directories and a thread pool, plus an inode set for hardlinks. The algorithm
  is ported to C++20; diskus's Rust code is not used.
- Thread count: default 8 on internal NVMe and M.2, 2 on USB. USB HDDs thrash
  under parallel seeks. Detect the volume kind from the mount, with a setting to
  override it.
- Tree representation: a compact arena of directory nodes (name offset into a
  string pool, parent index, size on disk, apparent size, file count, flags).
  **Files are not stored individually.** Each directory keeps its N largest
  files (default 32), and the rest roll up into one "(other files)" node. This
  limits memory on volumes with millions of files.
- Progress: the walker publishes counters through atomics and finished subtrees
  through a lock-free handoff. The UI thread reads a snapshot once per frame and
  never blocks on the walker.

### 7.4 Cache

- Location: `/data/prospero-disk/cache/<volume-key>.pdc`. Settings live in
  `/data/prospero-disk/settings`.
- Format: a versioned binary header (magic, schema version, volume key,
  `statfs` totals at scan time, scan timestamp, duration), then the node arena
  and the string pool. The app memory-maps or reads the file in one pass.
- A schema mismatch or a corrupt file is discarded silently, and the volume
  shows as "not scanned".
- The cache is invalidated only by a rescan. Version 1 does not watch the file
  system.

### 7.5 Text

The kit's baked fonts are ASCII-only. Game titles and file names contain
Japanese, Cyrillic, accented Latin and other scripts. Adopt **ProsperoEden's
text engine**, the way ProsperoRadio did (`ADOPTING.md`, "What the first
adoption taught"):

- Source: `github.com/blackbearreloaded/ProsperoEden`, directory
  `headless/prosperoeden/pe/gfx/`. Take `font.*`, `font_format.hpp`,
  `system_fonts.*`, `harfbuzz.cpp` and `bidi.*`, and replace the kit's
  `gfx/font.*` with them. GPL-3.0-or-later, the same as the kit. The code
  started in ProsperoPuzzles, which the kit also descends from.
- The baked atlas (Montserrat in Eden; the kit's Inter here, with its
  `european` glyph set) covers Latin, Greek and Cyrillic. Japanese, Korean, Chinese, Greek, Thai and Arabic
  come from the console's own fonts in `/preinst/common/font` (SST, SSTJpPro,
  YoonGothic, DFHEI5 and others). The app ships none of them. Each font file
  is read the first time one of its characters is needed, and glyphs become
  distance fields like the baked ones. Font choice follows the system language
  (`third_party/ps5_system_language.hpp`), so shared CJK characters take the
  user's regional forms.
- HarfBuzz 12.3.2 (MIT), pinned by hash in Eden's `tools/deps.json`, does the
  shaping. `bidi.*` handles right-to-left order.
- Add an upload of the atlas rows that changed each frame (ProsperoRadio's
  addition).
- The first glyph of a new script costs one frame of about 110 ms while the
  font file is read. Warm the scripts that the scanned names and the title
  names use while the splash or the scan screen is up, not during Explore.
- Host snapshots: set `PE_SYSTEM_FONTS` to a folder with copies of the
  console fonts. Don't commit those fonts.
- Spike (§10): can the app read `/preinst/common/font` without elevation? If
  not, the degraded mode falls back to the baked atlas only.

Characters that can't be drawn render as a visible placeholder, never as a
blank.

### 7.6 Performance targets

| Metric | Target |
| --- | --- |
| Frame rate during a scan and during navigation | 60 fps at 4K, no hitch > 33 ms |
| Scan of internal storage, ~500k entries, warm cache | ≤ 10 s (to be measured; diskus-class) |
| Cold launch to Volumes screen with a cache | ≤ 1.5 s after the splash |
| Memory for a 2M-entry volume | ≤ 256 MB |

## 8. Distribution (ProsperoStore)

- Public GitHub repository under **GPL-3.0-or-later**. That is a requirement,
  not a choice: the kit and `ps5-native-app-boilerplate` are both
  GPL-3.0-or-later, and so are the elevation client and ProsperoEden's text
  engine. Ship a
  `THIRD_PARTY_NOTICES.md` (the kit's notices, kit fonts and sounds, Lapy's MIT
  license for the helper ELF, the font engine).
- Release: a tagged GitHub release with a `.zip` of the app folder. Raise
  `contentVersion` in `sce_sys/param.json` in every release.
- `sce_sys/icon0.png` at 512×512, `pic0`/`pic1` backgrounds. No `snd0.at9`
  and no in-app music: the owner removed both on 2026-11-18.
- An in-app update check: the boilerplate's `update-check` example against the
  catalog API.
- Catalog entry `apps/PPSA93475.json`, category `utilities`. Decide on
  `internalOnly`: the Lapy helper ELF and the `/data` cache may require it (see
  §10).

## 9. Quality and verification

- **Host first.** Use `tools/host-snapshots.sh` for every screen. The tour drives
  Volumes → scan (with a synthetic tree fixture) → explore → re-root → up.
  Look at the pictures before calling UI work done.
- **Unit tests (GoogleTest, sanitizers on):** walker correctness on a fixture
  tree (hardlinks, symlinks, unreadable directories, mount boundaries),
  small-file roll-up, cache round-trip, schema rejection, angle layout and
  merging of small segments.
- **Console validation** only with the owner's approval. Run the scripted tour
  plus a real scan of internal storage, M.2 and USB. Record timings and confirm
  that the scanned total plus unaccounted space equals `statfs` used.

## 10. Open questions / spikes

1. **What elevation actually grants.** Lapy's proof covers only `/data`
   read/write, and Playden relies on nothing beyond `/data` either, so no
   shipping app tells us more. On-console spike: which of `/user`, `/system_data`, `/system`,
   `/mnt/ext*` and `/mnt/usb*` become readable, and does `statfs` work on each?
   This defines the volume list and the degraded mode.
2. **Mount layout.** List the real mounts (`getmntinfo`) on a console. Confirm
   what is overlay or nullfs and which volume "Internal" should be: `/user` alone,
   or `/user` + `/data` + `/system_data` as one virtual volume.
3. **PFS and `st_blocks`.** Check that `st_blocks` is meaningful on PFS and on
   exFAT USB drives. Otherwise fall back to apparent size rounded to the cluster
   size.
4. **`internalOnly`.** Does the helper flow or the `/data` cache require
   installing on internal storage?
5. **Scan speed on PS5.** Measure the thread-count sweet spot per volume kind.
6. **System fonts while sandboxed.** Is `/preinst/common/font` readable
   without the grant (§7.5)? This decides what the degraded mode can show.

**First console run (2026-11-17, firmware 13.60, without elevation):** the
ELF loader was not listening on 9021, so the Lapy client returned
"unavailable" after 2.5 s. In the title's sandbox every candidate path
(`/`, `/user`, `/data`, `/system_data`, `/mnt/ext*`, `/mnt/usb*`,
`/preinst/common/font`, `/user/appmeta`, `app.db`) fails with EPERM or
ENOENT, and `statfs`/`statvfs` end the process (CE-108255-1). The app
therefore calls them only once elevated. Still open: the same probe with a
working grant.

**Later runs the same day, elevated (Playden's helper pin, elfldr v0.27
reloaded on 9021):** the grant succeeds ("elevation ok via helper after
4.7 s", effective uid restored to 1), and `/` and `/user` can be listed. But
`statfs` still ends the process on `/` and on `/user`, even into an 8 KiB
buffer, so it is not a struct-size overrun.

**Resolved later that day (docs/platform/CONSOLE_NOTES.md):**

- `statfs`, `statvfs` and `getfsstat` are `libkernel_sys` exports: they link,
  but resolve to a null import in an app. Volume sizes now come from
  `_fstatfs`, resolved with `sceKernelDlsym(0x2001, ...)` after the grant, and
  they are correct (Internal 674 GB, M.2 1.00 TB).
- The run that "stayed on the launch background" did write its log, in
  `/download0/hui/dev/app.log`. It showed that the grant removes `/app0` and
  `/download0`: the fonts failed to load and `main` parked. The app now reads
  its install folder from `/data/homebrew/PPSA93475` after the grant, and
  keeps settings, the probe and the cache in `/data/prospero-disk`.
- Answers to the spikes above: after the grant, `/`, `/user`, `/data`,
  `/system_data`, `/system`, `/preinst/common/font`, `/user/appmeta` and every
  `/mnt/ext*` and `/mnt/usb*` folder can be listed (1, 6). Folders under `/mnt`
  exist without a drive, so a volume must also be on another device than
  `/mnt` (2). `st_blocks` is filled in on the internal disk and on exFAT USB
  (3). Scans take about 3 s for 140,000 files on M.2 and about 7 s for 540,000
  files on Internal, with the default thread counts (5).

## 11. MVP delimitation

The MVP is the smallest build that answers "what is using my space?" on a real
console. Everything else in this PRD is v1, delivered after the MVP.

| Area | MVP | After MVP (v1) |
| --- | --- | --- |
| Volumes | Every volume that Lapy elevation can read (§10.1), plus USB. Volume list with `statfs` bars. | Merging internal mounts into one virtual "Internal" volume, if the spike shows that is better. |
| Elevation | The Lapy helper, plus a degraded-mode banner on failure. | — |
| Scanner | Parallel walk, hardlink dedupe, mount-boundary and exclusion rules, small-file roll-up, cancel, progressive results. Size on disk only. | Apparent size as a second value. Per-volume thread tuning and the settings override (MVP uses fixed defaults: 8 internal/M.2, 2 USB). |
| Visualization | Sunburst (drill in and up with springs), size-sorted list, D-pad navigation, detail panel (path, size, count), "System & hidden" segment. | Right-stick aiming at segments. Sort by name. Modified dates. |
| Titles | Title **names** from `/user/appmeta/<ID>/param.json`. | Icons. The `app.db` fallback. |
| Text | The kit's `european` glyph set. Characters it can't draw show as a placeholder. | The ProsperoEden font engine for full CJK/Cyrillic coverage. |
| Cache | — (every launch rescans). | The per-volume cache (§7.4), "scanned N ago", opening instantly. |
| Polish | Kit sounds and motion, splash. | Final icon and art, About screen. |
| Distribution | Sideloaded dev build. | Update check, GitHub release, ProsperoStore catalog PR. |
| Verification | Unit tests for the walker and layout, host snapshots, one console run with timings and a `statfs` reconciliation. | Performance targets in §7.6 met and recorded. |

**MVP exit criteria:** on the console, scan the internal storage and a USB
drive, drill into the largest game folder and see it by title name, and see a
scanned total plus "System & hidden" that equals `statfs` used, all at 60 fps.

This maps to milestones M0–M3, plus title names from M4.

## 12. Milestones

| # | Milestone | Done when |
| --- | --- | --- |
| M0 | Fork and identity | The app builds as `PPSA93475` "Prospero Disk" with the gallery removed. Host snapshots run. |
| M1 | Elevation spike | The boilerplate elevation client and the `PPSA93475` Lapy helper are ported, following Playden's startup order (§7.2). A console run logs the readable paths and mounts (answers §10.1–3). |
| M2 | Scanner core | The parallel walker, hardlink dedupe, roll-up and the arena exist. Unit tests are green. A host benchmark on a large directory runs. |
| M3 | Explore UI | Volumes screen, progressive sunburst, list, navigation and detail panel, in host snapshots. |
| M4 | Titles and text | appmeta/app.db resolution, icons. ProsperoEden's text engine is ported, with Japanese, Cyrillic and Arabic titles in host snapshots. |
| M5 | Cache | Persist and load, "scanned N ago", rescan. |
| M6 | Console validation and release | Performance targets measured. Icon and art. Update check. GitHub release and catalog PR. |
