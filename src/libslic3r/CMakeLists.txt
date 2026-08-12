cmake_minimum_required(VERSION 3.13)
project(libslic3r)

include(PrecompiledHeader)

if(NOT DEFINED ORCA_CHECK_GCODE_PLACEHOLDERS)
    set(ORCA_CHECK_GCODE_PLACEHOLDERS "0")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libslic3r_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h @ONLY)

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

set(OpenVDBUtils_SOURCES "")
if (TARGET OpenVDB::openvdb)
    set(OpenVDBUtils_SOURCES OpenVDBUtils.cpp OpenVDBUtils.hpp)
endif()

option(BUILD_SHARED_LIBS "Build shared libs" OFF)
option(SLIC3R_ENABLE_VULKAN_SLICER "Build the experimental deterministic Vulkan slicer backend" OFF)
option(SLIC3R_ENABLE_CUDA_SLICER "Build the optional CUDA exact geometry backend" OFF)

set(lisbslic3r_sources
    AABBMesh.cpp
    AABBMesh.hpp
    AABBTreeIndirect.hpp
    AABBTreeLines.hpp
    Algorithm/LineSplit.cpp
    Algorithm/LineSplit.hpp
    Algorithm/RegionExpansion.cpp
    Algorithm/RegionExpansion.hpp
    AnyPtr.hpp
    AppConfig.cpp
    AppConfig.hpp
    Arachne/BeadingStrategy/BeadingStrategy.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.hpp
    Arachne/BeadingStrategy/BeadingStrategy.hpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.cpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.hpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.cpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.hpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.cpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.hpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.cpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.hpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.cpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.hpp
    Arachne/SkeletalTrapezoidation.cpp
    Arachne/SkeletalTrapezoidationEdge.hpp
    Arachne/SkeletalTrapezoidationGraph.cpp
    Arachne/SkeletalTrapezoidationGraph.hpp
    Arachne/SkeletalTrapezoidation.hpp
    Arachne/SkeletalTrapezoidationJoint.hpp
    Arachne/utils/ExtrusionJunction.hpp
    Arachne/utils/ExtrusionLine.cpp
    Arachne/utils/ExtrusionLine.hpp
    Arachne/utils/HalfEdgeGraph.hpp
    Arachne/utils/HalfEdge.hpp
    Arachne/utils/HalfEdgeNode.hpp
    Arachne/utils/PolygonsPointIndex.hpp
    Arachne/utils/PolygonsSegmentIndex.hpp
    Arachne/utils/PolylineStitcher.cpp
    Arachne/utils/PolylineStitcher.hpp
    Arachne/utils/SparseGrid.hpp
    Arachne/utils/SparseLineGrid.hpp
    Arachne/utils/SparsePointGrid.hpp
    Arachne/utils/SquareGrid.cpp
    Arachne/utils/SquareGrid.hpp
    Arachne/WallToolPaths.cpp
    Arachne/WallToolPaths.hpp
    ArcFitter.cpp
    ArcFitter.hpp
    Arrange.cpp
    Arrange.hpp
    BlacklistedLibraryCheck.cpp
    BlacklistedLibraryCheck.hpp
    BoundingBox.cpp
    BoundingBox.hpp
    BridgeDetector.cpp
    BridgeDetector.hpp
    Brim.cpp
    BrimEarsPoint.hpp
    Brim.hpp
    BuildVolume.cpp
    BuildVolume.hpp
    calib.cpp
    calib.hpp
    Circle.cpp
    Circle.hpp
    clipper.cpp
    clipper.hpp
    ClipperUtils.cpp
    ClipperUtils.hpp
    Clipper2Utils.cpp
    Clipper2Utils.hpp
    ClipperZUtils.hpp
    Color.cpp
    Color.hpp
    CommonDefs.hpp
    Config.cpp
    Config.hpp
    ContourZ.cpp
    CustomGCode.cpp
    CustomGCode.hpp
    CutUtils.cpp
    CutUtils.hpp
    EdgeGrid.cpp
    EdgeGrid.hpp
    ElephantFootCompensation.cpp
    ElephantFootCompensation.hpp
    Emboss.cpp
    Emboss.hpp
    EmbossShape.hpp
    enum_bitmask.hpp
    Execution/Execution.hpp
    Execution/ExecutionSeq.hpp
    Execution/ExecutionTBB.hpp
    ExPolygon.cpp
    ExPolygon.hpp
    ExPolygonSerialize.hpp
    ExPolygonsIndex.cpp
    ExPolygonsIndex.hpp
    Extruder.cpp
    Extruder.hpp
    ExtrusionEntityCollection.cpp
    ExtrusionEntityCollection.hpp
    ExtrusionEntity.cpp
    ExtrusionEntity.hpp
    ExtrusionSimulator.cpp
    ExtrusionSimulator.hpp
    FaceDetector.cpp
    FaceDetector.hpp
    Feature/FuzzySkin/FuzzySkin.cpp
    Feature/FuzzySkin/FuzzySkin.hpp
    Feature/Interlocking/InterlockingGenerator.cpp
    Feature/Interlocking/InterlockingGenerator.hpp
    Feature/Interlocking/VoxelUtils.cpp
    Feature/Interlocking/VoxelUtils.hpp
    FileParserError.hpp
    Fill/Fill3DHoneycomb.cpp
    Fill/Fill3DHoneycomb.hpp
    Fill/FillAdaptive.cpp
    Fill/FillAdaptive.hpp
    Fill/FillBase.cpp
    Fill/FillBase.hpp
    Fill/FillConcentric.cpp
    Fill/FillConcentric.hpp
    Fill/FillConcentricInternal.cpp
    Fill/FillConcentricInternal.hpp
    Fill/Fill.cpp
    Fill/FillCrossHatch.cpp
    Fill/FillCrossHatch.hpp
    Fill/FillGyroid.cpp
    Fill/FillGyroid.hpp
    Fill/FillHoneycomb.cpp
    Fill/FillHoneycomb.hpp
    Fill/Fill.hpp
    Fill/FillLightning.cpp
    Fill/FillLightning.hpp
    Fill/FillLine.cpp
    Fill/FillLine.hpp
    Fill/FillPlanePath.cpp
    Fill/FillPlanePath.hpp
    Fill/FillRectilinear.cpp
    Fill/FillRectilinear.hpp
    Fill/FillTpmsD.cpp
    Fill/FillTpmsD.hpp
    Fill/FillTpmsFK.cpp
    Fill/FillTpmsFK.hpp
    Fill/Lightning/DistanceField.cpp
    Fill/Lightning/DistanceField.hpp
    Fill/Lightning/Generator.cpp
    Fill/Lightning/Generator.hpp
    Fill/Lightning/Layer.cpp
    Fill/Lightning/Layer.hpp
    Fill/Lightning/TreeNode.cpp
    Fill/Lightning/TreeNode.hpp
    Flow.cpp
    Flow.hpp
    FlushVolCalc.cpp
    FlushVolCalc.hpp
    Format/3mf.cpp
    Format/3mf.hpp
    Format/AMF.cpp
    Format/AMF.hpp
    Format/DRC.cpp
    Format/DRC.hpp
    Format/bbs_3mf.cpp
    Format/bbs_3mf.hpp
    format.hpp
    Format/OBJ.cpp
    Format/OBJ.hpp
    Format/objparser.cpp
    Format/objparser.hpp
    Format/SL1.cpp
    Format/SL1.hpp
    Format/STEP.cpp
    Format/STEP.hpp
    Format/STL.cpp
    Format/STL.hpp
    Format/svg.cpp
    Format/svg.hpp
    Format/ZipperArchiveImport.cpp
    Format/ZipperArchiveImport.hpp
    GCode/AdaptivePAInterpolator.cpp
    GCode/AdaptivePAInterpolator.hpp
    GCode/AdaptivePAProcessor.cpp
    GCode/AdaptivePAProcessor.hpp
    GCode/AvoidCrossingPerimeters.cpp
    GCode/AvoidCrossingPerimeters.hpp
    GCode/ConflictChecker.cpp
    GCode/ConflictChecker.hpp
    GCode/CoolingBuffer.cpp
    GCode/CoolingBuffer.hpp
    GCode/TimelapsePosPicker.cpp
    GCode/TimelapsePosPicker.hpp
    GCode.cpp
    GCode/ExtrusionProcessor.hpp
    GCode/FanMover.cpp
    GCode/FanMover.hpp
    GCode/GCodeProcessor.cpp
    GCode/GCodeProcessor.hpp
    GCode/ElegooGCodeProcessorHelper.cpp
    GCode.hpp
    GCode/PchipInterpolatorHelper.cpp
    GCode/PchipInterpolatorHelper.hpp
    GCode/PressureEqualizer.cpp
    GCode/PressureEqualizer.hpp
    GCode/PrintExtents.cpp
    GCode/PrintExtents.hpp
    GCodeReader.cpp
    GCodeReader.hpp
    GCode/RetractWhenCrossingPerimeters.cpp
    GCode/RetractWhenCrossingPerimeters.hpp
    GCode/SeamPlacer.cpp
    GCode/SeamPlacer.hpp
    #GCodeSender.cpp
    #GCodeSender.hpp
    GCode/SmallAreaInfillFlowCompensator.cpp
    GCode/SmallAreaInfillFlowCompensator.hpp
    GCode/SpiralVase.cpp
    GCode/SpiralVase.hpp
    GCode/ThumbnailData.cpp
    GCode/ThumbnailData.hpp
    GCode/Thumbnails.cpp
    GCode/Thumbnails.hpp
    GCode/ToolOrdering.cpp
    GCode/ToolOrdering.hpp
    GCode/WipeTower2.cpp
    GCode/WipeTower2.hpp
    GCode/WipeTower.cpp
    GCode/WipeTower.hpp
    GCodeWriter.cpp
    GCodeWriter.hpp
    Geometry/ArcWelder.hpp
    Geometry/ArcWelder.cpp
    Geometry/Bicubic.hpp
    Geometry/Circle.cpp
    Geometry/Circle.hpp
    Geometry/ConvexHull.cpp
    Geometry/ConvexHull.hpp
    Geometry.cpp
    Geometry/Curves.hpp
    Geometry.hpp
    Geometry/MedialAxis.cpp
    Geometry/MedialAxis.hpp
    Geometry/Voronoi.cpp
    Geometry/Voronoi.hpp
    Geometry/VoronoiOffset.cpp
    Geometry/VoronoiOffset.hpp
    Geometry/VoronoiUtilsCgal.cpp
    Geometry/VoronoiUtilsCgal.hpp
    Geometry/VoronoiUtils.cpp
    Geometry/VoronoiUtils.hpp
    Geometry/VoronoiVisualUtils.hpp
    Int128.hpp
    KDTreeIndirect.hpp
    Layer.cpp
    Layer.hpp
    LayerRegion.cpp
    libslic3r.cpp
    libslic3r.h
    Line.cpp
    Line.hpp
    LocalesUtils.cpp
    LocalesUtils.hpp
    MarchingSquares.hpp
    Measure.cpp
    Measure.hpp
    MeasureUtils.hpp
    MeshSplitImpl.hpp
    MinAreaBoundingBox.cpp
    MinAreaBoundingBox.hpp
    MinimumSpanningTree.cpp
    MinimumSpanningTree.hpp
    miniz_extension.cpp
    miniz_extension.hpp
    ModelArrange.cpp
    ModelArrange.hpp
    Model.cpp
    Model.hpp
    MTUtils.hpp
    MultiMaterialSegmentation.cpp
    MultiMaterialSegmentation.hpp
    MultiPoint.cpp
    MultiPoint.hpp
    MutablePolygon.cpp
    MutablePolygon.hpp
    MutablePriorityQueue.hpp
    NormalUtils.cpp
    NormalUtils.hpp
    NSVGUtils.cpp
    NSVGUtils.hpp
    ObjColorUtils.cpp
    ObjColorUtils.hpp
    ObjectID.cpp
    ObjectID.hpp
    Optimize/BruteforceOptimizer.hpp
    Optimize/NLoptOptimizer.hpp
    Optimize/Optimizer.hpp
    Orient.cpp
    Orient.hpp
    ParameterUtils.cpp
    ParameterUtils.hpp
    pchheader.cpp
    pchheader.hpp
    PerimeterGenerator.cpp
    PerimeterGenerator.hpp
    PlaceholderParser.cpp
    PlaceholderParser.hpp
    Platform.cpp
    Platform.hpp
    PNGReadWrite.cpp
    PNGReadWrite.hpp
    Point.cpp
    Point.hpp
    Polygon.cpp
    Polygon.hpp
    PolygonTrimmer.cpp
    PolygonTrimmer.hpp
    Polyline.cpp
    Polyline.hpp
    PresetBundle.cpp
    PresetBundle.hpp
    Preset.cpp
    Preset.hpp
    PrincipalComponents2D.cpp
    PrincipalComponents2D.hpp
    PrintApply.cpp
    PrintBase.cpp
    PrintBase.hpp
    MaterialType.cpp
    MaterialType.hpp
    PrintConfig.cpp
    PrintConfig.hpp
    Print.cpp
    Print.hpp
    PrintObject.cpp
    PrintObjectSlice.cpp
    PrintRegion.cpp
    ProjectTask.cpp
    ProjectTask.hpp
    QuadricEdgeCollapse.cpp
    QuadricEdgeCollapse.hpp
    Semver.cpp
    Shape/TextShape.cpp
    Shape/TextShape.hpp
    ShortEdgeCollapse.cpp
    ShortEdgeCollapse.hpp
    ShortestPath.cpp
    ShortestPath.hpp
    SLA/AGGRaster.hpp
    SLA/BoostAdapter.hpp
    SLA/Clustering.cpp
    SLA/Clustering.hpp
    SLA/ConcaveHull.cpp
    SLA/ConcaveHull.hpp
    SLA/Concurrency.hpp
    SLA/Hollowing.cpp
    SLA/Hollowing.hpp
    SLA/IndexedMesh.cpp
    SLA/IndexedMesh.hpp
    SLA/JobController.hpp
    SLA/Pad.cpp
    SLA/Pad.hpp
    SLAPrint.cpp
    SLAPrint.hpp
    SLAPrintSteps.cpp
    SLAPrintSteps.hpp
    SLA/RasterBase.cpp
    SLA/RasterBase.hpp
    SLA/RasterToPolygons.cpp
    SLA/RasterToPolygons.hpp
    SLA/ReprojectPointsOnMesh.hpp
    SLA/Rotfinder.cpp
    SLA/Rotfinder.hpp
    SLA/SpatIndex.cpp
    SLA/SpatIndex.hpp
    SLA/SupportPointGenerator.cpp
    SLA/SupportPointGenerator.hpp
    SLA/SupportPoint.hpp
    SLA/SupportTreeBuilder.cpp
    SLA/SupportTreeBuilder.hpp
    SLA/SupportTreeBuildsteps.cpp
    SLA/SupportTreeBuildsteps.hpp
    SLA/SupportTree.cpp
    SLA/SupportTree.hpp
    #SLA/SupportTreeIGL.cpp
    SLA/SupportTreeMesher.cpp
    SLA/SupportTreeMesher.hpp
    SlicesToTriangleMesh.cpp
    SlicesToTriangleMesh.hpp
    SlicingAdaptive.cpp
    SlicingAdaptive.hpp
    Slicing.cpp
    Slicing.hpp
    Support/SupportCommon.cpp
    Support/SupportCommon.hpp
    Support/SupportLayer.hpp
    Support/SupportMaterial.cpp
    Support/SupportMaterial.hpp
    Support/SupportParameters.hpp
    Support/SupportSpotsGenerator.cpp
    Support/SupportSpotsGenerator.hpp
    Support/TreeModelVolumes.cpp
    Support/TreeModelVolumes.hpp
    Support/TreeSupport3D.cpp
    Support/TreeSupport3D.hpp
    Support/TreeSupportCommon.hpp
    Support/TreeSupport.cpp
    Support/TreeSupport.hpp
    SurfaceCollection.cpp
    SurfaceCollection.hpp
    Surface.cpp
    Surface.hpp
    SurfaceMesh.hpp
    SVG.cpp
    SVG.hpp
    Technologies.hpp
    Tesselate.cpp
    Tesselate.hpp
    TextConfiguration.hpp
    Thread.cpp
    Thread.hpp
    Time.cpp
    Time.hpp
    Timer.cpp
    Timer.hpp
    TriangleMesh.cpp
    TriangleMesh.hpp
    TriangleMeshDeal.cpp
    TriangleMeshDeal.hpp
    TriangleMeshSlicer.cpp
    TriangleMeshSlicer.hpp
    TriangleSelector.cpp
    TriangleSelector.hpp
    TriangleSetSampling.cpp
    TriangleSetSampling.hpp
    TriangulateWall.cpp
    TriangulateWall.hpp
    utils.cpp
    Utils.hpp
    VariableWidth.cpp
    VariableWidth.hpp
    Zipper.cpp
    Zipper.hpp
    FilamentGroup.hpp
    FilamentGroup.cpp
    FilamentGroupUtils.hpp
    FilamentGroupUtils.cpp
    MultiNozzleUtils.hpp
    MultiNozzleUtils.cpp
    GCode/ToolOrderUtils.hpp
    GCode/ToolOrderUtils.cpp
    Gpu/GpuExactGeometry.hpp
    Gpu/CudaSlicer.hpp
    Gpu/CudaSlicer.cpp
    Gpu/VulkanSlicer.hpp
    Gpu/VulkanSlicer.cpp
    FlushVolPredictor.hpp
    FlushVolPredictor.cpp
)

