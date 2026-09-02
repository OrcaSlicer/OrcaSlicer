#!/usr/bin/env bash
# Shared MSVC environment import for Git Bash on Windows.
#
# Source this file (not execute) from dev scripts that need the MSVC toolchain
# (INCLUDE/LIB are unset in a plain Git Bash). Exposes:
#
#   import_msvc_env PREFIX   -> imports the MSVC x64 environment via vcvarsall,
#                               excluding PATH so bash's own tools keep working.
#                               Returns 0 if imported, nonzero if VS was not
#                               found (caller decides whether to warn or abort).
#
# On non-Windows, import_msvc_env is a no-op that returns 0.

import_msvc_env() {
    local prefix="${1:-}"
    local vs_install=""
    local vswhere_u vsw_pf vcdir line name

    [[ "$OSTYPE" == msys* || "$OSTYPE" == cygwin* || "$OSTYPE" == mingw* ]] || return 0

    # vswhere.exe lives at the fixed %ProgramFiles(x86)%\Microsoft Visual Studio\Installer
    # location. %ProgramFiles(x86)% is not addressable from bash, so resolve it via cmd.
    vsw_pf=""
    for pf in "$(cmd //c "echo %ProgramFiles(x86)%" 2>/dev/null)" "/c/Program Files (x86)"; do
        pf="${pf%$'\r'}"
        [[ -n "$pf" ]] || continue
        vsw_pf="$(cygpath -u "$pf" 2>/dev/null)"
        [[ -n "$vsw_pf" ]] && break
    done

    vswhere_u="$vsw_pf/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -x "$vswhere_u" ]]; then
        vs_install="$(cygpath -u "$("$vswhere_u" -latest -products '*' \
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
            -property installationPath 2>/dev/null)")"
    fi

    if [[ -z "$vs_install" ]]; then
        for p in "/c/Program Files/Microsoft Visual Studio/2022/Community" \
                 "/c/Program Files (x86)/Microsoft Visual Studio/2022/Community"; do
            [[ -e "$p/VC/Auxiliary/Build/vcvarsall.bat" ]] && { vs_install="$p"; break; }
        done
    fi

    if [[ -z "$vs_install" ]]; then
        echo "${prefix}WARNING: Visual Studio with the C++ workload was not found; MSVC headers may be missing" >&2
        unset vs_install vswhere_u vsw_pf
        return 1
    fi

    vcdir="$vs_install/VC/Auxiliary/Build"
    echo "${prefix}importing MSVC environment from ${vs_install}"
    pushd "$vcdir" >/dev/null
    while IFS= read -r line; do
        line="${line%$'\r'}"
        name="${line%%=*}"
        [[ "$name" == [Pp][Aa][Tt][Hh] ]] && continue
        if [[ "$line" == *=* && -n "${line#*=}" ]]; then
            export "$line" 2>/dev/null || true
        fi
    done < <(cmd //c "vcvarsall.bat x64 >nul 2>&1 && set")
    popd >/dev/null
    unset vs_install vswhere_u vsw_pf vcdir
    return 0
}
