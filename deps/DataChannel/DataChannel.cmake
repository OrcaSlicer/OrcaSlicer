# libdatachannel is the native ICE/DTLS/SCTP implementation used by the
# GUI WebRTC camera controller. Keep the source revision fixed: the signaling
# protocol is evolving independently of this transport dependency.
#
# It vendors plog, usrsctp and libjuice as git submodules, which a plain
# GitHub tag tarball does not include. So the tag tarball and one archive per
# submodule, at the commit the tag pins, are downloaded into the dependency
# download directory (DL_CACHE) like any other archive, checked against their
# SHA256 and reused by later builds. The patch step unpacks the submodule
# archives into the source tree. json (examples only) and libsrtp (media) are
# not built here, so they are not fetched.
#
# When bumping the tag, take each submodule's commit from
# `git ls-tree <tag> deps/` and update its URL and hash below.
#
# The flatpak sandbox has no network access during the build, so there the
# manifest itself clones the repo (submodules and all) into the dependency
# download directory before the sandbox closes. ExternalProject_Add is pointed
# at that existing checkout instead of being given its own download method.
if (FLATPAK)
    set(_datachannel_source
        SOURCE_DIR ${DEP_DOWNLOAD_DIR}/DataChannel
    )
else()
    set(_datachannel_submodules
        "deps/plog,https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz,92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072"
        "deps/usrsctp,https://github.com/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz,e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c"
        "deps/libjuice,https://github.com/paullouisageneau/libjuice/archive/3c40a3545b6b1b62c7adee7f8f2bd58aa290afd6.tar.gz,a6b1d55338ea12adc0177eaafd9521ac0101b6a8716c71024a668f4896fd6b7c"
    )
    # Passed through a file: the entries hold characters a command line would mangle.
    set(_datachannel_list ${CMAKE_CURRENT_BINARY_DIR}/DataChannel_submodules.cmake)
    file(WRITE ${_datachannel_list} "set(_submodules \"${_datachannel_submodules}\")\n")
    set(_datachannel_source
        URL https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz
        URL_HASH SHA256=454537c3cd526bed935d847bb2dff4046f266eef84d43b2a5f2f2f293c0026f4
        DOWNLOAD_NAME libdatachannel-v0.24.5.tar.gz
        PATCH_COMMAND ${CMAKE_COMMAND}
            -DLIST=${_datachannel_list}
            -DDL=${DEP_DOWNLOAD_DIR}/DataChannel
            -DSRC=<SOURCE_DIR>
            -P ${CMAKE_CURRENT_LIST_DIR}/fetch_submodules.cmake
    )
endif()

orcaslicer_add_cmake_project(DataChannel
    DEPENDS ${OPENSSL_PKG}
    CMAKE_ARGS
        -DNO_EXAMPLES=ON
        -DNO_TESTS=ON
        -DNO_WEBSOCKET=ON
        -DNO_MEDIA=ON
        -DUSE_NICE=OFF
        -DUSE_SYSTEM_JUICE=OFF
        -DUSE_SYSTEM_USRSCTP=OFF
        -DOPENSSL_ROOT_DIR:PATH=${DESTDIR}
        -DOPENSSL_USE_STATIC_LIBS=ON
    ${_datachannel_source}
)
