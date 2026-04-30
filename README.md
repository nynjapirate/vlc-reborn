# vlc-reborn

A fork of [VLC media player](https://code.videolan.org/videolan/vlc) **3.0.20 (Vetinari)** with Qt-side UI extensions:

- **Hover-thumbnail previews** on the seek bar (libavformat-direct backend, ~5–15 ms per frame, no subprocess).
- **"Thumbnail List" playlist view** with per-row video thumbnails. User-configurable thumbnail size (Small / Medium / Large / XL / XXL) via right-click → *Thumbnail size*.
- **XDG thumbnail-cache reuse** — the playlist thumbnailer first checks `~/.cache/thumbnails/` (populated by file managers and `ffmpegthumbnailer`) before falling back to a libav decode. Near-instant on already-browsed media.
- **Size column** showing on-disk file size (with `Sort by Size` working both via the column header and the right-click *Sort by* submenu — backed by a custom `SORT_SIZE` enum added to `libvlccore`).
- **Decoded paths in the URI/Location column** — `/home/user/My Videos/clip.mp4` instead of `file:///home/user/My%20Videos/clip.mp4`.
- **Jump-to-click on playlist scrollbars** — clicking the trough sets the slider to that position rather than paging.
- **Atomic-state TimeTooltip** — fixes the "phantom thumbnail at lower-right of cursor" race on fast scrubs.
- **`folder_siblings` interface plugin** — auto-loads the on-disk siblings of an opened file as a playlist (use with `--extraintf=folder_siblings`).

Most changes live under `modules/gui/qt/`; only the `SORT_SIZE` addition touches `libvlccore` (`include/vlc_playlist.h`, `src/playlist/sort.c`).

## Pre-built binaries

Each [release](https://github.com/nynjapirate/vlc-reborn/releases) ships:

- **`VLC_Reborn-x86_64.AppImage`** (~120 MB) — single-file, runs on any x86_64 Linux with glibc 2.31+. Bundles libavformat / libavcodec / Qt5 / libxcb so it's not exposed to host FFmpeg ABI changes.
- **`vlc-reborn.deb`** (~30 MB) — installs to `/opt/vlc-reborn/` with a `vlc-reborn` wrapper at `/usr/bin/`. Targets Ubuntu 24.04 / Debian trixie (the Qt5 `t64` ABI line). Coexists with stock `vlc`.

## Build from source

Standard VLC autotools build. Tested with GCC 13.x, Qt 5.15, FFmpeg 6.x:

```bash
./bootstrap
./configure --prefix=/tmp/vlc-reborn-3-install
make -j$(nproc)
./run.sh                 # in-tree launcher; sets VLC_PLUGIN_PATH
```

Re-package binaries:

```bash
./package-appimage.sh    # → ./VLC_Reborn-x86_64.AppImage
./package-deb.sh         # → ./vlc-reborn.deb
```

## Relationship to upstream VLC

This is a **practical** fork, not a [GitHub fork](https://docs.github.com/en/get-started/quickstart/fork-a-repo) of `videolan/vlc`. The repo was created independently with the VLC 3.0.20 source as its initial commit; modifications are added as ordinary commits on top.

If you're looking for upstream VLC, go to [code.videolan.org/videolan/vlc](https://code.videolan.org/videolan/vlc) — that's the canonical source. This repo doesn't track upstream and isn't intended to be merged back; it exists as a personal fork that ships UI features I (the author) want without waiting for upstream review cycles.

## License

GPL-2.0-or-later, inherited from VLC. See `COPYING`. Original VLC `README` preserved in the tree.
