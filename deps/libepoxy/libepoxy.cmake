# libepoxy -- GL function loader with runtime EGL/GLX dispatch
# Uses Meson build system, so we use raw ExternalProject_Add
# instead of orcaslicer_add_cmake_project.

include(ExternalProject)

ExternalProject_Add(
    dep_libepoxy
    EXCLUDE_FROM_ALL  ON
    INSTALL_DIR       ${DESTDIR}
    DOWNLOAD_DIR      ${DEP_DOWNLOAD_DIR}/libepoxy
    URL               https://github.com/anholt/libepoxy/archive/refs/tags/1.5.10.tar.gz
    URL_HASH          SHA256=a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15
    CONFIGURE_COMMAND meson setup
        --prefix=<INSTALL_DIR>
        --default-library=static
        --buildtype=release
        -Dtests=false
        -Dx11=true
        -Degl=yes
        -Dglx=yes
        -Dc_args=-fPIC
        <SOURCE_DIR>
        <BINARY_DIR>
    BUILD_COMMAND     ninja -C <BINARY_DIR> -j${NPROC}
    INSTALL_COMMAND   ninja -C <BINARY_DIR> install
)
