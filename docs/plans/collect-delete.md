# Plan: Collect and delete

Status: draft for the owner's review, 2026-11-26. Branch `feat/collect-delete`
from `main` (470f04a).

Design and mockups: the artifact-colab page `6rvk5dFmfR`. Where the page and
the owner's review differ, the review wins; the decisions are repeated here so
this file stands alone.

## 1. What the owner decided

| Topic | Decision |
| --- | --- |
| Collect | Any folder or file the scan saw can be collected. **R2** collects or puts back the focused row in Explore. **R3** opens the review from any screen. **Square stays Rescan** everywhere. |
| Bar | Once something is collected, a pill at the bottom left of every screen shows the total and the size per drive (mockups 1 and 2). On Volumes, ↓ past the last card focuses it and ✕ opens the review. |
| Review drawer | Every collected item, grouped by drive, largest first. ✕ puts the focused item back. △ puts everything back after a small confirmation. ○ closes. The red "Delete…" button sits below the list, reached with ↓ past the last item. |
| Confirmation 1 | Size, count and drives. No "PS5 has no trash" line: instead, "be sure about what you delete: deleting files can have unintended consequences". The focus starts on Cancel. |
| Confirmation 2 | Hold ✕ for 2 s. Letting go early cancels. |
| Progress | A full-screen hold: bytes done of the total, the current path. ○ = "Stop after this item"; every other input is blocked. Frame loop keeps running; the work is on a worker thread. |
| Result | Space freed per drive (from statfs, not our own sum), failed items with their reason. Failed items stay collected. The tree is updated in place and the cache saved, with no full rescan. |
| Installed games | Option (c): uninstall through the system's app-install API, never by deleting `/user/app/PPSA…` by hand. **A Playden game is uninstalled the same way** (the owner, 2026-11-26). Section 4. |
| Persistence | None. The collection lasts for the session. |
| Protected | Volume roots, "System & hidden", roll-ups ("N smaller files"), `/system*`, `/preinst`, `/user/appmeta`, `app.db` and its folder, Prospero Disk's own install and data folders. Soft refusal and a toast. |
| Deletion | Bottom-up removal. No symlink is followed, no mount point crossed, `lstat` again just before each removal. Errors are skipped and logged with errno. |
| Copy | Drop the "read-only / never changes anything" promise from the README, PRD (§1, §3 non-goal, F11), About, and the F11 comments in `cache.hpp` and `main.cpp`. The scanner's "read-only walk" header stays: the scanner still only reads. The catalog PR is not touched. |

**One deviation from the brief: `unlink`, not `unlinkat`.** The SDK stubs
(`.deps/native/ps5-payload-sdk/target/lib`) show `unlink` and `rmdir` exported
by `libkernel.so`, which an app may call, while `unlinkat` is exported only by
`libkernel_sys.so`, the library whose imports resolve to null in an app and
end the process (CONSOLE_NOTES §3, where `openat` and `fstatat` are listed for
the same reason). So removal is `unlink`/`rmdir` on full paths, and the
re-`lstat` before each removal is path-based too: between that `lstat` and the
removal a folder could be swapped, which `unlink` cannot rule out the way an
`unlinkat` on an open folder could. The ancestor check in section 3 narrows
that window; nothing on a PS5 races us there in practice, but it is a limit.

## 2. Step 1: the write probe (the risk, before any UI)

The app has never written outside `/data/prospero-disk`. After the grant it
runs as euid 1, and root has no DAC bypass on this console (CONSOLE_NOTES §4):
whether a removal succeeds depends on the owner and mode of the *folder* the
entry is in. What we can delete is unknown until a console says.

**When it runs.** A new `src/platform/ps5/write_probe.cpp`, called once at
startup right after the storage probe, while the process is still
single-threaded (so raising the euid is safe), and **only** when the file
`/data/homebrew/PPSA93475/dev/write-probe` exists (uploaded over FTP for that
run; the app deletes it before probing, so the probe runs once). The app
already calls `unlink` (cache and request files), so the call itself is not
new; writing outside our folder is.

