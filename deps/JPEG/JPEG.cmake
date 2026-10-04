orcaslicer_add_cmake_project(JPEG
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.4.zip
    URL_HASH SHA256=0c58853494f31a65329e567569d8614f35a74c1251bdcca10bb3d01689b35035
    CMAKE_ARGS
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.10
)
