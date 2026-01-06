cmake_minimum_required(VERSION 3.13)
project(libvgcode)

# glad library
if (EMSCRIPTEN OR SLIC3R_OPENGL_ES)
    set(GLAD_SOURCES
        glad/include/glad/gles2.h
        glad/include/KHR/khrplatform.h
        glad/src/gles2.c
    )
else ()
    set(GLAD_SOURCES
        glad/include/glad/gl.h
        glad/include/KHR/khrplatform.h
        glad/src/gl.c
    )
endif ()

set(LIBVGCODE_SOURCES
	# API
	include/ColorPrint.hpp
    include/ColorRange.hpp
    include/GCodeInputData.hpp
    include/PathVertex.hpp
    include/Types.hpp
    include/Viewer.hpp
	# source
	src/Bitset.hpp
	src/Bitset.cpp
	src/CogMarker.hpp
	src/CogMarker.cpp
	src/ColorPrint.cpp
	src/ColorRange.cpp
	src/ExtrusionRoles.hpp
	src/ExtrusionRoles.cpp
	src/GCodeInputData.cpp
	src/Layers.hpp
	src/Layers.cpp
	src/OpenGLUtils.hpp
	src/OpenGLUtils.cpp
	src/OptionTemplate.hpp
	src/OptionTemplate.cpp
	src/PathVertex.cpp
	src/Range.hpp
	src/Range.cpp
	src/SegmentTemplate.hpp
	src/SegmentTemplate.cpp
	src/Settings.hpp
	src/Settings.cpp
	src/Shaders.hpp
	src/ShadersES.hpp
	src/ToolMarker.hpp
	src/ToolMarker.cpp
	src/Types.cpp
	src/Utils.hpp
	src/Utils.cpp
	src/Viewer.cpp
	src/ViewerImpl.hpp
	src/ViewerImpl.cpp
	src/ViewRange.hpp
	src/ViewRange.cpp
	${GLAD_SOURCES}
)

add_library(libvgcode STATIC ${LIBVGCODE_SOURCES})

if (EMSCRIPTEN OR SLIC3R_OPENGL_ES)
    add_compile_definitions(ENABLE_OPENGL_ES)
endif()

if (WIN32)
    foreach(_source IN ITEMS ${LIBVGCODE_SOURCES})
        get_filename_component(_source_path "${_source}" PATH)
        string(REPLACE "/" "\\" _group_path "${_source_path}")
        source_group("${_group_path}" FILES "${_source}")
    endforeach()
endif ()

# glad includes
include_directories(glad/include)