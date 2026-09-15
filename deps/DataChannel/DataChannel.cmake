# libdatachannel is the native ICE/DTLS/SCTP/SRTP implementation used by the
# GUI WebRTC camera controller. Keep the source revision fixed: the signaling
# protocol is evolving independently of this transport dependency.
orcaslicer_add_cmake_project(DataChannel
    DEPENDS ${OPENSSL_PKG}
    CMAKE_ARGS
        -DNO_EXAMPLES=ON
        -DNO_TESTS=ON
        -DNO_WEBSOCKET=ON
        -DNO_MEDIA=OFF
        -DUSE_NICE=OFF
        -DUSE_SYSTEM_SRTP=OFF
        -DUSE_SYSTEM_JUICE=OFF
        -DUSE_SYSTEM_USRSCTP=OFF
        -DOPENSSL_ROOT_DIR:PATH=${DESTDIR}
        -DOPENSSL_USE_STATIC_LIBS=ON
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.22.2
    GIT_SHALLOW ON
    GIT_SUBMODULES_RECURSE ON
)