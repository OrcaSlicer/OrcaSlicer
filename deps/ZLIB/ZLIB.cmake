set(patch_command git init && ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-Respect-BUILD_SHARED_LIBS.patch)

orcaslicer_add_cmake_project(ZLIB
  # GIT_REPOSITORY https://github.com/madler/zlib.git
  # GIT_TAG v1.3.2
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.zip

  URL_HASH SHA256=e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28
  CMAKE_ARGS
    -DSKIP_INSTALL_FILES=ON         # Prevent installation of man pages et al.
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

