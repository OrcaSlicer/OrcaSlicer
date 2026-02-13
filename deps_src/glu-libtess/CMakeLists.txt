cmake_minimum_required(VERSION 3.13)

project(glu-libtess)

add_library(glu-libtess STATIC
	src/dict-list.h
	src/dict.c
	src/dict.h
	src/geom.c
	src/geom.h
	src/gluos.h
	src/memalloc.c
	src/memalloc.h
	src/mesh.c
	src/mesh.h
	src/normal.c
	src/normal.h
	src/priorityq.c
	src/priorityq.h
	src/priorityq-heap.h
	src/priorityq-sort.h
	src/render.c
	src/render.h
	src/sweep.c
	src/sweep.h
	src/tess.c
	src/tess.h
	src/tessmono.c
	src/tessmono.h
	include/glu-libtess.h
	)

if(UNIX)
    target_link_libraries(glu-libtess m)
endif(UNIX)

target_include_directories(glu-libtess SYSTEM
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
