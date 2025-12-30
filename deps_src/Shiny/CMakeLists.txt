cmake_minimum_required(VERSION 3.13)
project(Shiny)

add_library(Shiny STATIC
    Shiny.h
    ShinyConfig.h
    ShinyData.h
    ShinyMacros.h
    ShinyManager.c
    ShinyManager.h
    ShinyNode.c
    ShinyNode.h
    ShinyNodePool.c
    ShinyNodePool.h
    ShinyNodeState.c
    ShinyNodeState.h
    ShinyOutput.c
    ShinyOutput.h
    ShinyPrereqs.h
    ShinyTools.c
    ShinyTools.h
    ShinyVersion.h
    ShinyZone.c
    ShinyZone.h
)

target_include_directories(Shiny SYSTEM
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
