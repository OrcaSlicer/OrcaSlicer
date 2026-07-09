if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (JPEG_VERSION STREQUAL "6")
        message("Using Jpeg Lib 62")
        set(jpeg_flag "")
    elseif (JPEG_VERSION STREQUAL "7")
        message("Using Jpeg Lib 70")
        set(jpeg_flag "-DWITH_JPEG7=ON")
    else ()
        message("Using Jpeg Lib 80")
        set(jpeg_flag "-DWITH_JPEG8=ON")
    endif ()
endif()

orcaslicer_add_cmake_project(JPEG
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.2.0.zip
    URL_HASH SHA256=1b4c2cb6cba1ca5e53b9e85eeeab02154cab4b70894c96e97a9c747a26d19d63
    DEPENDS ${ZLIB_PKG}
    CMAKE_ARGS
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        ${jpeg_flag}
)
