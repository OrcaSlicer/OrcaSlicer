# Standalone stub to iterate on the "Send print job" plate-changer table layout.
# Build with system wxWidgets (no OrcaSlicer build required):
#   cd send_dialog_layout_stub && mkdir build && cd build
#   cmake -DCMAKE_BUILD_TYPE=Release ..
#   cmake --build .
#   ./send_dialog_layout

cmake_minimum_required(VERSION 3.13)
project(send_dialog_layout CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(wxWidgets 3.0 REQUIRED core base)
include(${wxWidgets_USE_FILE})

add_executable(send_dialog_layout main.cpp)
target_link_libraries(send_dialog_layout ${wxWidgets_LIBRARIES})
target_include_directories(send_dialog_layout PRIVATE ${wxWidgets_INCLUDE_DIRS})
target_compile_definitions(send_dialog_layout PRIVATE ${wxWidgets_DEFINITIONS})
