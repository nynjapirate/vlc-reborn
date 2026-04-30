#!/bin/bash
# package-appimage.sh — build a vlc-reborn.AppImage from the in-tree build.
#
# Approach: skip `make install` (the configured prefix is a temp dir; we
# don't want to reconfigure + 25-min full rebuild) and instead copy the
# built artifacts into an AppDir directly. linuxdeploy + the qt plugin
# walk NEEDED dependencies and bundle libQt5* / libavformat / libxcb /
# libsharedmem etc. from the host, producing a self-contained AppImage.
#
# Usage:
#   ./package-appimage.sh           # build AppImage at ./VLC_Reborn-x86_64.AppImage
#   ./package-appimage.sh --clean   # also wipe .appimage-tools/ first
#
# Idempotent: external tools are cached in .appimage-tools/. AppDir is
# rebuilt fresh on each run.
#
# Output:
#   ./vlc-reborn.AppDir/                # the staged tree
#   ./VLC_Reborn-x86_64.AppImage        # the final single-file artifact
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APPDIR="$HERE/vlc-reborn.AppDir"
TOOLS="$HERE/.appimage-tools"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$TOOLS"
fi

# Some hosts have no FUSE / libfuse2; let nested AppImages run by extraction.
export APPIMAGE_EXTRACT_AND_RUN=1

# ---------------------------------------------------------------------------
# 0. Sanity checks — make sure the tree has been built recently.
# ---------------------------------------------------------------------------
if [[ ! -x "$HERE/bin/vlc-static" ]]; then
    echo "ERROR: $HERE/bin/vlc-static not found. Run 'make' first." >&2
    exit 1
fi
if [[ -z "$(find "$HERE/modules/.libs" -maxdepth 1 -name 'lib*_plugin.so' -print -quit 2>/dev/null)" ]]; then
    echo "ERROR: no plugin .so files in modules/.libs/. Build is incomplete?" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Download external tools (cached). The "continuous" tag follows the
#    upstream build and is what AppImage docs recommend.
# ---------------------------------------------------------------------------
mkdir -p "$TOOLS"
fetch() {
    local out="$TOOLS/$1" url="$2"
    if [[ ! -x "$out" ]]; then
        echo "Fetching $1 ..."
        wget --quiet --show-progress -O "$out" "$url"
        chmod +x "$out"
    fi
}
fetch linuxdeploy.AppImage \
    'https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage'
fetch linuxdeploy-plugin-qt.AppImage \
    'https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage'
fetch appimagetool.AppImage \
    'https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage'

# linuxdeploy auto-finds its plugins on $PATH; symlink one in.
export PATH="$TOOLS:$PATH"

# ---------------------------------------------------------------------------
# 2. Build the AppDir layout.
#
#    AppDir/
#      AppRun                       # entry point (sets env, exec's vlc)
#      vlc-reborn.desktop           # XDG desktop entry
#      vlc-reborn.png               # icon (256x256)
#      usr/
#        bin/vlc                    # the binary
#        lib/                       # bundled .so files (linuxdeploy fills)
#        lib/vlc/plugins/           # all of VLC's plugin .so files
#        share/vlc/                 # lua extensions, art, etc.
#        share/icons/hicolor/.../   # icons (for the .desktop file)
# ---------------------------------------------------------------------------
echo "Staging AppDir at $APPDIR ..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/lib/vlc/plugins" \
         "$APPDIR/usr/share/vlc" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# 2a. Binary + core libs
install -m 755 "$HERE/bin/vlc-static" "$APPDIR/usr/bin/vlc"
# vlc-cache-gen creates the plugin cache index on first launch — bundle it
# so first-run is fast (otherwise vlc scans every plugin .so for metadata).
[[ -x "$HERE/bin/vlc-cache-gen" ]] && \
    install -m 755 "$HERE/bin/vlc-cache-gen" "$APPDIR/usr/bin/vlc-cache-gen"

# Versioned core libs — copy the actual file then recreate symlinks
for so in "$HERE/src/.libs/libvlccore.so.9.0.1" \
          "$HERE/lib/.libs/libvlc.so.5.6.1"; do
    [[ -f "$so" ]] || continue
    base=$(basename "$so")
    cp "$so" "$APPDIR/usr/lib/$base"
    soname=${base%.*.*}      # libvlccore.so.9
    ln -sf "$base" "$APPDIR/usr/lib/$soname"
    short=${soname%.*}        # libvlccore.so
    ln -sf "$base" "$APPDIR/usr/lib/$short"
done

# 2b. Plugins. find -L follows libtool symlinks; we want the real .so.
echo "Copying VLC plugins ..."
find "$HERE/modules/.libs" -maxdepth 1 -name 'lib*_plugin.so' -type f \
    -exec install -m 644 {} "$APPDIR/usr/lib/vlc/plugins/" \;
plugin_count=$(find "$APPDIR/usr/lib/vlc/plugins" -name '*.so' | wc -l)
echo "  bundled $plugin_count plugins"

# 2c. Share data (lua extensions, art, locale, icons)
for d in lua skins2 hrtfs; do
    [[ -d "$HERE/share/$d" ]] && cp -a "$HERE/share/$d" "$APPDIR/usr/share/vlc/"
done
[[ -d "$HERE/share/locale" ]] && cp -a "$HERE/share/locale" "$APPDIR/usr/share/"

