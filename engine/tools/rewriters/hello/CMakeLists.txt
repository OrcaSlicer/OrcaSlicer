cmake_minimum_required(VERSION 3.20)
project(orca-hello C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Clang REQUIRED CONFIG)
find_package(LLVM  REQUIRED CONFIG)

add_executable(orca-hello main.cpp)

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
    OUTPUT_VARIABLE _detected_resource_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _detect_rc)
if(NOT _detect_rc EQUAL 0 OR NOT _detected_resource_dir)
    set(_detected_resource_dir "/usr/lib/clang/${LLVM_VERSION_MAJOR}")
endif()
target_compile_definitions(orca-hello PRIVATE
    CLANG_RESOURCE_DIR="${_detected_resource_dir}")

target_include_directories(orca-hello PRIVATE
    ${CLANG_INCLUDE_DIRS}
    ${LLVM_INCLUDE_DIRS})

# libclang-cpp.so re-exports the whole tooling surface as a single library on
# modern LLVM/Clang installs. Linking against it avoids having to enumerate
# every clang* component target.
target_link_libraries(orca-hello PRIVATE clang-cpp LLVM)
