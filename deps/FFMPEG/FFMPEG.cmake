set(_conf_cmd ./configure)

if (MSVC)
    set(_source_dir "${CMAKE_BINARY_DIR}/dep_FFMPEG-prefix/src/dep_FFMPEG")

    set(PREBUILD_URL_arm64 "https://github.com/Noisyfox/FFmpeg-Builds-Orca/releases/download/autobuild-2026-07-17-14-28/ffmpeg-n7.0.3-31-g9b6ffd74b5-winarm64-orca-shared-7.0.zip")
    set(PREBUILD_HASH_arm64 "12f4140279f2f8469885e1b5b2e8be9d788882914c21523cacd56989f3548054")
    set(PREBUILD_URL_x64 "https://github.com/Noisyfox/FFmpeg-Builds-Orca/releases/download/autobuild-2026-07-17-14-28/ffmpeg-n7.0.3-31-g9b6ffd74b5-win64-orca-shared-7.0.zip")
    set(PREBUILD_HASH_x64 "e65916020ddb9ef84b2666dfbcbfc9b1d67f69d15b4a66db53754637bf2d498c")

    ExternalProject_Add(dep_FFMPEG
        URL ${PREBUILD_URL_${DEPS_ARCH}}
        URL_HASH SHA256=${PREBUILD_HASH_${DEPS_ARCH}}
        DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/FFMPEG
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_directory  "${_source_dir}/bin" "${DESTDIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_directory  "${_source_dir}/lib" "${DESTDIR}/lib"
            COMMAND ${CMAKE_COMMAND} -E copy_directory  "${_source_dir}/include" "${DESTDIR}/include"
    )

else ()
    if (APPLE)
        set(_minos_cmd
            "--extra-cflags=-mmacosx-version-min=${DEP_OSX_TARGET}"
            "--extra-ldflags=-mmacosx-version-min=${DEP_OSX_TARGET}"
            )
        # Static FFmpeg: nothing to bundle into the .app, no rpath handling.
        # Disable the VideoToolbox/AudioToolbox HW-accel paths: the player decodes
        # in software (swscale), and the auto-detected HW objects would drag in
        # system frameworks that the static libs would then depend on.
        set(_link_cmd --enable-static --disable-shared --disable-videotoolbox --disable-audiotoolbox)
        if (IS_CROSS_COMPILE)
            set(_cross_cmd --enable-cross-compile)
            set(_pic_cmd --enable-pic)
            if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
                set(_arch_cmd --arch=arm64)
                set(_cc_cmd "--cc=clang -arch arm64")
            else()
                set(_arch_cmd --arch=x86_64)
                set(_cc_cmd "--cc=clang -arch x86_64")
            endif()
        endif()
    else ()
        set(_link_cmd --enable-shared)
    endif ()

    set(_build_j -j)
    if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
        set(_build_j "-j$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()

    ExternalProject_Add(dep_FFMPEG
        URL https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.0.3.tar.gz
        URL_HASH SHA256=DEEDCABE339165214A3637DF4C86A507AEF0D793CF8774FF68735F4737E8DDBC
        DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/FFMPEG
        CONFIGURE_COMMAND ${_conf_cmd}
            ${_cross_cmd}
            ${_pic_cmd}
            ${_arch_cmd}
            ${_cc_cmd}
            "--prefix=${DESTDIR}"
            ${_link_cmd}
            ${_minos_cmd}
            --disable-doc
            --enable-small
            --disable-outdevs
            --disable-filters
            --enable-filter=*null*,afade,*fifo,*format,*resample,aeval,allrgb,allyuv,atempo,pan,*bars,color,*key,crop,draw*,eq*,framerate,*_qsv,*_vaapi,*v4l2*,hw*,scale,volume,test*
            --disable-protocols
            --enable-protocol=file,fd,pipe,rtp,udp
            --disable-muxers
            --enable-muxer=rtp
            --disable-encoders
            --disable-decoders
            --enable-decoder=*aac*,h264*,mp3*,mjpeg,rv*
            --disable-demuxers
            --enable-demuxer=h264,mp3,mov
            --disable-zlib
            --disable-avdevice
        BUILD_IN_SOURCE ON
        BUILD_COMMAND make ${_build_j}
        INSTALL_COMMAND make install
    )

endif()
