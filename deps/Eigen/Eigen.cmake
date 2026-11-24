set(_eigen_extra_flags "")
if (MSVC)
    set(_eigen_extra_flags "-DCMAKE_CXX_FLAGS:STRING=/bigobj")
endif ()

orcaslicer_add_cmake_project(Eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.zip
    URL_HASH SHA256=0dbb1f9e3aaad66f352c03227d8c983f6f0b49e0b07e71a7300f4abcc01aee12
    CMAKE_ARGS "${_eigen_extra_flags}"
        # Only the headers are consumed here. Everything below builds nothing we
        # use, and all three enable_language(Fortran): test/CMakeLists.txt:9,
        # lapack/CMakeLists.txt:6 and blas/testing/CMakeLists.txt:2. They default
        # to ON because the dependency configures as its own top-level project.
        #
        # Whether that probe is harmless depends on what CMake finds. The Visual
        # Studio generator supports no Fortran, so it finds nothing; clang-cl sits
        # next to the LLVM toolset's flang, which works. MSVC with Ninja finds
        # Strawberry Perl's MinGW gfortran instead, which the deps build already
        # requires for OpenSSL, and hands it the MSVC-style /machine:x64 that
        # MinGW's ld reads as a missing input file. The configure dies there and
        # takes the rest of the superbuild with it.
        -DEIGEN_BUILD_TESTING=OFF
        -DEIGEN_BUILD_BLAS=OFF
        -DEIGEN_BUILD_LAPACK=OFF
    DEPENDS dep_Boost dep_GMP dep_MPFR
)
