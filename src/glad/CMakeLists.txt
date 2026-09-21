add_library(glad STATIC src/gl.c)
target_include_directories(glad PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(glad PRIVATE ${CMAKE_DL_LIBS})