**What it does.** Every step is logged before the call, as the storage probe
does, to `app.log` and `/data/prospero-disk/write-probe.txt`.

1. *Discriminating cases*, in a scratch folder `/data/prospero-disk/probe-w`:
   - a 0755 folder made as euid 0 with a file inside (made as euid 0):
     unlink it as euid 1, then as euid 0;
   - a 0700 folder made as euid 1 with a file inside: unlink it as euid 0,
     then as euid 1;
   - a 0777 folder made as euid 0: unlink, as euid 1, a file made by euid 0
     (the case "anyone may write the folder").
   Each result says whether ownership of the folder, not of the file, decides.
2. *Real locations*, with throwaway entries only: `/user/download`,
   `/user/home/<first user folder>`, `/user/data`, and `/mnt/ext0`, `/mnt/ext1`,
   `/mnt/usb0` when mounted. For each: the folder's owner, group and mode; then,
   as euid 1, `mkdir .pd-probe-<pid>` → a file in it → `unlink` → `rmdir`; if
   `mkdir` fails, the same as euid 0 (restored at once).
3. *What users would collect*, read-only: owner and mode of the entries of
   `/user/download`, of one `/user/home/<id>` subfolder, of one
   `/user/app/<ID>`, one `/mnt/ext1/user/app/<ID>`, and of `/data/homebrew` and
   one homebrew folder in it.

It never touches an existing file.

