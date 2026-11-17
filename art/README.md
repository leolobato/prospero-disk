# Prospero Disk art

Source artwork for what the PS5 home screen shows, and for the ProsperoStore
listing (milestone M6 in [docs/PRD.md](../docs/PRD.md)). Put the source files
here; the converted files the build packages live in [`sce_sys/`](../sce_sys)
and are made with `tools/prepare-assets.sh`. `sce_sys/` still holds the kit's
placeholder art.

## What is needed

| Asset | Deliver here | Becomes | Requirements |
| --- | --- | --- | --- |
| Icon | `icon.png` | `sce_sys/icon0.png` | Square, at least 512 x 512 (1024 x 1024 preferred). Converted to exactly 512 x 512 PNG. Opaque, no rounded corners or drop shadow (the console adds its own frame). Keep the subject inside the centre ~80%: the tile is cropped and scaled in places. |
| Selection background | `background.png` | `sce_sys/pic0.dds` | 16:9, 3840 x 2160 preferred (1920 x 1080 minimum). Shown full screen while the tile is selected on the home screen, under the system's title, buttons and "Friends Who Play" card: keep the left third and the bottom third calm and fairly dark. Converted to a single-level 3840 x 2160 BC7_UNORM DX10 DDS. |
| Launch background | `launch.png` (optional) | `sce_sys/pic1.dds` | Same format as the selection background, shown while the app starts. Without it the selection background is used for both. Supply both DDS files or neither. |
| Selection music | `music.wav` (or .mp3/.flac/.m4a, optional) | `sce_sys/snd0.at9` | Plays while the tile is selected. Looped 48 kHz ATRAC9, mono or stereo, under 2 MiB once encoded: about 15 seconds is typical, 87.3 seconds at most. Mixed around -18 LUFS with peaks at or below -1 dBTP so it does not jump out of the home screen. Needs a legally obtained ATRAC9 encoder. |
| Store screenshots | `screenshots/*.png` | the ProsperoStore listing | 1920 x 1080 PNGs of the real app on a console: Volumes, a scan, Explore. Taken during console validation; no art needed from you. |

## Style

The app's look is in `src/concepts/disk.cpp`: a dark navy-to-violet gradient
(`#0b0e1a` to `#191533`), frosted panels, and a sunburst whose branches are
coloured `#4f8cff`, `#ff6b6b`, `#ffb84d`, `#3fd18b`, `#b07cff`, `#2ec5d3`,
`#f06fb3` and `#9bd64b`. A sunburst (concentric ring segments around a
disc) or a disk-and-rings motif would match it. Text, if any, is "Prospero
Disk" (the console prints the name under the tile anyway, so the icon reads
better without it). No PlayStation logos, buttons or trademarks.

## Converting

`tools/prepare-assets.sh` runs the conversion (DirectXTex `texconv` for the
DDS files, FFmpeg and an ATRAC9 encoder for the music) and
`tools/validate-assets.sh` checks the results; `make` runs the check too.

```bash
tools/prepare-assets.sh --icon art/icon.png --background art/background.png \
    [--launch-background art/launch.png] [--audio art/music.wav --audio-duration 15]
```

[docs/platform/PRESENTATION_ASSETS.md](../docs/platform/PRESENTATION_ASSETS.md)
has the details. Artwork is the owner's: add a licence line to
`THIRD_PARTY_NOTICES.md` for anything not made for this project.
