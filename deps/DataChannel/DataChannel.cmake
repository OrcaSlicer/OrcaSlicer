# libdatachannel is the native ICE/DTLS/SCTP implementation used by the
# GUI WebRTC camera controller. Keep the source revision fixed: the signaling
# protocol is evolving independently of this transport dependency.
#
# It vendors plog, usrsctp and libjuice as git submodules, which a plain
# GitHub tag tarball does not include. The flatpak sandbox has no network
# access during the build, so there the manifest itself clones the repo
# (submodules and all) straight into this ExternalProject's default source
# dir before the sandbox closes; with no download method given here and that
# dir already non-empty, ExternalProject_Add silently uses it as-is instead
# of trying its own (network) git step.
if (FLATPAK)
    set(_datachannel_source "")
else()
    set(_datachannel_source
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.24.5
        GIT_SHALLOW ON
        GIT_SUBMODULES_RECURSE ON
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
