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

## 9. As built (2026-11-27)

What changed from the plan while implementing:

- **Order.** Run 1 happened; the UI was built before runs 2 and 3, since only
  the title rows depend on them. Run 2 (a deletion from the app with its other
  threads alive, the euid raised per call) and run 3 (an uninstall through the
  payload) are still to do, each with the owner's go-ahead.
- **Title presence** is checked as Playden does, by `lstat` of
  `/user/app/<ID>` (and the M.2 app folders, raised when refused), and has three
  values: a check that fails is "unknown", never "gone". The payload is sent
  only for a listed title, and a Playden game's data is deleted only once its
  title is gone.
- **Playden's order**: the title's folder is moved to
  `/data/prospero-disk/trash` before the uninstall (and back if it is
  refused), then deleted. Playden also removes ShadowMount's "manual source"
  for the title first; Prospero Disk does not, which the owner should confirm
  is harmless once the folder has moved.
- **PS4 games (CUSA)** are refused for now ("Uninstall this from the home
  screen") until a console run covers one. `/user/app/PPSA93475` (our own
  registration) is protected.
- **Mount points**: the console reports `st_dev` 0 for several file systems, so
  the deleter also compares mount names (`_fstatfs` `f_mntonname`). The storage
  probe now logs which first-level folders are mounts of their own.
- **Tours** set `app::Context::scripted`; on real volumes a scripted run never
  starts the deleter ("A tour drove the controller, so Prospero Disk deleted
  nothing").
- **Unreadable folders** (as the scanner's user, e.g. root's 0700
  `/user/download`) can't be collected: their size is unknown. The scanner runs
  as the app's user, so such folders show as "could not be read" already.
- The title remover ELF is built from source by `make`; two builds differ, so
  its hash is not pinned like `lapy.elf`.

### Run 2 (2026-11-27, 13.60): a deletion from the app

A throwaway folder made over FTP (`/data/pd-run2`: 4 files, 3 folders,
7.67 MB, 0777 folders under root's `/data`) was collected in Explore as
`/user/data/pd-run2`, reviewed, confirmed, and deleted with the 2-second hold.
Letting go after one second cancelled ("Nothing was deleted."). The deletion
took under a second, the result read "7.67 MB freed", the folder was gone over
FTP, the log had no `delete:` errors, frames stayed at 16.7 ms, and the quit
request closed the app. Seen on the way: after the result, the list's focus
goes back to its first row; `/user/download` is not readable by the scanner, so
nothing in it can be collected.

### Run 3 (2026-11-27, 13.60): uninstalls

Two small Steam games were installed through Playden for it. Before the run,
`/data/playden-games` was missing from the game folders (the `/data` volume row
left main), so a game's files counted as plain files; fixed and tested.

- **Strikers 1945, by its game files** (`/user/data/playden-games/1261960`):
  uninstalled through the payload, title folder moved to the trash and deleted;
  `/user/app`, `/user/appmeta`, `/data/homebrew` and the files gone; 673 MB
  freed. Left behind: Playden's bottle `/data/playden/games/steam-1261960`.
  Playden then shows the game as "Storage disconnected" with no way to remove
  or reinstall it.
- **Xeno Crisis, by its title** (`/user/data/homebrew/FAKE78079`): uninstalled;
  files and bottle kept; 572 MB freed. Playden shows "Setup stopped early ·
  Finish setup", which registers it again. `/user/app/FAKE78079` itself is
  refused ("Uninstall this from the home screen").
- **CONTROL Resonant, retail through ShadowMountPlus** (`/user/app/PPSA34547`,
  `mount.lnk` to `/mnt/ext1/homebrew/CONTROL Resonant …`): the payload's
  uninstall was accepted and confirmed, then ShadowMountPlus registered it
  again on its next scan, about 10 s later. Its own `/api/v1/games/uninstall`
  does the same. A title whose source sits in a scanned folder cannot stay
  uninstalled while the source is there.
- The payload loader on 9021 answered from inside the app (not from the LAN).
  No `delete:` errors; the payload's `result` file stays in its folder.

### After run 3: what changed

The owner chose: a Playden game's files also clean Playden's records; a
ShadowMount game's title goes with its source folder; PS4 games are allowed.

- **Playden's records**: when a game's files (`<drive>/playden-games/<game>`)
  are deleted, Prospero Disk adds `uninstall <game>` to Playden's request file
  (`/data/homebrew/FAKE77031/dev/install-request.txt`); Playden reads it on its
  next start and forgets the install, with nothing left to delete. Its bottle
  in `playden/games` stays: Playden keeps bottles for their saves and brings
  them back on a new install. Collecting the bottle itself does not ask.
- **ShadowMount games**: each title's `mount.lnk` names its source folder (a
  folder inside a `homebrew` folder). Collecting the title or the source
  uninstalls the title and deletes the source; the source first moves to
  `<drive>/prospero-disk/trash` (same drive, outside ShadowMount's scan), as a
  Playden title's folder does. Package installs have no `mount.lnk` and are
  left to the system as before. `/user/app/FAKE78xxx` now means the Playden
  title (its `/data/homebrew` folder).
- **PS4 games** (CUSA) are accepted by the rules and the payload.
- A title's size in the collection now includes its other folders when a
  loaded scan knows them.

### Run 4 (2026-11-27, 13.60)

- **CONTROL Resonant** by its source (`/mnt/ext1/homebrew/CONTROL …`, 160 GB):
  moved to `/mnt/ext1/prospero-disk/trash`, uninstalled, deleted in about 10 s;
  ShadowMount logged "source removed" and did not register it again over the
  following scans. Extended 2 up 160 GB, Internal up 118 MB.
- **Shovel Knight** (CUSA01867) and **Rez Infinite** (CUSA05868) on the M.2:
  uninstalled; their app, patch and appmeta folders gone; 593 MB and 1.01 GB.
- **Loom** (Playden, GOG) by its files: title, title folder and files gone, the
  request line written; Playden then showed "Not installed" with Install.
- Strikers 1945 (run 3) was cleaned by writing the same request by hand.
- Not seen: a Playden request while Playden's runtime is not ready (Playden
  keeps the file and says so in `install.log`); deleting Xeno Crisis's files now
  that its title is gone (no title maps to them: they are plain files, and
  Playden is not told).

### Second review and run 5 (2026-11-27)

One Codex review of `c2e46e1` found that a `mount.lnk` could name a folder
that is not the title's, that two titles on one folder (or one title on two)
were resolved silently, that the source was not checked again at delete time,
that a linked trash or request folder could redirect a write, that a copy of a
Playden game on another drive would make Playden forget the active one, and
that sizes went to the title's drive. Fixed in `3660ccd`: a source must sit
directly in a scanned `homebrew` folder and hold this title's `param.json`;
records that don't add up refuse both sides; the source is read again before
anything moves; every trash folder and the request file are checked as real
(the request file with one link); a Playden game on two drives is left to
Playden; a title's other folders count on their own drive, and an unknown size
is said. ShadowMountPlus (`FAKE10101`) is protected.

The owner chose option 1 for games whose title is already gone: a whole
folder in `playden-games` asks Playden to forget the game even when no title
maps to it.

Run 5: startup with the stricter reading kept Flight of the Amazon Queen
collectable as a title (571 MB with its title folder); Xeno Crisis's files
(`464260`, no title) were deleted as files and Playden forgot the game
(`install-request uninstall game=464260`, gone from its registry).
