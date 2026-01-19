set(patch_command git init && ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-Respect-BUILD_SHARED_LIBS.patch)

orcaslicer_add_cmake_project(ZLIB
  # GIT_REPOSITORY https://github.com/madler/zlib.git
  # GIT_TAG v1.2.11
  #URL https://github.com/madler/zlib/archive/refs/tags/v1.2.11.zip
  URL https://github.com/madler/zlib/archive/refs/tags/v1.2.13.zip
  #URL_HASH SHA256=f5cc4ab910db99b2bdbba39ebbdc225ffc2aa04b4057bc2817f1b94b6978cfc3
  URL_HASH SHA256=c2856951bbf30e30861ace3765595d86ba13f2cf01279d901f6c62258c57f4ff
  PATCH_COMMAND ${patch_command}
  CMAKE_ARGS
    -DSKIP_INSTALL_FILES=ON         # Prevent installation of man pages et al.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

