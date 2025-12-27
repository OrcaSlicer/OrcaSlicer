cmake_minimum_required(VERSION 3.13)

project(imgui)

add_library(imgui STATIC
    imconfig.h
    imgui.cpp
    imgui.h
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_internal.h
    imgui_stdlib.cpp
    imgui_stdlib.h
    imgui_tables.cpp
    imgui_widgets.cpp
    # imgui STB
    imstb_rectpack.h
    imstb_textedit.h
    imstb_truetype.h
)

target_include_directories(imgui SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(Boost_FOUND)
    target_link_libraries(imgui PRIVATE boost_headeronly)
endif()
