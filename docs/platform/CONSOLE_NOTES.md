# Console notes: what a PS5 title can and cannot do

Rules learned on a jailbroken PS5 (firmware 13.60). Each rule says what
happens when you break it. **[seen here]** marks what Prospero Disk itself hit
or measured on 2026-11-17/18. **[Playden]** marks what Playden (`FAKE77031`,
another title on the same console and Lapy grant) measured: likely true here
too, but not re-checked in this app. Read this before you write code that
touches files, the kernel or threads, and before a console run.

`$PS5_HOST` is the console's address (see `.env.example`).

## 1. Find out what happened: the app log [seen here]

You do not need the kernel log (port 3232). The app writes its own log.

- `src/runtime/runtime_shims.c` opens `/download0/hui/dev/app.log` in a
  constructor, before `main`, and points `stdout` and `stderr` at it,
  unbuffered. `hui::sys::log` writes there. The previous launch is kept as
  `app.prev.log`.
- While the title runs, read it through the console's web file manager:

  ```sh
  curl -s "http://$PS5_HOST:8888/fs?path=/mnt/sandbox/PPSA93475_000/download0/hui/dev/app.log"
  ```

  The same path works over FTP on port 2120. When the title is closed, its
  sandbox is unmounted and the log cannot be read.
- "Stays on the launch background" nearly always means `main` reached
  `sys::park()` after a `fatal:` line. Read the log before you guess.
- To diagnose a crash, log one line *before* each risky call (the storage
  probe does this). The last line in the log is the call that killed it.

## 2. The grant changes the root [seen here]

After the Lapy grant (`elevation::request`), the process sees the console's
real root instead of its sandbox.

| Path | Before the grant | After the grant |
| --- | --- | --- |
| `/app0` (install folder) | yes | **no** (ENOENT) |
| `/download0` (save area) | yes | **no**; descriptors opened before stay valid |
| `/data/homebrew/<TITLE_ID>` | listable | yes: this is the install folder now |
| `/data`, `/user`, `/system_data`, `/mnt/*`, `/preinst` | EPERM/ENOENT | readable |

- Read everything you need from `/app0` (param.json, the title ID) **before**
  the grant, and resolve the install folder again after it.
  `find_app_root` in `src/main.cpp` tries `/app0`, then
  `/data/homebrew/<id>`, then `/mnt/sandbox/<id>_000/app0`, and logs each.
- Breaking this cost a day: elevation worked, then the fonts under
  `/app0/assets` failed to load and the app parked on the launch background.
- The grant takes about 4.2 s and needs the ELF loader on port 9021 (or a
  resident Lapy service). The grant never returns for a title installed
  outside `/data/homebrew`: black screen.

## 3. Some libc calls end the process

Calls that libkernel exports only in `libkernel_sys` link fine but resolve to
a null import in an app. Calling them ends the title with **CE-108255-1**. None
of them returns an error first.

Known to crash: `statfs` and `statvfs` **[seen here]**; `fstatfs`,
`getfsstat`, `umask`, `link`, `getcwd`, `chdir`, `fchdir` **[Playden]**.
`openat`, `mkdirat` and `fstatat` are `libkernel_sys` exports too, so treat
them as crashing. Playden reaches them only through its own syscall gateway,
which this app does not have, and even then only `AT_FDCWD` with an absolute
path works (a real directory descriptor gives EINVAL).

- The workaround is the app-side export, resolved by name:

  ```cpp
  extern "C" int sceKernelDlsym(int module, const char *name, void **address);
  void *symbol = nullptr;
  sceKernelDlsym(0x2001, "_fstatfs", &symbol);   // 0x2001: libkernel
  ```

  `src/disk/posix_filesystem.cpp` uses this for volume sizes **[seen here]**.
  Call it only after the grant; before the grant it failed on Playden's title
  **[Playden]**.
- The console libc has no locale support. Use stdio and POSIX calls, not
  iostreams.
- Walk directories with `opendir`/`readdir` and `lstat` on full paths.

## 4. Writing under /data [seen here, root DAC rule from Playden]

