# Prospero Disk

Shows what is using the storage on a jailbroken PS5. It scans a drive quickly
and draws the result as an interactive sunburst next to a sorted list, all
driven by the controller. You can collect folders and files as you explore and
delete them together, after two confirmations.

![The Explore screen: a sunburst of the internal drive next to its folders sorted by size](docs/images/explore.png)

## Install

1. Download `prospero-disk-<version>-PPSA93475.zip` from the
   [latest release](https://github.com/leolobato/prospero-disk/releases/latest).
2. Unzip it and copy the `PPSA93475` folder to `/data/homebrew/` on the
   console (with FTP or a file manager), so the app ends up at
   `/data/homebrew/PPSA93475`.
3. Wait about ten seconds for the console to register the title. Prospero Disk
   then appears on the home screen.

It needs [elfldr](https://github.com/ps5-payload-dev/elfldr), the ELF loader
on port 9021, running when you launch it. Most jailbreak loaders start it for
you (see [Access](#access)).

Tested on firmware 13.60.

To update, replace the `PPSA93475` folder with the one from the new release.

## Using it

![The Volumes screen: the internal drive and a USB drive with their used and free space](docs/images/volumes.png)

| Screen | Buttons |
| --- | --- |
| Volumes | Cross opens the last scan (or starts one), Square rescans, Options shows About. Below the last card, the collection bar: Cross opens the review |
| Scanning | Circle cancels. What was found so far opens as a partial scan |
| Explore | The D-pad moves through the list, the right stick aims at a segment, Cross opens a folder, Circle goes up, Triangle sorts by size or name, Square rescans, R2 collects or puts back the focused row, Options opens the menu (Rescan, Scan details, About), L1/R1 switch volume |
| Anywhere | R3 opens the review of what is collected |

- **System & hidden**: at the top of a volume, this entry is the used space
  the scan cannot see, so the numbers add up to the drive's used space.
- **Game names**: folders named after a title ID (`PPSA01467`,
  `PPSA01467-app0` and the like) show the game's name and icon. So do
  Playden's game folders (`playden/games/2610`).
- **Other languages**: names in Japanese, Korean, Chinese, Thai or Arabic are
  drawn with the console's own fonts.
- **Cache**: scans are saved in `/data/prospero-disk/cache`, so opening a
  volume again shows the last scan without scanning again. Square rescans it.

### Collect and delete

R2 in Explore collects the focused folder or file for deletion; a bar at the
bottom left then shows the total on every screen. R3 opens the review: Cross
puts an item back, Triangle puts everything back, and **Delete...** at the
bottom asks twice: a summary first, then Cross held for two seconds. While it
deletes, the app shows the progress and Circle stops after the current item.
The result says how much space each drive got back and lists anything that
could not be deleted, which stays collected. The collection is forgotten when
the app closes.

- Be sure about what you delete: deleting files can have unintended
  consequences.
- Some things can't be collected: a whole drive, "System & hidden", the
  console's system folders (`/system*`, `/preinst`, `/user/appmeta`, the
  `app.db` folder) and Prospero Disk's own files.
- A PS5 game (`/user/app/PPSA...`) is **uninstalled through the console**, as
  the home screen would, never deleted by hand; its patches and add-ons go with
  it. A Playden game is uninstalled the same way: collecting its title removes
  the title, collecting its game data removes the data and the title. PS4
  games are left to the home screen for now.
- Uninstalling sends a small helper, `title-remover.elf`, to the ELF loader on
  port 9021, like Lapy.

## Access

A PS5 app normally sees only its own sandbox. To read the whole console, Prospero
Disk ships a small root helper, [Lapy](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)
(`lapy.elf` in the app folder). When the app starts, it sends the helper to the
ELF loader on port 9021, and the helper grants it read access to the file
system. This takes a few seconds. You don't install or start Lapy yourself.

If nothing answers on port 9021, the app still opens: it shows a "Limited
access" banner, lists only the volumes it can read and keeps its cache in its
own storage. Load the ELF loader and open the app again for full access.

## Known limits

Not yet checked on a console: scanning USB drives, the console fonts for
non-Latin scripts, whether the "System & hidden" total adds up exactly,
deleting from the app's own process with other threads running, and
uninstalling a game from it.
Please open an issue if you try any of these.

## Building

Needs Linux (or WSL) with clang 18, ninja, ccache, Python 3, clang-format-18
and clang-tidy-18 (`make doctor` checks them). Every dependency (the Payload
SDKs, ps5-opengl, HarfBuzz, the Lapy helper) is downloaded at a pinned version
and hash-checked.

```bash
make                       # dist/PPSA93475 and dist/PPSA93475.zip
make test-unit             # unit tests, with sanitizers
make lint                  # format, clang-tidy, headers, metadata
APP_LAPY_HELPER=0 make     # without the elevation helper
```

Built on [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui).
The design is described in [docs/PRD.md](docs/PRD.md).

## License

GPL-3.0-or-later, the same as ps5-homebrew-ui and ProsperoEden. See
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