# Reshape VLC's flat share/icons/<size>/vlc.png layout into the standard
# hicolor theme structure that .desktop's Icon=vlc-reborn expects. Only
# the 256x256 entry is required for the AppImage's outer icon, but
# bundling the rest gives correct icons in app menus too.
for d in "$HERE/share/icons"/*x*; do
    [[ -d "$d" ]] || continue
    sz=$(basename "$d")
    mkdir -p "$APPDIR/usr/share/icons/hicolor/$sz/apps"
    [[ -f "$d/vlc.png" ]] && \
        install -m 644 "$d/vlc.png" \
            "$APPDIR/usr/share/icons/hicolor/$sz/apps/vlc-reborn.png"
done

# Top-level icon for the AppImage (must be at AppDir root)
install -m 644 "$HERE/share/icons/256x256/vlc.png" "$APPDIR/vlc-reborn.png"

# 2d. Desktop file. Don't conflict with stock vlc.desktop's StartupWMClass
#     so a user with both installed gets distinct taskbar grouping.
cat > "$APPDIR/vlc-reborn.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=VLC Reborn
GenericName=Media Player
Comment=Modified VLC fork — playlist thumbnails, hover previews, file-size column, sort by size
Exec=vlc %U
Icon=vlc-reborn
Terminal=false
Categories=AudioVideo;Audio;Video;Player;Recorder;
StartupWMClass=vlc-reborn
MimeType=video/mp4;video/x-matroska;video/webm;video/x-msvideo;video/quicktime;video/mpeg;audio/mpeg;audio/x-flac;audio/ogg;audio/x-wav;application/ogg;
EOF
cp "$APPDIR/vlc-reborn.desktop" "$APPDIR/usr/share/applications/"

# 2e. AppRun — sets env so the bundled binary finds its plugins/data.
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"

# Plugin discovery. VLC checks VLC_PLUGIN_PATH first, then a hardcoded
# build-time path; we override here so the AppImage is fully self-contained.
export VLC_PLUGIN_PATH="$HERE/usr/lib/vlc/plugins"
export VLC_DATA_PATH="$HERE/usr/share/vlc"

# Library path — bundled libavformat / libavcodec / Qt5 / libvlccore live
# under usr/lib. linuxdeploy --plugin qt also bundles platform plugins
# under usr/plugins, exposed via QT_QPA_PLATFORM_PLUGIN_PATH.
export LD_LIBRARY_PATH="$HERE/usr/lib:$HERE/usr/lib/vlc:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$HERE/usr/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"

# Pre-generate / refresh the plugin cache on first run if missing. Avoids
# the multi-second startup scan every launch.
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vlc-reborn"
if [[ ! -f "$cache_dir/plugins-$(uname -m).dat" ]] && [[ -x "$HERE/usr/bin/vlc-cache-gen" ]]; then
    mkdir -p "$cache_dir"
    "$HERE/usr/bin/vlc-cache-gen" "$HERE/usr/lib/vlc/plugins" >/dev/null 2>&1 || true
fi

exec "$HERE/usr/bin/vlc" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# ---------------------------------------------------------------------------
# 3. linuxdeploy: walk NEEDED of the binary AND each plugin, bundle their
#    libraries into AppDir/usr/lib. The qt plugin handles Qt5's quirks
#    (platform plugins, qml stuff, libxcb-related shenanigans).
# ---------------------------------------------------------------------------
echo "Bundling shared libraries with linuxdeploy ..."

# Build the --executable list: the main binary + every plugin we bundled.
# linuxdeploy walks each one's NEEDED entries and copies the closure of
# missing libs into AppDir/usr/lib.
exec_args=( --executable "$APPDIR/usr/bin/vlc" )
while IFS= read -r p; do
    exec_args+=( --executable "$p" )
done < <(find "$APPDIR/usr/lib/vlc/plugins" -name '*.so' -type f)

# QMAKE env is required by the qt plugin to discover Qt's install paths.
export QMAKE="${QMAKE:-$(command -v qmake-qt5 || command -v qmake)}"

# Critical: prepend AppDir/usr/lib to LD_LIBRARY_PATH so when linuxdeploy
# resolves vlc-static's NEEDED entry for `libvlc.so.5`, it finds OUR
# patched copy (the one we staged in step 2a) before any system-wide
# stock-VLC install. Same defense for libvlccore.so.9.
export LD_LIBRARY_PATH="$APPDIR/usr/lib:${LD_LIBRARY_PATH:-}"

"$TOOLS/linuxdeploy.AppImage" \
    --appdir "$APPDIR" \
    "${exec_args[@]}" \
    --desktop-file "$APPDIR/vlc-reborn.desktop" \
    --icon-file "$APPDIR/vlc-reborn.png" \
    --plugin qt

# ---------------------------------------------------------------------------
# 4. Pack into a single .AppImage. We use appimagetool directly (rather
#    than linuxdeploy --output appimage) because the latter triggers an
#    extra deps walk; we already did that.
# ---------------------------------------------------------------------------
echo "Packing AppImage ..."
ARCH=x86_64 "$TOOLS/appimagetool.AppImage" "$APPDIR" "$HERE/VLC_Reborn-x86_64.AppImage"

echo
echo "Done. Built: $HERE/VLC_Reborn-x86_64.AppImage"
echo "Size: $(du -h "$HERE/VLC_Reborn-x86_64.AppImage" | cut -f1)"
echo
echo "Test:  $HERE/VLC_Reborn-x86_64.AppImage --version"
echo "Run:   $HERE/VLC_Reborn-x86_64.AppImage [files...]"
