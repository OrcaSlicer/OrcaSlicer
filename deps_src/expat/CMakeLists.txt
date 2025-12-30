cmake_minimum_required(VERSION 3.13)
project(expat)

add_library(expat STATIC
    xmlparse.c
    xmlrole.c
    xmltok.c
)

target_include_directories(expat SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(expat PRIVATE
    ascii.h
    asciitab.h
    expat.h
    expat_config.h
    expat_external.h
    iasciitab.h
    internal.h
    latin1tab.h
    nametab.h
    utf8tab.h
    xmlrole.h
    xmltok.h
    xmltok_impl.h
)

if(WIN32)
    target_compile_definitions(expat PRIVATE COMPILED_FROM_DSP XML_STATIC)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(expat PRIVATE -Wno-unused-parameter)
endif()
