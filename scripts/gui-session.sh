#!/usr/bin/env bash
# Bring the headless GUI up on a VNC-served X display, ready to drive or to attach Remmina to.
#
# Runs INSIDE the long-lived GUI container (see the header of scripts/docker-iter-build.sh for how
# that container is created). Idempotent: safe to re-run to recover a session whose app died.
#
#   docker exec <container> /OrcaSlicer/scripts/gui-session.sh          # launch + settle
#   docker exec <container> /OrcaSlicer/scripts/gui-session.sh --status # report, change nothing
#
# WHY THIS EXISTS. Dismissing the first-run dialogs by computing the titlebar close box from
# `xdotool getwindowgeometry --shell` and clicking it went wrong whenever the dialog had already
# closed: the eval left the geometry variables stale or empty, the click landed at a garbage
# coordinate, and it repeatedly hit the Sketch button in the toolbar underneath, so the app came up
# in sketch mode with a stray Sketch feature. Three of those in one session.
#
# The titlebar click is nevertheless the RIGHT mechanism and is kept. `xdotool windowclose` looks
# cleaner but kills the app: it destroys the GdkWindow out from under the dialog and the process
# dies with "GdkWindow unexpectedly destroyed", three GLib-GObject criticals and a segfault
# (measured 2026-07-30). Escape does not close the Setup Wizard either. So the fix is not a
# different mechanism, it is refusing to click on geometry we have not validated.
set -euo pipefail

DISP="${DISP:-:11}"
GEOM="${GEOM:-1920x1080}"
BIN="${BIN:-/OrcaSlicer/build/src/Release/orca-slicer}"
# Packaging cannot bundle python (the deps python layer carries a doubled-DESTDIR RUNPATH), so the
# build-tree binary needs the deps libpython on the path. Packaging-only issue; the app runs fine.
LIBPY="/OrcaSlicer/deps/build/destdir/usr/local/libpython/lib"
LIBPY2="/OrcaSlicer/build/src/Release/python/lib"
LOG="${LOG:-/tmp/gui-session.log}"

export DISPLAY="$DISP" HOME=/root
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export LD_LIBRARY_PATH="$LIBPY:$LIBPY2:${LD_LIBRARY_PATH:-}"
mkdir -p /root/.config          # startup dies in boost::filesystem::create_directory without this

# Skip zombies. This container accumulates <defunct> instances of the app across runs, and pgrep
# matches them, so the naive "first match" reported a dead pid as if the session were healthy.
app_pid() {
    local p
    for p in $(pgrep -f "$(basename "$BIN")" 2>/dev/null); do
        [ "$(awk "{print \$3}" "/proc/$p/stat" 2>/dev/null)" = "Z" ] && continue
        echo "$p"; return 0
    done
    return 1
}

status() {
    echo "display : $(pgrep -f "Xvfb $DISP" >/dev/null && echo up || echo DOWN)"
    echo "wm      : $(pgrep -x openbox   >/dev/null && echo up || echo DOWN)"
    if pgrep -x x11vnc >/dev/null; then echo "vnc     : up on :5900"
    elif ! command -v x11vnc >/dev/null; then echo "vnc     : n/a (x11vnc not installed here)"
    else echo "vnc     : DOWN"; fi
    local p; p="$(app_pid || true)"
    echo "app     : ${p:-DOWN}"
    # WHICH binary is on screen, not just that something is. A pid alone cannot tell you whether
    # you are looking at the build you just linked or one from last week, and that is precisely
    # the question every rig verification is asking.
    [ -n "${p:-}" ] && echo "binary  : $(readlink -f "/proc/$p/exe" 2>/dev/null || echo unknown)"
    [ -n "${p:-}" ] && echo "windows : $(xdotool search --name . getwindowname %@ 2>/dev/null | paste -sd'|' -)"
    return 0
}

[ "${1:-}" = "--status" ] && { status; exit 0; }

