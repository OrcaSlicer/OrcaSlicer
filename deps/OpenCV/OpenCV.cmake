# Intel IPP / IPP-ICV is x86/x64 only — there is no ARM64 build, so enabling it
# leaves ~200 unresolved ippicv* externals at link time on Windows ARM64.
if (MSVC AND NOT "${DEPS_ARCH}" STREQUAL "arm64")
    set(_use_IPP "-DWITH_IPP=ON")
    if (DEP_DEBUG)
        set(_options "FORWARD_CONFIG")
    endif ()
else ()
    set(_use_IPP "-DWITH_IPP=OFF")
    set(_options "")
endif ()

# carotene is OpenCV's ARM NEON HAL. It uses M_PI without _USE_MATH_DEFINES
# and does not compile with clang-cl.
set(_disable_carotene "")
if ("${DEPS_ARCH}" STREQUAL "arm64" AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(_disable_carotene "-DWITH_CAROTENE=OFF")
endif ()

# clang-cl parses template bodies at the end of each file. In softfloat.cpp that
# is after OpenCV's own float32_t, which then hides the NEON one in arm_neon.h.
set(_clang_cl_flags "")
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(_clang_cl_flags "-DOPENCV_EXTRA_CXX_FLAGS=-fno-delayed-template-parsing")
endif ()

orcaslicer_add_cmake_project(OpenCV
    ${_options}
    URL https://github.com/opencv/opencv/archive/refs/tags/4.14.0.tar.gz
    URL_HASH SHA256=ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e
    DEPENDS ${JPEG_PKG} ${PNG_PKG}
    CMAKE_ARGS
    -DBUILD_SHARED_LIBS=0
       -DBUILD_PERE_TESTS=OFF
       -DBUILD_TESTS=OFF
       -DBUILD_opencv_python_tests=OFF
       -DBUILD_EXAMPLES=OFF
       -DBUILD_JASPER=OFF
       -DBUILD_JAVA=OFF
       # Share Orca's libjpeg and libpng to avoid conflicting codec symbols in the application.
       -DBUILD_JPEG=OFF
       -DBUILD_PNG=OFF
       -DBUILD_APPS_LIST=version
       -DBUILD_opencv_apps=OFF
       -DBUILD_opencv_java=OFF
       -DBUILD_OPENEXR=OFF
       -DBUILD_TBB=OFF
       -DBUILD_WEBP=OFF
       -DBUILD_ZLIB=OFF
       -DWITH_1394=OFF
       -DWITH_CUDA=OFF
       -DWITH_EIGEN=OFF
       ${_use_IPP}
       -DWITH_ITT=OFF
       -DWITH_FFMPEG=OFF
       -DWITH_GPHOTO2=OFF
       -DWITH_GSTREAMER=OFF
       -DOPENCV_GAPI_GSTREAMER=OFF
       -DWITH_GTK_2_X=OFF
       -DWITH_JASPER=OFF
       -DWITH_LAPACK=OFF
       -DWITH_MATLAB=OFF
       -DWITH_MFX=OFF
       -DWITH_DIRECTX=OFF
       -DWITH_DIRECTML=OFF
       -DWITH_OPENCL=OFF
       -DWITH_OPENCL_D3D11_NV=OFF
       -DWITH_OPENCLAMDBLAS=OFF
       -DWITH_OPENCLAMDFFT=OFF
       -DWITH_OPENEXR=OFF
       -DWITH_OPENJPEG=OFF
       -DWITH_QUIRC=OFF
       -DWITH_VTK=OFF
       # Textured GLB/OBJ imports decode embedded and external JPEGs with cv::imdecode.
       -DWITH_JPEG=ON
       -DWITH_WEBP=OFF
       -DWITH_TIFF=OFF
       -DBUILD_TIFF=OFF
       # Codecs and HALs added after 4.6 that default to ON. AVIF would link any
       # system libavif it finds, and KleidiCV is downloaded at configure time.
       -DWITH_AVIF=OFF
       -DWITH_IMGCODEC_GIF=OFF
       -DWITH_KLEIDICV=OFF
       -DENABLE_PRECOMPILED_HEADERS=OFF
       -DINSTALL_TESTS=OFF
       -DINSTALL_C_EXAMPLES=OFF
       -DINSTALL_PYTHON_EXAMPLES=OFF
       -DOPENCV_GENERATE_SETUPVARS=OFF
       -DOPENCV_INSTALL_FFMPEG_DOWNLOAD_SCRIPT=OFF
       -DBUILD_opencv_python2=OFF
       -DBUILD_opencv_python3=OFF
       -DWITH_OPENVINO=OFF
       -DWITH_INF_ENGINE=OFF
       -DWITH_NGRAPH=OFF
       -DBUILD_WITH_STATIC_CRT=OFF#set /MDd /MD
       -DBUILD_LIST=core,imgcodecs,imgproc,world
       -DBUILD_opencv_highgui=OFF
       -DWITH_ADE=OFF
       -DBUILD_opencv_world=ON
       -DWITH_PROTOBUF=OFF
       -DWITH_WIN32UI=OFF
       -DHAVE_WIN32UI=FALSE
       ${_disable_carotene}
       ${_clang_cl_flags}
)

