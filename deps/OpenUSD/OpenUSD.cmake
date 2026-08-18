orcaslicer_add_cmake_project(OpenUSD
    URL https://github.com/PixarAnimationStudios/OpenUSD/archive/refs/tags/v26.08.zip
    URL_HASH SHA256=fbf509e8292055a0be7801f56218ed5d15a3c9022e24636fcc9ca8587345c760
    DEPENDS dep_TBB
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        # Only the scene-description core is wanted: open a stage, walk prims,
        # read UsdGeomMesh. Everything below is renderer or authoring surface.
        -DPXR_ENABLE_PYTHON_SUPPORT=OFF
        -DPXR_BUILD_IMAGING=OFF
        -DPXR_BUILD_USD_IMAGING=OFF
        -DPXR_BUILD_USDVIEW=OFF
        -DPXR_BUILD_TESTS=OFF
        -DPXR_BUILD_EXAMPLES=OFF
        -DPXR_BUILD_TUTORIALS=OFF
        -DPXR_BUILD_USD_TOOLS=OFF
        -DPXR_BUILD_DOCUMENTATION=OFF
        -DPXR_ENABLE_GL_SUPPORT=OFF
        -DPXR_ENABLE_MATERIALX_SUPPORT=OFF
        -DPXR_ENABLE_OSL_SUPPORT=OFF
        -DPXR_ENABLE_HDF5_SUPPORT=OFF
        # Orca links its deps statically, and a monolithic archive keeps the
        # link line to one library instead of USD's ~40.
        -DBUILD_SHARED_LIBS=OFF
        -DPXR_BUILD_MONOLITHIC=ON
)
