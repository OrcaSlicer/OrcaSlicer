#!/usr/bin/env bash
#
# smoke_gui.sh — Phase 0.3 GUI launch smoke.
#
# Launches orca-slicer for a short window (default 5s) and checks it survived
# wx/OpenGL/GTK init without crashing. Pass; the binary's main window came up
# and we killed it on purpose. Fail; the binary exited non-zero on its own
# (segfault, abort, missing resource, etc.).
#
# Usage:
#   engine/scripts/smoke_gui.sh [path-to-orca-slicer] [seconds]
#
# Defaults: discovers build/src/Release/orca-slicer, runs for 5 seconds.
# Requires a display (DISPLAY or WAYLAND_DISPLAY). Skips with a clear message
# if neither is set (headless CI: use Xvfb upstream).

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

SLICER="${1:-}"
SECONDS_TO_RUN="${2:-5}"

if [[ -z "$SLICER" ]]; then
    SLICER="$(find "$REPO_ROOT/build" -name 'orca-slicer' -type f -executable 2>/dev/null | head -1)"
fi

if [[ -z "$SLICER" || ! -x "$SLICER" ]]; then
    echo "smoke_gui: could not find orca-slicer (pass path as 1st arg)" >&2
    exit 2
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "smoke_gui: SKIP — no display (set DISPLAY/WAYLAND_DISPLAY, or use Xvfb)"
    exit 0
fi

ENGINE_DIR="$(dirname "$(find "$REPO_ROOT/build" -name 'liborca-engine.so' -type f 2>/dev/null | head -1)")"
if [[ -n "$ENGINE_DIR" ]]; then
    export LD_LIBRARY_PATH="$ENGINE_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

LOGFILE="$(mktemp -t orca-smoke-XXXXXX.log)"
trap 'rm -f "$LOGFILE"' EXIT

echo "smoke_gui: launching $SLICER for ${SECONDS_TO_RUN}s"
echo "smoke_gui: engine .so dir = ${ENGINE_DIR:-<not set>}"

# Run with timeout. timeout(1) returns 124 when it terminates the child on
# expiry — that's our PASS signal (the slicer was still running when we killed
# it, which means it survived startup). Any other non-zero is a real failure.
set +e
"$SLICER" >"$LOGFILE" 2>&1 &
PID=$!

slept=0
while kill -0 "$PID" 2>/dev/null && (( slept < SECONDS_TO_RUN )); do
    sleep 1
    slept=$((slept + 1))
done

if kill -0 "$PID" 2>/dev/null; then
    # Still running — graceful kill, then escalate if needed.
    kill -TERM "$PID" 2>/dev/null || true
    sleep 1
    kill -KILL "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null
    echo "smoke_gui: OK — slicer ran for ${slept}s and was killed cleanly."
    rc=0
else
    # Already exited on its own — that's bad in the smoke window.
    wait "$PID" 2>/dev/null
    rc=$?
    echo "smoke_gui: FAIL — slicer exited on its own after ${slept}s (rc=$rc)" >&2
    echo "smoke_gui: last 30 lines of output:" >&2
    tail -30 "$LOGFILE" >&2
fi

exit "$rc"
