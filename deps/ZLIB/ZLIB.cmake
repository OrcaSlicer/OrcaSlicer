set(patch_command git init && ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-Respect-BUILD_SHARED_LIBS.patch)

orcaslicer_add_cmake_project(ZLIB
  # GIT_REPOSITORY https://github.com/madler/zlib.git
  # GIT_TAG v1.3.2
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.zip
  URL_HASH SHA256=31fd9fee98812abcf147d0e103bc4d2f983c35a8d7a807a328a299f3a74e0050
  PATCH_COMMAND ${patch_command}
  CMAKE_ARGS
    -DSKIP_INSTALL_FILES=ON         # Prevent installation of man pages et al.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

