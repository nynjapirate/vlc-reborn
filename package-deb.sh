#!/bin/bash
# package-deb.sh — build a vlc-reborn.deb from the in-tree build.
#
# Layout choice: install everything under /opt/vlc-reborn/ so the package
# can coexist with a stock VLC install (different binary name, separate
# plugin path, separate libvlccore SO so the linker can't accidentally
# pick our patched copy for stock vlc). The user-facing entry point is
# a tiny shell wrapper at /usr/bin/vlc-reborn that sets VLC_PLUGIN_PATH
# / LD_LIBRARY_PATH and execs the real binary.
#
# Trade-off vs the AppImage: this .deb is ABI-coupled to the host's Qt5
# and FFmpeg minor versions (we link, we don't bundle). It targets the
# user's current Ubuntu 24.04 / Debian-trixie line — `dpkg -i` on an
# older release will fail at install time complaining about Qt/libav
# version mismatches. That's by design (smaller artifact, integrates
# with apt) — the AppImage is the answer for cross-distro distribution.
#
# Usage:
#   ./package-deb.sh                # builds ./vlc-reborn.deb
#   ./package-deb.sh --version 1.2  # override the package version
#
# Output:
#   ./vlc-reborn-pkg/    # staged .deb tree (preserved for inspection)
#   ./vlc-reborn.deb     # the final package
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/vlc-reborn-pkg"
DEB_OUT="$HERE/vlc-reborn.deb"

PKG_VERSION="3.0.20-reborn1"
if [[ "${1:-}" == "--version" && -n "${2:-}" ]]; then
    PKG_VERSION="$2"
fi

# ---------------------------------------------------------------------------
# 0. Sanity checks.
# ---------------------------------------------------------------------------
if [[ ! -x "$HERE/bin/vlc-static" ]]; then
    echo "ERROR: $HERE/bin/vlc-static not found. Run 'make' first." >&2
    exit 1
fi
if ! command -v dpkg-deb >/dev/null; then
    echo "ERROR: dpkg-deb not found (install dpkg-dev)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Stage the package tree.
#
#    /opt/vlc-reborn/
#      bin/vlc                 # the binary (called by the wrapper)
#      bin/vlc-cache-gen
#      lib/libvlccore.so.9.0.1 + symlinks
#      lib/libvlc.so.5.6.1     + symlinks
#      lib/vlc/plugins/*.so    # all built plugins
#      share/vlc/lua/...
#    /usr/bin/vlc-reborn       # wrapper script
#    /usr/share/applications/vlc-reborn.desktop
#    /usr/share/icons/hicolor/<size>/apps/vlc-reborn.png
# ---------------------------------------------------------------------------
echo "Staging package tree at $PKG ..."
rm -rf "$PKG"
mkdir -p "$PKG/DEBIAN" \
         "$PKG/opt/vlc-reborn/bin" \
         "$PKG/opt/vlc-reborn/lib/vlc/plugins" \
         "$PKG/opt/vlc-reborn/share/vlc" \
         "$PKG/usr/bin" \
         "$PKG/usr/share/applications" \
         "$PKG/usr/share/doc/vlc-reborn"

# 1a. Binary + cache-gen helper
install -m 755 "$HERE/bin/vlc-static" "$PKG/opt/vlc-reborn/bin/vlc"
# bin/vlc-cache-gen at the top level is a libtool shell wrapper that
# re-execs the real binary in bin/.libs/vlc-cache-gen and fails when
# bin/.libs/ isn't present in /opt. Always copy the actual ELF.
if [[ -x "$HERE/bin/.libs/vlc-cache-gen" ]]; then
    install -m 755 "$HERE/bin/.libs/vlc-cache-gen" \
        "$PKG/opt/vlc-reborn/bin/vlc-cache-gen"
fi

# 1b. Patched libvlccore + libvlc (versioned file + ABI symlinks). vlc-static
#     links against libvlc.so.5; libvlc loads libvlccore.so.9 transitively.
for so in "$HERE/src/.libs/libvlccore.so.9.0.1" \
          "$HERE/lib/.libs/libvlc.so.5.6.1"; do
    [[ -f "$so" ]] || continue
    base=$(basename "$so")
    cp "$so" "$PKG/opt/vlc-reborn/lib/$base"
    soname=${base%.*.*}      # libvlccore.so.9
    ln -sf "$base" "$PKG/opt/vlc-reborn/lib/$soname"
    ln -sf "$base" "$PKG/opt/vlc-reborn/lib/${soname%.*}"  # libvlccore.so