**Console run 1** (needs the owner's go-ahead): build, upload `eboot.bin`
and the trigger file, launch once, read the log and the report, close. About
2 minutes. One new risk: writing outside our folder.

**What the answer decides.**

- euid 1 can remove where users keep things → the deleter runs as euid 1.
- Only euid 0 can → the deleter raises the euid around each removal. The
  euid is per process, so while it is raised no other thread may create a
  file: the cache save, settings and tour pictures wait until the deletion
  ends (the app is held then anyway), and the title loader only reads. That
  is a new context (other threads alive), so it gets its own **console run 2**
  that deletes one throwaway folder through the real deleter before the UI is
  built on it.
- Neither → those locations are refused at collect time ("Prospero Disk
  cannot delete here"), and I report back before building the UI.

**Run 1 result (2026-11-26, 13.60; CONSOLE_NOTES §4).** Only the folder's
owner may remove what is in it (or anyone, in a 0777 folder); root has no
bypass. Users' folders are root's, Playden's are uid 1's, exFAT USB has no
owners. So neither euid alone can delete everything: the deleter tries each
call (list, `lstat`, `unlink`, `rmdir`) as the app's user and, on `EACCES` or
`EPERM`, once more with the euid raised to 0, restored at once. **Run 2** is
therefore needed. (Launching first needed a ShadowMountPlus restart for a stale
mount left by an earlier title, with the owner's go-ahead. The probe left one
root-owned scratch folder in `/user/download`, which it could not see as uid 1;
it was removed over FTP. The quit request closed the app cleanly.)

## 3. The model and the deleter (pure, tested on the PC)

**`FileSystem` gains** `remove_file(path, &error)` and
`remove_folder(path, &error)`. The default refuses (`EROFS`). The POSIX one
calls `unlink`/`rmdir`; on a PC it refuses unless `PD_ALLOW_DELETE=1`, so
`PD_VOLUMES` pointed at real folders cannot delete by accident. The fixture
removes from its in-memory tree (under a new mutex: its maps are read by the
scanner and title threads) and lowers its "used" space, so the tour, the tests
and the snapshots show a real result.

**Identity of an item: its path, canonical.** Node indices change when the
tree is edited or rescanned, so an item is keyed by its path; the node is
looked up again when needed. On `main`, `/data` is its own volume card
("Homebrew data") and is a nullfs view of `/user/data`: the same folder can be
seen from both trees. Paths are therefore canonicalised (`/user/data/x` →
`/data/x` while both volumes exist) before they are compared, so one folder
cannot be collected twice, and "per drive" groups and totals are by *drive*
(the volume's device, as `lstat` reports it; Internal and Homebrew data show
as one drive, "Internal"), not by volume card. After a deletion, every loaded
tree that shows the path is updated and every such cache saved. (The branch
`fix/drop-homebrew-data-volume` removes the `/data` card; if it lands first,
this collapses to a no-op.)

**`src/disk/collection.{hpp,cpp}`**, no I/O:

- `Collection`: items `{path, volume, drive, size, kind (folder / file / title),
  title_id}`, `add`, `remove`, `clear`, `contains`, `covered_by` (an ancestor
  is collected), per-drive totals, the sorted view for the drawer.
- Nesting: collecting a folder drops the collected items inside it; inside a
  collected folder R2 refuses ("Already collected with <folder>").
- `Protection`: the rules of section 1 as data from the `Environment`
  (console paths on a PS5, fixture paths on a PC), checked component by
  component on the canonical path. A path is protected when it is, or is
  inside, a protected root (`/system*`, `/preinst`, `/user/appmeta`,
  `/system_data/priv/mms`, our install and data folders, including their
  `/user/data/…` aliases), **or when it contains one** (`/data/homebrew` holds
  our install; `/user` holds `appmeta`). A folder that contains installed
  titles (`/user/app`, `/mnt/ext1/user`, `/data/playden/games`) is refused:
  collecting it would delete games by hand. `/user/patch/<ID>`,
  `/user/addcont/<ID>` are refused with "removed with its game".
- Titles: `/user/app/<ID>`, `/mnt/ext*/user/app/<ID>`, a Playden game folder
  (one that `playden_titles` maps to a `FAKE78xxx` title) and that title's
  `/data/homebrew/FAKE78xxx` become *title* items, uninstalled (section 4).
- After a rescan, items whose path is still in a tree stay, with the new size;
  the rest drop out.
- Sizes come from the scan. Confirmation 1 shows them as "about"; an item
  collected from an old cache is not re-measured before deleting (a re-stat of
  a 100 GB folder is a walk of its own); the result shows statfs's real figure.

**`src/disk/deleter.{hpp,cpp}`**: one worker thread, started by the screen.

- Input: the items, the `FileSystem`, a `TitleRemover` (section 4), a
  privilege hook (raise / restore, or none: section 2). Progress (atomics, the
  current path under a mutex): bytes planned and done (from the `lstat` of each
  removed file), item index, stop requested, finished. No large stack buffers
  (CONSOLE_NOTES §5).
- Per item, first: the path must be absolute, without `.`/`..`, strictly
  inside its volume and not protected (checked again, not trusted from the
  collection); each ancestor from the volume root down is `lstat`ed and must be
  a real folder, not a symlink.
- Walk: an explicit stack, not recursion. `list()` closes each folder before
  returning, so the 250-open-files limit is never near. A folder on the item's
  device is entered; one on another device is a mount point, skipped with
  `EXDEV`; anything else is `lstat`ed again, checked to be the same device and
  inode as listed, and removed (a symlink as a link, never followed). A folder
  is removed after its children. Errors are recorded (path, errno) and the
  walk goes on.
- Stop: a flag checked between items ("after this item").
- After a partly removed folder, the worker rescans just that folder (the
  existing `Scanner`, one thread) so the tree can show what is left.
- Result per item: removed / partly / not at all, the first errno, bytes
  removed, the remaining subtree when partly removed.

**`Tree` gains** `remove(index)` (unlink from the parent's child chain and
subtract its totals up the path), `replace(index, subtree)` (graft the rescan
of a partly removed folder) and `compacted()` (drops unreachable nodes before
the cache is written; `adopt` keeps validating the result).

**Tests** (`tests/unit/disk_test.cpp`; the `PosixScan` pattern on a temporary
folder, and the fixture): nesting and totals; every protection rule, including
the `/user/data` aliases and "contains a protected path"; the same folder
collected from `/data` and from `/user/data` counts once, in one drive;
deletion of a tree with files, empty folders and a symlink to a folder outside
(the target survives); a nested mount (the fixture's `mnt_other`, another
device) is not entered; a folder made read-only leaves the item partly removed
with `EACCES` and the rest gone; Stop between items; a path swapped for a
symlink after planning is not followed; tree removal and graft keep `adopt`
happy; cache round-trip after compaction; title items go to the remover, never
to the walk.

## 4. Installed games and Playden games: uninstall through the system

**The reference is Playden's title remover**
(`playden-ps5/tools/title-remover/main.c`, `src/platform/ps5/title_remover.cpp`),
which has uninstalled Playden's `FAKE78xxx` titles on this console. It answers
the brief's questions:

- *Can the Lapy-granted app call it itself?* No. Playden, with the same
  grant, "cannot load libSceAppInstUtil itself". It ships a one-shot payload
  and sends it to the payload loader on `127.0.0.1:9021`, the same loader the
  Lapy helper already needs.
- *The call*: `sceAppInstUtilInitialize()`, then the caller's authid set to the
  installer's (`0x4800000000000006`, from ShadowMountPlus), then
  `sceAppInstUtilAppUnInstall(title_id)`. Order matters (Playden's
  `results/20261007-native-uninstall`). The uninstall completes
  asynchronously; Playden confirms it by polling `app.db` until the title is
  gone.
- *The exchange*: the app writes `title=…\nnonce=…\n` to a request file in a
  folder it owns; the payload trusts only a folder and a file owned by the
  app's user (not root, not links, one link), writes `nonce=…\ncode=XXXXXXXX\n`
  back as that user, and exits.

**What Prospero Disk does**: the same design, its own copy.

- `tools/title-remover/main.c` (GPL-3.0-or-later like Playden's payload,
  credited in `THIRD_PARTY_NOTICES.md`), built by the Makefile with the SDK
  that builds the Lapy helper, packaged as `title-remover.elf`, its hash pinned
  like `lapy.elf`. Its request folder is `/data/prospero-disk/title-remover`,
  trusted only when owned by the owner of `/data/prospero-disk` (and that owner
  is not root). It accepts a title id that is `PPSA`/`CUSA` + 5 digits, or
  `FAKE78` + 3 digits (Playden's games), and nothing else: never `PPSA93475`
  (us) or `FAKE77031` (Playden itself).
- `src/disk/title_remover.{hpp,cpp}`: the client (write the request, send the
  ELF, wait up to 15 s for the answer, then poll `app.db` through the existing
  `titles.cpp` reader for up to 60 s). On the PC and in tests a fake remover
  answers.
- In the deleter, a title item calls the remover instead of walking. The
  bytes freed are what statfs says afterwards. A refusal (code ≠ 0), no
  answer, or a title still in `app.db` is a failed item with that reason; the
  folder is never deleted by hand as a fallback.
- Retail: the system removes `app`, `patch`, `addcont` and appmeta together
  (to be confirmed on the console: what is left behind is measured by a
  rescan of those folders).
- **Playden games (the owner, 2026-11-26).** What is deleted follows what
  was collected:
  - the title (`/data/homebrew/FAKE78xxx`): uninstall the title through the
    system, then delete its folder;
  - the game data (`/data/playden/games/<id>`, the folder `playden_titles`
    maps to that title): delete the game data, and uninstall and delete its
    title as above.
  The game data is deleted only after the title is confirmed gone from
  `app.db`, so a refused uninstall leaves both in place. Playden's own records
  are not edited.
- Console runs: **run 3** resolves nothing new in our process (the call is in
  the payload) but sends the payload with a request for a throwaway homebrew
  title the owner picks and checks `app.db`; **run 4** a retail title the
  owner picks, on Internal; extended storage is a later run.

## 5. The UI (`src/concepts/disk.cpp`, kit components)

- **Explore.** R2 on a row collects / puts back, with a cue; a refused row
  plays the soft refusal (the existing shake) and a `ui::ToastStack` toast
  says why. Collected rows: an amber "Collected" tag and amber name/size; rows
  inside a collected folder: a hollow "In collection" tag; a title: "Uninstall".
  Collected segments are hatched amber in the sunburst (the dash technique of
  "System & hidden"). Hints: ✕ Open, ○ Up, □ Rescan, △ Sort, R2 Collect / Put
  back (hidden on a protected row), ≡ Menu, L1/R1 Volume. The header gains
  "N GB after delete".
- **The bar.** Drawn by the screen (a themed pill): the total, the size per
  drive, "R3 Review". On every screen while the collection is not empty,
  bottom left. On Volumes it is a focus stop below the list.
- **Volume cards**: the collected share hatched inside "used", "X free after
  delete".
- **Drawer**: `ui::Sheet` from the bottom, a `ui::ListView` of the items by
  drive (group header rows the focus skips), then a danger `ui::PushButton`
  "Delete 124 GB…". ✕ puts back, △ opens a small `ui::Dialog` ("Put everything
  back?"), ○ closes.
- **Confirmation 1**: `ui::Dialog`, `StatusKind::danger`, Cancel (default) /
  Continue (destructive): size, count, drives, the largest items, uninstalls
  named as such, and the owner's warning line.
- **Confirmation 2**: a panel with a `ui::HoldButton` (`hold_seconds = 2`).
  A release before it completes, or ○, closes it with nothing deleted.
- **Progress**: a new `Screen::deleting`, a bar in bytes, "Item 2 of 4", the
  current path or "Uninstalling <title>", hint "○ Stop after this item". Every
  other input is ignored; cache saves and settings wait.
- **Result**: `ui::Dialog`: freed per drive (statfs before and after), failed
  items with reasons (the first few, then "and N more"). The trees are already
  updated; caches are saved on the save thread.
- **R2 / R3 glyphs.** `ui::Button` has `r2` but no `r3`: a small kit commit
  adds `Button::r3` unless the `right_stick` glyph reads as R3.
- **Tour**: named pictures `collect`, `refused`, `bar-volumes`, `drawer`,
  `confirm`, `hold`, `deleting`, `result`, on the fixture.

**The tour must never delete on a console.** `tools/console-tour.py` runs the
same tour over the real volumes. `app::Context` gains a `scripted` flag set by
the tour runner (one boolean in `src/app`, its own commit); when it is set and
the file system is not the fixture, the deleter and the remover run dry: they
plan and report, and remove nothing. The 600 random inputs of
`concepts_test` run on the fixture only.

## 6. Copy

README line 5; PRD §1, §3 (the non-goal goes; a short §5.5 "Collect and
delete" and new F-rows describe the feature), F11 ("the app writes its cache;
it deletes only what the user collected and confirmed twice"); About; the F11
comments in `src/disk/cache.hpp` and `src/main.cpp`. The catalog PR is not
touched.

## 7. Commits, in order

1. Add the write probe. → **console run 1** (owner's go-ahead). Stop and
   report if nothing can be removed.
2. Add removal to the file systems (POSIX with the PC guard, fixture) + tests.
3. Add `Tree::remove`, `replace`, `compacted` + tests.
4. Add the collection model, canonical paths and protection rules + tests.
5. Add the deleter (+ the privilege hook if run 1 asks for it) + tests.
   → **console run 2** only if run 1 says euid 0 is needed.
6. Add the title remover payload and client (fake on the PC) + tests.
   → **console runs 3 and 4** (owner's go-ahead, titles the owner picks).
7. Mark the tour as scripted in `app::Context`.
8. Collect in Explore: R2, tags, hatching, toasts.
9. The bar on every screen, focusable on Volumes.
10. The review drawer.
11. The two confirmations.
12. Progress and result; tree update and cache save.
13. Tour steps and pictures.
14. Copy: README, PRD, About, comments.

Each commit builds and passes `make test-unit`. Before finishing: `make`,
`tools/lint-changed.sh main`, `LINT_SKIP_TIDY=1 make lint`, and every picture
of `tools/host-snapshots.sh build/snapshots disk` looked at. Then one Codex
review of `main..feat/collect-delete`.

## 8. Not verified until a console run

Write permissions (run 1); the deleter with other threads alive if it needs
euid 0 (run 2); the uninstall from Prospero Disk's payload, and what retail
uninstall leaves behind (runs 3, 4); uninstall of a game on extended storage;
deletion speed of a large folder on the internal SSD; whether statfs shows the
freed space at once (PFS may free lazily); the R2/R3 feel with a controller.
