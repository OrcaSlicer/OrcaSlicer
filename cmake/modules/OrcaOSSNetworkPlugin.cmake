# OrcaOSSNetworkPlugin.cmake
#
# The open-source network plugin (open-bamboo-networking, the bambu_network_oss
# submodule) is BUILT IN CI per platform and STAGED into resources/plugins/
# BEFORE the OrcaSlicer package step (see .github/workflows/build_all.yml +
# build_orca.yml). OrcaSlicer's top-level install(DIRECTORY resources/ ...)
# then bundles whatever is in resources/plugins/ into the AppImage (Linux) and
# the NSIS installer (Windows).
#
# This module therefore does NOT build the plugin. It only:
#   1. Defines ORCA_OSS_NETWORK_PLUGIN_VERSION (the ABI/version string the
#      plugin reports and the suffix in its on-disk filename). This MUST stay
#      in lockstep with:
#         - the --with-version / -WithVersion passed to the plugin's configure
#           in CI, and
#         - the ver string in GUI_App::ensure_oss_network_plugin().
#   2. Verifies that a matching pre-staged plugin exists in resources/plugins/.
#      It WARNS but does NOT fail if absent, so a local developer build (which
#      may not have staged the plugin) still configures and builds OrcaSlicer.
#
# The ORCA_OSS_NETWORK_PLUGIN compile definition (set in the top-level
# CMakeLists.txt) gates ensure_oss_network_plugin() and disables the
# proprietary-plugin download in PresetUpdater.cpp.
#
# vcpkg note: the plugin's Windows build needs vcpkg; OrcaSlicer's Windows
# build does NOT use vcpkg (it uses the deps/ ExternalProject tree). The two
# never meet because the plugin is built in its own isolated CI job and only
# the finished DLL is copied here. Do not ExternalProject-build the plugin
# inside this tree on Windows.

# Keep this in lockstep with:
#   - GUI_App::ensure_oss_network_plugin()  (src/slic3r/GUI/GUI_App.cpp)
#   - the CI plugin-build jobs (--with-version / -WithVersion)
set(ORCA_OSS_NETWORK_PLUGIN_VERSION "02.07.01.51" CACHE STRING
    "Version the OSS network plugin reports + filename suffix (modern ABI 02.07.01, the Bambu Studio 2.7 networking version; must match GUI_App::ensure_oss_network_plugin() and CI build).")

# Propagate the version into the C++ build so GUI_App.cpp uses the same string.
add_compile_definitions(ORCA_OSS_NETWORK_PLUGIN_VERSION="${ORCA_OSS_NETWORK_PLUGIN_VERSION}")

# Expected staged main-plugin filename per platform. The plugin's orca_slicer
# install renames the shared object to include the version (Orca dlopens the
# versioned name); BambuSource / live555 keep their fixed names.
if (WIN32)
    set(_oss_staged_main "bambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.dll")
elseif (APPLE)
    set(_oss_staged_main "libbambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.dylib")
else()
    set(_oss_staged_main "libbambu_networking_${ORCA_OSS_NETWORK_PLUGIN_VERSION}.so")
endif()

set(_oss_plugins_dir "${CMAKE_SOURCE_DIR}/resources/plugins")
set(_oss_staged_path "${_oss_plugins_dir}/${_oss_staged_main}")

if (EXISTS "${_oss_staged_path}")
    message(STATUS "ORCA_OSS_NETWORK_PLUGIN: found pre-staged plugin v${ORCA_OSS_NETWORK_PLUGIN_VERSION} at ${_oss_staged_path} (will be bundled via resources/).")
else()
    message(WARNING "ORCA_OSS_NETWORK_PLUGIN: no pre-staged plugin at ${_oss_staged_path}. The OrcaSlicer package will NOT bundle the OSS network plugin. CI stages it before packaging; for a local build, build open-bamboo-networking (--client-type=orca_slicer --with-version=${ORCA_OSS_NETWORK_PLUGIN_VERSION}) and copy plugins/* into resources/plugins/.")
endif()