done

# 1c. Plugins
echo "Copying VLC plugins ..."
find "$HERE/modules/.libs" -maxdepth 1 -name 'lib*_plugin.so' -type f \
    -exec install -m 644 {} "$PKG/opt/vlc-reborn/lib/vlc/plugins/" \;
plugin_count=$(find "$PKG/opt/vlc-reborn/lib/vlc/plugins" -name '*.so' | wc -l)
echo "  bundled $plugin_count plugins"

# 1d. Share data
for d in lua skins2 hrtfs; do
    [[ -d "$HERE/share/$d" ]] && cp -a "$HERE/share/$d" "$PKG/opt/vlc-reborn/share/vlc/"
done
[[ -d "$HERE/share/locale" ]] && \
    cp -a "$HERE/share/locale" "$PKG/opt/vlc-reborn/share/"

# 1e. Icons in standard hicolor layout
for d in "$HERE/share/icons"/*x*; do
    [[ -d "$d" ]] || continue
    sz=$(basename "$d")
    mkdir -p "$PKG/usr/share/icons/hicolor/$sz/apps"
    [[ -f "$d/vlc.png" ]] && \
        install -m 644 "$d/vlc.png" \
            "$PKG/usr/share/icons/hicolor/$sz/apps/vlc-reborn.png"
done

# 1f. /usr/bin/vlc-reborn — the user-facing wrapper. /opt-installed apps
#     can't rely on PATH; this wrapper sets VLC_PLUGIN_PATH and the
#     library path so the bundled libvlccore wins over any stock VLC
#     copy already on the system.
cat > "$PKG/usr/bin/vlc-reborn" <<'WRAPPER'
#!/bin/bash
HERE=/opt/vlc-reborn
# We pin LD_LIBRARY_PATH to /opt/vlc-reborn/lib so the dynamic loader
# picks our patched libvlccore.so.9 over any stock-VLC copy.
# libvlccore's config_GetLibDir() then auto-derives the plugin path
# from where libvlccore was loaded — i.e. /opt/vlc-reborn/lib/vlc/plugins
# — so we don't need (and don't want) to also set VLC_PLUGIN_PATH:
# adding the same path twice causes every plugin to load twice and
# every config option to duplicate in the Preferences UI.
#
# We DO set VLC_PLUGIN_PATH to an explicit empty string. That defeats
# bin/vlc.c's dev-mode setenv() which would otherwise inject the
# original build-tree path baked in at compile time
# (TOP_BUILDDIR/modules), leaking the developer's source tree into
# every installed copy. The empty string is "set" as far as the
# overwrite=0 check in bin/vlc.c is concerned, so the dev path never
# gets injected.
export VLC_PLUGIN_PATH=""
export VLC_DATA_PATH="$HERE/share/vlc"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/bin/vlc" "$@"
WRAPPER
chmod 755 "$PKG/usr/bin/vlc-reborn"

# 1g. Desktop file. StartupWMClass differs from stock 'vlc' so taskbars
#     group the two distinctly.
cat > "$PKG/usr/share/applications/vlc-reborn.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=VLC Reborn
GenericName=Media Player
Comment=Modified VLC fork — playlist thumbnails, hover previews, file-size column, sort by size
Exec=vlc-reborn %U
Icon=vlc-reborn
Terminal=false
Categories=AudioVideo;Audio;Video;Player;Recorder;
StartupWMClass=vlc-reborn
MimeType=video/mp4;video/x-matroska;video/webm;video/x-msvideo;video/quicktime;video/mpeg;audio/mpeg;audio/x-flac;audio/ogg;audio/x-wav;application/ogg;
EOF

# 1h. Doc directory (apt expects every package to ship a copyright file)
cat > "$PKG/usr/share/doc/vlc-reborn/copyright" <<EOF
vlc-reborn — fork of VLC media player

Upstream VLC copyright VideoLAN. Licensed under the GNU General Public
License version 2 or later. See /opt/vlc-reborn for the bundled COPYING.
EOF
[[ -f "$HERE/COPYING" ]] && \
    cp "$HERE/COPYING" "$PKG/usr/share/doc/vlc-reborn/COPYING"

# ---------------------------------------------------------------------------
# 2. DEBIAN/control — package metadata. Depends are pinned to Ubuntu
#    24.04 / Debian trixie (the t64 ABI transition) since that's what
#    the binary was built against. Older releases need a separate build.
# ---------------------------------------------------------------------------
INSTALLED_SIZE=$(du -sk "$PKG" | cut -f1)
cat > "$PKG/DEBIAN/control" <<EOF
Package: vlc-reborn
Version: $PKG_VERSION
Architecture: amd64
Maintainer: nynjapirate <trashy@collector.org>
Section: video
Priority: optional
Installed-Size: $INSTALLED_SIZE
Depends: libc6, libstdc++6,
 libavformat60, libavcodec60, libavutil58, libswscale7,
 libqt5core5t64, libqt5gui5t64, libqt5widgets5t64, libqt5svg5,
 libqt5network5t64, libqt5x11extras5,
 libx11-6, libxext6, libxcb1, libxcb-shm0, libxcb-xv0, libxcb-render0,
 libxcb-composite0, libxcb-keysyms1, libxcb-randr0, libxcb-xkb1,
 libdbus-1-3, libgcrypt20, libidn12, libxml2, fontconfig
Recommends: ffmpeg
Description: Modified VLC fork — playlist thumbnails, hover previews, sort by size
 vlc-reborn is a fork of VLC 3.0.20 (Vetinari) with several Qt UI
 extensions:
 .
  * Hover-thumbnail previews on the seek bar (libavformat-direct backend)
  * "Thumbnail List" playlist view with per-row video thumbnails,
    user-configurable thumbnail size, XDG thumbnail-cache reuse
  * Size column showing on-disk file size; sort by size
  * Decoded human-readable paths in the URI/Location column
  * Jump-to-click on playlist scrollbars
  * Custom folder_siblings interface plugin
 .
 Installs to /opt/vlc-reborn and exposes a 'vlc-reborn' wrapper at
 /usr/bin/vlc-reborn so it can coexist with a stock 'vlc' install.
EOF

# 2a. postinst — refresh the desktop / icon caches so the launcher and
#     mime-type associations show up immediately. The `|| true` lines
#     are deliberately permissive: missing tools (minimal install) is
#     not a reason to fail the install.
cat > "$PKG/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
case "$1" in
    configure)
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database -q /usr/share/applications || true
        fi
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -q -f /usr/share/icons/hicolor || true
        fi
        # Pre-warm the plugin cache so first launch is fast.
        if [ -x /opt/vlc-reborn/bin/vlc-cache-gen ]; then
            /opt/vlc-reborn/bin/vlc-cache-gen /opt/vlc-reborn/lib/vlc/plugins \
                >/dev/null 2>&1 || true
        fi
        ;;
esac
exit 0
POSTINST
chmod 755 "$PKG/DEBIAN/postinst"

# 2b. postrm — refresh caches on removal too.
cat > "$PKG/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
case "$1" in
    remove|purge)
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database -q /usr/share/applications || true
        fi
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -q -f /usr/share/icons/hicolor || true
        fi
        ;;
esac
exit 0
POSTRM
chmod 755 "$PKG/DEBIAN/postrm"

# ---------------------------------------------------------------------------
# 3. Build the .deb.
# ---------------------------------------------------------------------------
echo "Building $DEB_OUT ..."
# Use xz compression — slower to build but ~30% smaller than the gzip
# default; the saving is worth it for a one-shot package build.
dpkg-deb --build --root-owner-group -Zxz "$PKG" "$DEB_OUT"

echo
echo "Done. Built: $DEB_OUT"
echo "Size: $(du -h "$DEB_OUT" | cut -f1)"
echo
echo "Inspect:  dpkg-deb -I  $DEB_OUT"
echo "Install:  sudo dpkg -i $DEB_OUT"
echo "         (or)  sudo apt install ./$(basename "$DEB_OUT")"
echo "Run:     vlc-reborn [files...]"
echo "Remove:   sudo apt remove vlc-reborn"
