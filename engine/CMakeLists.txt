# Engine library subtree.
#
# orca-engine is the shared library exposing the public API in engine/include/orca/.
# Build-in-place strategy (Phase 0.3): the underlying target is libslic3r, defined
# in src/libslic3r/CMakeLists.txt — its sources live in src/libslic3r/ and its
# output-name is set to "orca-engine" so the shipped artifact is liborca-engine.so.
# We expose it here as orca-engine via ALIAS so engine consumers and future
# rewriter passes can use the canonical name without caring where the sources live.
#
# The alias is created in the root CMakeLists.txt after add_subdirectory(src)
# wires libslic3r up, and before this subdirectory is added — so targets here
# can link `orca-engine` by name.

# Phase 0.4a — engine-side service implementations.
#
# These sources are compiled into libslic3r (a.k.a. liborca-engine.so) via
# target_sources so the alias above can be linked by name without the GUI or
# CLI needing to know where the orca::* services live.
#
# engine/include/ becomes a PUBLIC include directory on the engine target so
# consumers automatically get the orca/*.hpp surface when they link orca-engine.
target_sources(libslic3r PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Globals.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Presets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Slicer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/c_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PlaceholderProvider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PrinterAgentAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/WasmHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/WasmImports.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/WasmPlugin.cpp
)
target_include_directories(libslic3r PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Phase 3.2.2 — Wasmtime runtime. The pre-built C-API tarball lands in
# ${DESTDIR}/include + ${DESTDIR}/lib (see deps/Wasmtime/Wasmtime.cmake).
# Required; if the dep tree is stale, re-run `cmake --build deps/build
# --target dep_Wasmtime`.
find_path(WASMTIME_INCLUDE_DIR wasmtime.h)
find_library(WASMTIME_LIBRARY NAMES wasmtime)
if (NOT WASMTIME_INCLUDE_DIR OR NOT WASMTIME_LIBRARY)
    message(FATAL_ERROR
        "Wasmtime C API not found. Build it via:\n"
        "  cmake --build deps/build --target dep_Wasmtime\n"
        "and re-run cmake. Looked for include=${WASMTIME_INCLUDE_DIR} "
        "lib=${WASMTIME_LIBRARY}.")
endif ()
target_include_directories(libslic3r SYSTEM PRIVATE ${WASMTIME_INCLUDE_DIR})
target_link_libraries(libslic3r PRIVATE ${WASMTIME_LIBRARY})

# orca-engine-cli: the standalone canary proving the engine links and runs
# without wxWidgets. Only built when engine tooling is requested.
add_subdirectory(cli)
