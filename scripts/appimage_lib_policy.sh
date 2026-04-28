#!/usr/bin/env bash

# Libraries that are safer to resolve from the host than bundle into the AppImage.
# Keep this list focused on the glibc/runtime loader and host-specific graphics/audio stacks.
appimage_is_host_library() {
    local lib_name
    lib_name="$(basename "$1")"

    case "$lib_name" in
        linux-vdso.so.*|linux-gate.so.*|ld-linux*.so*|ld64.so*|ld-musl-*.so*|libc.so*|libpthread.so*|libm.so*|libdl.so*|librt.so*|libresolv.so*|libutil.so*|libanl.so*|libnsl.so*|libBrokenLocale.so*|libcrypt.so*|libnss_*.so*|\
        libGL.so*|libOpenGL.so*|libGLX*.so*|libGLU.so*|libEGL.so*|libGLES*.so*|libGLdispatch.so*|libdrm.so*|libdrm_*.so*|libgbm.so*|libwayland-*.so*|libxcb*.so*|libX11.so*|libX11-xcb.so*|libXau.so*|libXdmcp.so*|libXext.so*|libXdamage.so*|libXfixes.so*|libXcomposite.so*|libXrender.so*|libXrandr.so*|libXcursor.so*|libXi.so*|libXinerama.so*|libxshmfence.so*|libxkbcommon.so*|libxkbcommon-x11.so*|libSM.so*|libICE.so*|libudev.so*|libasound.so*|libpulse.so*|libpulsecommon*.so*|libjack.so*|libpipewire-*.so*|libvulkan.so*|libva.so*|libva-*.so*|\
        libgtk-*.so*|libgdk-*.so*|libpango*.so*|libatk-bridge-*.so*|libatk*.so*|libatspi.so*|libcairo*.so*|libgdk_pixbuf-*.so*|libgio-2.0.so*|libgmodule-2.0.so*|libgobject-2.0.so*|libglib-2.0.so*|\
        libgstreamer-1.0.so*|libgst*.so*|libsoup-*.so*|libwebkit2gtk-*.so*|libjavascriptcoregtk-*.so*|libsecret-1.so*|libmanette-0.2.so*|libenchant-2.so*|libhyphen.so*|libtasn1.so*|\
        libfontconfig.so*|libfreetype.so*|libharfbuzz*.so*|libfribidi.so*|libgraphite2.so*|libthai.so*|libdatrie.so*|libepoxy.so*|libpixman-1.so*|\
        libstdc++.so*|libgcc_s.so*|libatomic.so*|libdbus-1.so*|libuuid.so*|libffi.so*|libselinux.so*|libmount.so*|libblkid.so*|libpcre2-*.so*|libsystemd.so*|libcap.so*|libseccomp.so*|\
        liborc-0.4.so*|libgudev-1.0.so*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

appimage_is_elf_file() {
    file -b "$1" 2>/dev/null | grep -q '^ELF '
}

appimage_list_direct_dependencies() {
    local target="$1"
    local line dep dep_name
    declare -A needed=()

    # Use objdump to identify the direct DT_NEEDED entries first. ldd reports the
    # full runtime tree, which can accidentally pull transitive dependencies of the
    # host GTK/pixbuf stack into the AppImage bundle.
    while IFS= read -r dep_name; do
        if [[ -n "$dep_name" ]]; then
            needed["$dep_name"]=1
        fi
    done < <(objdump -p "$target" 2>/dev/null | awk '$1 == "NEEDED" { print $2 }')

    if (( ${#needed[@]} == 0 )); then
        return 0
    fi

    while IFS= read -r line; do
        dep_name=""
        if [[ "$line" == *"=>"* ]]; then
            dep_name="$(printf '%s\n' "$line" | awk '{print $1}')"
        elif [[ "$line" =~ ^[[:space:]]/ ]]; then
            dep_name="$(basename "$(printf '%s\n' "$line" | awk '{print $1}')")"
        fi

        if [[ -z "$dep_name" || -z "${needed[$dep_name]+x}" ]]; then
            continue
        fi

        if [[ "$line" == *"=> not found"* ]]; then
            echo "MISSING:$dep_name"
            continue
        fi

        dep=""
        if [[ "$line" == *"=>"* ]]; then
            dep="$(printf '%s\n' "$line" | sed -n 's/.*=> \(\/[^ ]*\).*/\1/p')"
        elif [[ "$line" =~ ^[[:space:]]/ ]]; then
            dep="$(printf '%s\n' "$line" | awk '{print $1}')"
        fi

        if [[ -n "$dep" ]]; then
            echo "$dep"
        fi
    done < <(ldd "$target" 2>/dev/null || true)
}
