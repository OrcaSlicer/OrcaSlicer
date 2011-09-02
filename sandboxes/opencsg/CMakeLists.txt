cmake_minimum_required(VERSION 3.0)

project(OpenCSG-example)

add_executable(opencsg_example WIN32 
    main.cpp 
    Engine.hpp Engine.cpp 
    ShaderCSGDisplay.hpp ShaderCSGDisplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/GUI/Jobs/Job.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/GUI/ProgressStatusBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/GUI/I18N.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/GUI/I18N.cpp)

find_package(wxWidgets 3.1 REQUIRED COMPONENTS core base gl html)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenCSG REQUIRED)
include(${wxWidgets_USE_FILE})

target_link_libraries(opencsg_example libslic3r)
target_include_directories(opencsg_example PRIVATE ${wxWidgets_INCLUDE_DIRS})
target_compile_definitions(opencsg_example PRIVATE ${wxWidgets_DEFINITIONS})

slic3r_remap_configs(OpenCSG::opencsg RelWithDebInfo Release)
target_link_libraries(opencsg_example ${wxWidgets_LIBRARIES} 
    OpenCSG::opencsg 
    GLEW::GLEW
    OpenGL::GL 
    #-lXrandr -lXext -lX11
    )
