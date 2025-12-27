project(minilzo)
cmake_minimum_required(VERSION 3.13)


add_library(minilzo INTERFACE)

add_library(minilzo_static STATIC
	minilzo.c
	minilzo.h
	lzodefs.h
	lzoconf.h
)

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
	target_compile_definitions(minilzo_static PRIVATE _GNU_SOURCE)
endif()

target_link_libraries(minilzo INTERFACE minilzo_static)
target_include_directories(minilzo SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

message(STATUS "Minilzo using bundled version...")

