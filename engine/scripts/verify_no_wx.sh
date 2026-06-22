#!/usr/bin/env bash
#
# verify_no_wx.sh — enforce the engine-boundary invariant.
#
# liborca-engine.so must not depend on wxWidgets. This script inspects the
# built shared library's *undefined* dynamic symbols (the things it needs from
# outside) and fails if any reference a wxWidgets type/function, or if the
# library links a wx shared object directly.
#
# Usage:
#   engine/scripts/verify_no_wx.sh [path-to-liborca-engine.so]
#
# If no path is given, the script searches common build locations.
# Exits 0 if clean, non-zero (and prints offenders) otherwise.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

find_lib() {
    if [[ $# -ge 1 && -n "${1:-}" ]]; then
        printf '%s\n' "$1"
        return
    fi
    local candidate
    candidate=$(find "$REPO_ROOT/build" "$REPO_ROOT"/build-* \
        -name 'liborca-engine.so' -type f 2>/dev/null | head -1 || true)
    printf '%s\n' "$candidate"
}

LIB="$(find_lib "${1:-}")"

if [[ -z "$LIB" || ! -f "$LIB" ]]; then
    echo "verify_no_wx: could not find liborca-engine.so (pass the path as an argument)" >&2
    exit 2
fi

echo "verify_no_wx: inspecting $LIB"

# Pattern for wxWidgets symbols: mangled (_ZN2wx.., _ZNK2wx.., vtables/typeinfo
# _ZTVN2wx../_ZTIN2wx..) and common demangled forms. Anchored to avoid matching
# coincidental substrings like the libc function iswxdigit_l.
WX_RE='(^_ZN2wx|^_ZNK2wx|^_ZTVN2wx|^_ZTIN2wx|^_ZTSN2wx|^wxWindow|^wxApp|^wxEvtHandler|^wxString|^wxControl|^wxBoxSizer|^wxPanel|^wxFrame|^wxDialog)'

# 1) Undefined symbols that reference wxWidgets — i.e. the engine *needs* wx from
#    outside. This is the primary boundary violation.
wx_undef="$(nm -D --undefined-only "$LIB" 2>/dev/null \
    | awk '{print $NF}' \
    | grep -E "$WX_RE" \
    || true)"

# 2) Defined symbols belonging to wxWidgets — i.e. the engine has *embedded* wx
#    (e.g. accidentally static-linked a wx .a). Also a violation.
wx_defined="$(nm -D --defined-only "$LIB" 2>/dev/null \
    | awk '{print $NF}' \
    | grep -E "$WX_RE" \
    || true)"

# 3) Direct wx shared-library dependencies.
wx_libs="$(ldd "$LIB" 2>/dev/null | grep -iE 'libwx|wxgtk|wxbase' || true)"

status=0

if [[ -n "$wx_undef" ]]; then
    echo "verify_no_wx: FAIL — engine references (needs) wxWidgets symbols:" >&2
    # shellcheck disable=SC2086 # word-splitting is intentional: each symbol gets its own '%s\n'.
    printf '  %s\n' $wx_undef | head -20 >&2
    status=1
fi

if [[ -n "$wx_defined" ]]; then
    echo "verify_no_wx: FAIL — engine embeds wxWidgets symbols (static-linked wx?):" >&2
    # shellcheck disable=SC2086 # word-splitting is intentional: each symbol gets its own '%s\n'.
    printf '  %s\n' $wx_defined | head -20 >&2
    status=1
fi

if [[ -n "$wx_libs" ]]; then
    echo "verify_no_wx: FAIL — engine links wxWidgets shared objects:" >&2
    printf '  %s\n' "$wx_libs" >&2
    status=1
fi

if [[ "$status" -eq 0 ]]; then
    echo "verify_no_wx: OK — no wxWidgets coupling in the engine."
fi

exit "$status"
