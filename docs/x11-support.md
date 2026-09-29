# KDE Plasma X11 support

AnisPaper selects its monitor backend from `ANISPAPER_SESSION_TYPE` or
`XDG_SESSION_TYPE`. On X11 it enumerates connected XRandR outputs, reads the
active CRTC mode, and maps Plasma screens using KDE's `_KDE_SCREEN_INDEX`
output property. The Wayland path continues to use `wl_output` and KWin's
`kde_output_order_v1` protocol.

The wallpaper bridge remains the same POSIX shared-memory transport on both
sessions. Frame reads do not create an EGL or GLX context; Qt Quick uploads the
shared-memory image to the wallpaper scene graph. Renderer children use the
current session display and authorization environment. A missing X display is
reported instead of silently targeting `:0`.

## Verify on X11

Run these from a terminal opened inside the Plasma X11 session:

```bash
printf 'session=%s display=%s wayland=%s\n' "$XDG_SESSION_TYPE" "$DISPLAY" "$WAYLAND_DISPLAY"
xrandr --query
xrandr --prop | rg 'connected|_KDE_SCREEN_INDEX'
QT_QPA_PLATFORM=xcb anispaper-plasma-output-map
systemctl --user show anispaper.service -p MainPID -p Environment -p DropInPaths
journalctl --user -b -u anispaper.service --no-pager
```

The output-map JSON should list every active connector exactly once and use
zero-based Plasma screen numbers. Apply an animated Frame wallpaper to each
screen independently, confirm that right-click / Configure Desktop still
works, and restart the daemon to check restoration. For the reported setup the
expected connector names are `HDMI-A-0` and `DisplayPort-0`; do not infer
connectors by subtracting one from the number in a Wayland name.

## Wayland regression check

```bash
printf 'session=%s wayland=%s\n' "$XDG_SESSION_TYPE" "$WAYLAND_DISPLAY"
kscreen-doctor -o
QT_QPA_PLATFORM=wayland anispaper-plasma-output-map
```

Confirm existing `DP-1` / `HDMI-A-1` assignments and the Wayland renderer
behavior remain unchanged. The README keeps the established Wayland status as
Working; X11 stays marked Experimental until both-session visual checks have
been completed on the target desktop.