# --- desktop: Xvfb, a window manager, and the VNC server ------------------------------------
# openbox is REQUIRED: without it xdotool windowactivate aborts with "windowmanager claims not to
# support _NET_ACTIVE_WINDOW" and dialogs never take focus.
pgrep -f "Xvfb $DISP" >/dev/null || { nohup Xvfb "$DISP" -screen 0 "${GEOM}x24" -nolisten tcp >/tmp/xvfb.log 2>&1 & sleep 3; }
pgrep -x openbox      >/dev/null || { nohup openbox >/tmp/openbox.log 2>&1 & sleep 1; }
if ! pgrep -x x11vnc >/dev/null && command -v x11vnc >/dev/null; then
    AUTH=()
    [ -f /root/.vnc/passwd ] && AUTH=(-rfbauth /root/.vnc/passwd)
    nohup x11vnc -display "$DISP" -rfbport 5900 "${AUTH[@]}" -forever -shared -noxdamage \
        >/tmp/x11vnc.log 2>&1 &
    sleep 2
fi

# --- app ------------------------------------------------------------------------------------
# Kill by BASENAME, not by "$BIN". The app enforces a single instance, so an older copy launched
# from a DIFFERENT path (the packaged build/package/bin/ one, say, when BIN points at the freshly
# linked build/src/Release/ one) survives a path-matched pkill, keeps the instance lock, and the
# new process exits seconds after loading fonts — leaving no error anywhere. app_pid() below has
# always matched by basename, so status then reported that stale process as a healthy session:
# the launch looked green while the window on screen was days old. Measured 2026-08-02, where it
# nearly passed a UI change against a Jul-30 binary. The killer and the reporter must agree on
# what counts as "the app".
pkill -9 -f "$(basename "$BIN")" 2>/dev/null || true
sleep 2
nohup "$BIN" >"$LOG" 2>&1 &
echo "launched $(basename "$BIN") pid $!"

# Wait for the main window rather than sleeping a fixed amount: cold starts vary a lot under
# software GL, and a fixed sleep either wastes time or races.
for _ in $(seq 1 40); do
    xdotool search --name "Untitled" >/dev/null 2>&1 && break
    sleep 1
done

# --- first-run dialogs ----------------------------------------------------------------------
# Click the titlebar close box, but only on geometry we have just read for a window that still
# exists, and only if the resulting point is inside the screen. Every variable is unset first so a
# failed read cannot leave the previous dialog's numbers behind — that is the whole bug.
screen_w="${GEOM%x*}"; screen_h="${GEOM#*x}"
close_dialog() {
    local name="$1" id X Y WIDTH HEIGHT cx cy
    id="$(xdotool search --name "$name" 2>/dev/null | head -1 || true)"
    [ -z "$id" ] && return 1
    unset X Y WIDTH HEIGHT
    eval "$(xdotool getwindowgeometry --shell "$id" 2>/dev/null || true)"
    # All four must be present and numeric: an empty or stale read is how the stray click happened.
    for v in "${X:-}" "${Y:-}" "${WIDTH:-}" "${HEIGHT:-}"; do
        [[ "$v" =~ ^-?[0-9]+$ ]] || { echo "  $name: unreadable geometry, not clicking"; return 1; }
    done
    cx=$((X + WIDTH - 11)); cy=$((Y - 31))          # openbox decoration: close box above the frame
    if [ "$cx" -lt 0 ] || [ "$cy" -lt 0 ] || [ "$cx" -ge "$screen_w" ] || [ "$cy" -ge "$screen_h" ]; then
        echo "  $name: close box at ${cx},${cy} is off-screen, not clicking"; return 1
    fi
    echo "  $name: closing via titlebar at ${cx},${cy}"
    xdotool mousemove "$cx" "$cy" click 1
    sleep 2
    return 0
}
for name in "Setup Wizard" "New version"; do
    for _ in 1 2 3; do close_dialog "$name" || break; done
done

# --- main window ----------------------------------------------------------------------------
main="$(xdotool search --name "Untitled" 2>/dev/null | head -1 || true)"
if [ -n "$main" ]; then
    xdotool windowmove "$main" 0 0 windowsize "$main" ${GEOM/x/ } 2>/dev/null || true
    xdotool windowactivate "$main" 2>/dev/null || true
    sleep 2
fi

echo "--- session ---"
status