- `/data` and anything uploaded over FTP belong to root. The app runs as
  euid 1, and **root has no DAC bypass** on this console. Create the app's
  folder with the effective uid raised, then open it up:
  `seteuid(0)` → `mkdir` → `chmod 0777` → `seteuid(app_uid)`
  (`ensure_granted_directory` in `src/main.cpp`).
- The effective uid belongs to the whole process. Raise it only while no
  other thread runs, or files that other threads create become root's.
- `/data` drops the sticky bit (1777 reads back as 0777). `/data` is a nullfs
  view of `/user/data`: the same disk and the same free space.

## 5. Threads, files, mounts

- **Small worker stacks [Playden].** A 64 KiB stack buffer on a
  `std::thread` crashed Playden (CE-108255-1). Keep large buffers on the heap. A 16 KiB one in
  `PosixFileSystem::read_file` has been fine so far.
- **About 250 open files per title [Playden].** `open` fails with EMFILE after about
  249, as root too. `dup`, sockets and pipes do not count.
- Processes cannot be created.
- **Empty mount folders [seen here].** `/mnt/ext0`, `/mnt/ext1` and `/mnt/usb0`–`usb7`
  exist and can be listed whether or not a drive is in them. A folder is a
  mounted drive only when its `st_dev` differs from its parent's
  (`is_mounted` in `src/disk/volumes.cpp`). Without this check the app
  showed "USB 3"–"USB 8" and a 2 MB "Extended" drive.
- Do not answer title-behaviour questions with a payload sent to port 9021:
  a payload runs with other rights and accepts calls that a title refuses.
  Test inside the title, with a log line.

## 6. The loop: build, deploy, run

1. Build on a Linux x86-64 host (`make doctor` checks the tools): `make` for
   `dist/PPSA93475/`, `make test-unit`, and the PC preview first
   (`tools/host-snapshots.sh`).
2. Deploy only what changed. For code, that is `eboot.bin`:

   ```sh
   # FTP is zftpd on port 2120 (the Makefile's FTP_PORT default is 2121)
   python3 - <<'EOF'
   import ftplib, os
   f = ftplib.FTP(); f.connect(os.environ["PS5_HOST"], 2120); f.login()
   f.storbinary("STOR /data/homebrew/PPSA93475/eboot.bin", open("dist/PPSA93475/eboot.bin", "rb"))
   f.sendcmd("SITE CHMOD 755 /data/homebrew/PPSA93475/eboot.bin")   # else CE-107750-0
   EOF
   curl -s "http://$PS5_HOST:8888/fs?path=/data/homebrew/PPSA93475/eboot.bin" | sha256sum
   ```

   Compare the hash with your build. An upload can fail silently.
3. Launch from the home screen (or the ps5 MCP `launch`), wait about 15 s,
   then look at the screen **and** read the log (section 1).
4. Close the title before the next deploy.

The console is shared with the owner and other agents:

- Ask the owner before the first console run. With the ps5 MCP, call
  `claim_console` and read its answer before any FTP or file-manager write:
  those channels ignore the claim.
- Before you close an app, take a snapshot. If a game you did not start is in
  front, ask first: closing it loses the owner's progress.
- Never put a write in the same batch as the step it depends on (claim, close,
  launch).
- Test one new risk per run. Two changes that can both crash the title make
  the log ambiguous.
- Release the claim when you are done.

## 7. What was verified (2026-11-17, 13.60)

- Startup with the grant: install folder from `/data/homebrew/PPSA93475`, data
  and cache in `/data/prospero-disk`, 60 fps at 4K.
- Scans: the 1 TB M.2 drive (`/mnt/ext1`, 955 GB used) in about 3 s, Internal
  (`/user`, 490 GB, 540,000 files) in about 7 s. The cache file was written,
  and folders open in Explore.
- Volume sizes through `_fstatfs`: Internal 674 GB, M.2 1.00 TB, USB 128 GB
  and 1.00 TB.
- The volume list shows only mounted drives (no "USB 3"–"USB 8").
- `PPSA28416-app0` shows as its title, "STAR WARS: Galactic Racer™".
