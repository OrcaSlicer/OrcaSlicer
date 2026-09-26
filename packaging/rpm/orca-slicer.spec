%global appname   OrcaSlicer
%global install_dir %{_libdir}/%{appname}

Name:           orca-slicer
Version:        %{rpm_version}
Release:        1%{?dist}
Summary:        Open-source 3D slicer for FDM and SLA printers
License:        AGPLv3
URL:            https://github.com/OrcaSlicer/OrcaSlicer

# This spec packages pre-built artifacts. OrcaSlicer requires custom-built
# versions of several dependencies that are statically linked into the binary,
# so a standard source-only RPM is not practical for distribution builds.
ExclusiveArch:  x86_64 aarch64

%description
OrcaSlicer is an open-source 3D slicer forked from Bambu Studio.
It supports FDM and SLA printers with advanced slicing algorithms,
multi-material printing, tree supports, variable layer height, and more.

%prep
# Nothing to prepare; artifacts are pre-built in %%{_builddir}.

%build
# Nothing to build; artifacts are pre-built.

%install
# Application binary
install -d %{buildroot}%{install_dir}/bin
cp -a %{src_artifacts}/package/bin/. %{buildroot}%{install_dir}/bin/

# Fix build-time RPATHs: replace with $ORIGIN so co-located libs are found
# without relying on absolute build-machine paths (which rpmbuild rejects).
for f in %{buildroot}%{install_dir}/bin/*; do
    if file "$f" | grep -q ELF; then
        patchelf --set-rpath '$ORIGIN' "$f"
    fi
done

# Resources
install -d %{buildroot}%{install_dir}/resources
cp -a %{src_artifacts}/package/resources/. %{buildroot}%{install_dir}/resources/

# Launcher wrapper (uses fixed install path unlike the AppImage wrapper)
install -d %{buildroot}%{_bindir}
cat > %{buildroot}%{_bindir}/orca-slicer << 'EOF'
#!/bin/bash
ORCA_DIR=%{install_dir}
export LD_LIBRARY_PATH="$ORCA_DIR/bin:$LD_LIBRARY_PATH"

# OrcaSlicer segfault workaround: ensure locale info is set
export LC_ALL=C

if [ "$XDG_SESSION_TYPE" = "wayland" ] && [ "$ZINK_DISABLE_OVERRIDE" != "1" ]; then
    if command -v glxinfo >/dev/null 2>&1; then
        RENDERER=$(glxinfo | grep "OpenGL renderer string:" | sed 's/.*: //')
        if echo "$RENDERER" | grep -qi "NVIDIA"; then
            if command -v nvidia-smi >/dev/null 2>&1; then
                DRIVER_VERSION=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -n1)
                DRIVER_MAJOR=$(echo "$DRIVER_VERSION" | cut -d. -f1)
                [ "$DRIVER_MAJOR" -gt 555 ] && ZINK_FORCE_OVERRIDE=1
            fi
            if [ "$ZINK_FORCE_OVERRIDE" = "1" ]; then
                export __GLX_VENDOR_LIBRARY_NAME=mesa
                export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
                export MESA_LOADER_DRIVER_OVERRIDE=zink
                export GALLIUM_DRIVER=zink
                export WEBKIT_DISABLE_DMABUF_RENDERER=1
            fi
        fi
    fi
fi

exec "$ORCA_DIR/bin/orca-slicer" "$@"
EOF
chmod 0755 %{buildroot}%{_bindir}/orca-slicer

# Desktop entry
install -Dm644 %{src_artifacts}/src/dev-utils/platform/unix/com.orcaslicer.OrcaSlicer.desktop \
    %{buildroot}%{_datadir}/applications/com.orcaslicer.OrcaSlicer.desktop

# Icons (multiple sizes)
install -Dm644 %{src_artifacts}/resources/images/OrcaSlicer_192px.png \
    %{buildroot}%{_datadir}/icons/hicolor/192x192/apps/OrcaSlicer.png
install -Dm644 %{src_artifacts}/resources/images/OrcaSlicer_128px.png \
    %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/OrcaSlicer.png
install -Dm644 %{src_artifacts}/resources/images/OrcaSlicer_64.png \
    %{buildroot}%{_datadir}/icons/hicolor/64x64/apps/OrcaSlicer.png
install -Dm644 %{src_artifacts}/resources/images/OrcaSlicer_32px.png \
    %{buildroot}%{_datadir}/icons/hicolor/32x32/apps/OrcaSlicer.png

%post
/bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
/usr/bin/update-desktop-database &>/dev/null || :

%postun
if [ $1 -eq 0 ]; then
    /bin/touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
    /usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
fi
/usr/bin/update-desktop-database &>/dev/null || :

%posttrans
/usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :

%files
%{_bindir}/orca-slicer
%{install_dir}/
%{_datadir}/applications/com.orcaslicer.OrcaSlicer.desktop
%{_datadir}/icons/hicolor/192x192/apps/OrcaSlicer.png
%{_datadir}/icons/hicolor/128x128/apps/OrcaSlicer.png
%{_datadir}/icons/hicolor/64x64/apps/OrcaSlicer.png
%{_datadir}/icons/hicolor/32x32/apps/OrcaSlicer.png

%changelog
* Mon May 25 2026 Simonas Kareiva <skareiva@redhat.com> - 2.4.0-1
- Update .desktop file to use reverse-DNS name com.orcaslicer.OrcaSlicer.desktop
* Thu Mar 19 2026 Simonas Kareiva <skareiva@redhat.com> - 2.3.2-1
- Initial RPM packaging for Fedora
