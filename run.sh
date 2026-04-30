#!/bin/bash
# In-tree launcher for vlc-reborn (VLC 3.0.20 + hover-thumbnail port).
# Avoids the need to `make install` while developing.
#
# Plugin path order:
#   1. Our build's modules/  — includes hover-thumbnail Qt module + libqt
#   2. /usr/lib/.../vlc/plugins  — stock-VLC plugin dir, brings in third-
#      party plugins the user has installed (pause_click, etc.) since both
#      builds are VLC 3.0.20 with identical libvlccore SO version (.9).
# Lua extensions like vlc-delete auto-load from ~/.local/share/vlc/lua/.
#
# Flag rationale:
#   --no-one-instance        : don't D-Bus-handoff to a stock-VLC instance.
#   --vout=xcb_xv            : bypasses GL+VAAPI which fails on nvidia-open
#                              (vaInitialize: unknown libva error). XVideo
#                              path uses straight YUV → display.
#   --avcodec-hw=none        : disable VAAPI hardware decode for the same
#                              reason — software decoding via dav1d/avcodec.
here="$(cd "$(dirname "$0")" && pwd)"
stock_plugins="/usr/lib/x86_64-linux-gnu/vlc/plugins"
if [ -d "$stock_plugins" ]; then
    export VLC_PLUGIN_PATH="$here/modules:$stock_plugins"
else
    export VLC_PLUGIN_PATH="$here/modules"
fi
exec "$here/bin/vlc" \
    --no-one-instance \
    --vout=xcb_xv \
    --avcodec-hw=none \
    --extraintf=folder_siblings \
    "$@"
