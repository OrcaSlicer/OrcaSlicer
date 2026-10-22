set(patch_command git init && ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-Respect-BUILD_SHARED_LIBS.patch)

orcaslicer_add_cmake_project(ZLIB
  # GIT_REPOSITORY https://github.com/madler/zlib.git
  # GIT_TAG v1.3.1
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.zip

  URL_HASH SHA256=50b24b47bf19e1f35d2a21ff36d2a366638cdf958219a66f30ce0861201760e6
  CMAKE_ARGS
    -DSKIP_INSTALL_FILES=ON         # Prevent installation of man pages et al.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

