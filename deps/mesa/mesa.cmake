# Prebuilt Mesa3D binary from https://download.qt.io/development_releases/prebuilt/llvmpipe/windows/
ExternalProject_Add(dep_mesa
    URL "${CMAKE_CURRENT_LIST_DIR}/opengl32sw-64-mesa_11_2_2-signed_sha256.7z"
    URL_HASH SHA256=c61f801c1760aa24b02c7a8354323cddf368b86f8f5c34b50b3b224f7d728afd
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy "<SOURCE_DIR>/opengl32sw.dll" ${DESTDIR}/bin/mesa/opengl32.dll # Copy the dll to dist
)