if (APPLE)
    list(APPEND lisbslic3r_sources
            MacUtils.mm
            Format/ModelIO.hpp
            Format/ModelIO.mm
        )
endif ()

add_library(libslic3r STATIC ${lisbslic3r_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h"
    ${OpenVDBUtils_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lisbslic3r_sources})

if (SLIC3R_ENABLE_CUDA_SLICER)
    # Visual Studio's CUDA rule supplies host options through -Xcompiler.
    # Keep CMake's CUDA flag cache empty so MSVC switches are not emitted as
    # standalone NVCC options (CUDA 13.x rejects that form).
    set(CUDA_PROPAGATE_HOST_FLAGS OFF)
    set(CudaToolkitDir "$ENV{CUDA_PATH}" CACHE PATH "CUDA toolkit directory for Visual Studio CUDA rules")
    if (NOT CudaToolkitDir OR NOT EXISTS "${CudaToolkitDir}")
        set(CudaToolkitDir "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2" CACHE PATH "CUDA toolkit directory for Visual Studio CUDA rules" FORCE)
    endif ()
    # CUDA 13.x's MSBuild targets read this property at build time rather
    # than from CMake's cache. Export it as a global project property so GUI
    # targets built later in the solution see the same toolkit directory.
    set(CMAKE_MSVC_CUDA_TOOLKIT_DIR "${CudaToolkitDir}" CACHE PATH "CUDA toolkit directory exported to MSBuild" FORCE)
    set(CMAKE_VS_GLOBALS "CudaToolkitCustomDir=${CudaToolkitDir};CUDA_PATH_V13_2=${CudaToolkitDir}" CACHE STRING "CUDA MSBuild globals" FORCE)
    if (MSVC)
        foreach (_cuda_config "" "_DEBUG" "_MINSIZEREL" "_RELEASE" "_RELWITHDEBINFO")
            set(CMAKE_CUDA_FLAGS${_cuda_config} "" CACHE STRING "" FORCE)
        endforeach ()
        set(CMAKE_CUDA_HOST_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "MSVC host compiler for CUDA" FORCE)
        # CMake's VS generator copies the global CXX exception switch into
        # CUDA AdditionalOptions. Remove it before enabling CUDA and restore
        # the switch only for C++ sources on this target.
        string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif ()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(libslic3r PRIVATE Gpu/CudaSlicer.cu)
    set_source_files_properties(Gpu/CudaSlicer.cu PROPERTIES LANGUAGE CUDA)
    set_target_properties(libslic3r PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(libslic3r PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
        # `/EHsc` is a host-only switch; the VS CUDA rule already adds it via
        # -Xcompiler.  Keeping it out of AdditionalOptions avoids NVCC 13.x's
        # multi-input diagnostic when the project is built from MSBuild.
        set_property(TARGET libslic3r PROPERTY CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
    endif ()
    target_compile_definitions(libslic3r PRIVATE SLIC3R_ENABLE_CUDA_SLICER=1)
    target_link_libraries(libslic3r PRIVATE CUDA::cudart)

    # The CUDA driver does not provide the CUDART runtime DLL. Keep the exact
    # runtime used by this build beside the executable so a user with only a
    # compatible NVIDIA driver can run the CUDA-enabled installer.
    if (WIN32)
        get_target_property(SLIC3R_CUDA_RUNTIME_DLL CUDA::cudart IMPORTED_LOCATION)
        if (NOT SLIC3R_CUDA_RUNTIME_DLL)
            get_target_property(SLIC3R_CUDA_RUNTIME_DLL CUDA::cudart IMPORTED_LOCATION_RELEASE)
        endif ()
        if (SLIC3R_CUDA_RUNTIME_DLL AND EXISTS "${SLIC3R_CUDA_RUNTIME_DLL}")
            install(FILES "${SLIC3R_CUDA_RUNTIME_DLL}" DESTINATION ".")
        endif ()
    endif ()
endif()

if (SLIC3R_ENABLE_VULKAN_SLICER)
    find_package(Vulkan 1.2 REQUIRED)
    target_link_libraries(libslic3r PRIVATE Vulkan::Vulkan)
    target_compile_definitions(libslic3r PRIVATE SLIC3R_ENABLE_VULKAN_SLICER=1)
    # Build-tree runs link resources/ to the source tree, while the generated
    # SPIR-V modules live in this binary directory. Keep that path as a
    # development-only runtime fallback; installed packages use resources/vulkan.
    target_compile_definitions(libslic3r PRIVATE
        SLIC3R_VULKAN_BUILD_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan")

    # Use literal LocalSize shader variants. The Windows glslc shipped with
    # some SDKs compiles LocalSizeId into LocalSize=1, so pipeline
    # specialization cannot safely tune it at runtime.
    set(ORCA_VULKAN_WORKGROUP_SIZES 64 128 256 512 1024)
    set(ORCA_VULKAN_INTERSECTION_SHADERS)
    foreach(ORCA_VULKAN_WORKGROUP_SIZE IN LISTS ORCA_VULKAN_WORKGROUP_SIZES)
        set(ORCA_VULKAN_INTERSECTION_SHADER
            "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan/perimeter_infill_candidates_${ORCA_VULKAN_WORKGROUP_SIZE}.spv")
        add_custom_command(
            OUTPUT "${ORCA_VULKAN_INTERSECTION_SHADER}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
                    "-DORCA_WORKGROUP_SIZE=${ORCA_VULKAN_WORKGROUP_SIZE}"
                    -o "${ORCA_VULKAN_INTERSECTION_SHADER}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Gpu/shaders/perimeter_infill_candidates.comp"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Gpu/shaders/perimeter_infill_candidates.comp"
            VERBATIM)
        list(APPEND ORCA_VULKAN_INTERSECTION_SHADERS "${ORCA_VULKAN_INTERSECTION_SHADER}")
    endforeach()

    add_custom_target(orca_vulkan_intersection_shader ALL
        DEPENDS ${ORCA_VULKAN_INTERSECTION_SHADERS})
    add_dependencies(libslic3r orca_vulkan_intersection_shader)
    install(FILES ${ORCA_VULKAN_INTERSECTION_SHADERS}
        DESTINATION "./resources/vulkan")

    set(ORCA_VULKAN_TREE_CONTOUR_SHADER
        "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan/tree_support_contour_candidates.spv")
    add_custom_command(
        OUTPUT "${ORCA_VULKAN_TREE_CONTOUR_SHADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/resources/vulkan"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
                -o "${ORCA_VULKAN_TREE_CONTOUR_SHADER}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Gpu/shaders/tree_support_contour_candidates.comp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Gpu/shaders/tree_support_contour_candidates.comp"
        VERBATIM)
    add_custom_target(orca_vulkan_tree_contour_shader ALL
        DEPENDS "${ORCA_VULKAN_TREE_CONTOUR_SHADER}")
    add_dependencies(libslic3r orca_vulkan_tree_contour_shader)
    install(FILES "${ORCA_VULKAN_TREE_CONTOUR_SHADER}"
        DESTINATION "./resources/vulkan")

    if (ORCA_TOOLS)
        add_executable(orca-vulkan-compute-selftest Gpu/VulkanComputeSelfTest.cpp)
        target_link_libraries(orca-vulkan-compute-selftest PRIVATE libslic3r)
        target_include_directories(orca-vulkan-compute-selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif ()
endif ()

if (SLIC3R_STATIC)
    set(CGAL_Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif ()
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON CACHE BOOL "" FORCE)

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)
find_package(CGAL REQUIRED)
find_package(OpenCV REQUIRED core)
cmake_policy(POP)

add_library(libslic3r_cgal STATIC
    CutSurface.hpp CutSurface.cpp
    IntersectionPoints.hpp IntersectionPoints.cpp
    MeshBoolean.hpp MeshBoolean.cpp
    TryCatchSignal.hpp TryCatchSignal.cpp
    Triangulation.hpp Triangulation.cpp
)
target_include_directories(libslic3r_cgal PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Reset compile options of libslic3r_cgal. Despite it being linked privately, CGAL options
# (-frounding-math) still propagate to dependent libs which is not desired.
get_target_property(_cgal_tgt CGAL::CGAL ALIASED_TARGET)
if (NOT TARGET ${_cgal_tgt})
    set (_cgal_tgt CGAL::CGAL)
endif ()
get_target_property(_opts ${_cgal_tgt} INTERFACE_COMPILE_OPTIONS)
if (_opts)
    set(_opts_bad "${_opts}")
    set(_opts_good "${_opts}")
    list(FILTER _opts_bad INCLUDE REGEX frounding-math)
    list(FILTER _opts_good EXCLUDE REGEX frounding-math)
    set_target_properties(${_cgal_tgt} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts_good}")
    target_compile_options(libslic3r_cgal PRIVATE "${_opts_bad}")
endif()

target_link_libraries(libslic3r_cgal PRIVATE ${_cgal_tgt} admesh libigl mcut boost_libs)

if (MSVC AND "${CMAKE_SIZEOF_VOID_P}" STREQUAL "4") # 32 bit MSVC workaround
    target_compile_definitions(libslic3r_cgal PRIVATE CGAL_DO_NOT_USE_MPZF)
endif ()

encoding_check(libslic3r)

target_compile_definitions(libslic3r PUBLIC -DUSE_TBB -DTBB_USE_CAPTURED_EXCEPTION=0)
target_compile_definitions(libslic3r PRIVATE $<$<CONFIG:RelWithDebInfo>:SLIC3R_CONSOLE_LOG>)
target_include_directories(libslic3r PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(libslic3r SYSTEM PUBLIC ${EXPAT_INCLUDE_DIRS})

# Find the OCCT and related libraries
set(OpenCASCADE_DIR "${CMAKE_PREFIX_PATH}/lib/cmake/occt")
find_package(OpenCASCADE REQUIRED)
target_include_directories(libslic3r SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})

find_package(JPEG REQUIRED)
find_package(draco REQUIRED)

set(OCCT_LIBS
    TKXDESTEP
    TKSTEP
    TKSTEP209
    TKSTEPAttr
    TKSTEPBase
    TKXCAF
    TKXSBase
    TKVCAF
    TKCAF
    TKLCAF
    TKCDF
    TKV3d
    TKService
    TKMesh
    TKBO
    TKPrim
    TKHLR
    TKShHealing
    TKTopAlgo
    TKGeomAlgo
    TKBRep
    TKGeomBase
    TKG3d
    TKG2d
    TKMath
    TKernel
)

target_link_libraries(libslic3r
    PUBLIC
        Eigen3::Eigen
        admesh
        libigl
        libnest2d
        miniz
        opencv_world
    PRIVATE
        ${CMAKE_DL_LIBS}
        ${EXPAT_LIBRARIES}
        ${OCCT_LIBS}
        boost_libs
        cereal::cereal
        clipper
        Clipper2
        draco::draco
        glu-libtess
        JPEG::JPEG
        libslic3r_cgal
        mcut
        noise::noise
        PNG::PNG
        qhull
        qoi
        semver
        TBB::tbb
        TBB::tbbmalloc
        ZLIB::ZLIB
        OpenSSL::Crypto
    )

if(NOT WIN32)
    # Link freetype for OCCT dependency (CAD operations need font rendering)
    target_link_libraries(libslic3r PRIVATE ${FREETYPE_LIBRARIES})
    if (NOT APPLE)
        target_link_libraries(libslic3r PRIVATE fontconfig)
    endif()
endif()

if (APPLE)
    find_library(FOUNDATION Foundation REQUIRED)
    find_library(MODELIO ModelIO REQUIRED)
    target_link_libraries(libslic3r PRIVATE ${FOUNDATION} ${MODELIO})
endif ()

if (TARGET OpenVDB::openvdb)
    target_link_libraries(libslic3r PRIVATE OpenVDB::openvdb)
endif()

if(WIN32)
    target_link_libraries(libslic3r PRIVATE Psapi.lib bcrypt.lib)
endif()

if(SLIC3R_PROFILE)
    target_link_libraries(libslic3r PRIVATE Shiny)
endif()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r pchheader.hpp FORCEINCLUDE)
endif ()
