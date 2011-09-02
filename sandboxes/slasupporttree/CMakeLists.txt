add_executable(slasupporttree slasupporttree.cpp)
target_link_libraries(slasupporttree libslic3r ${Boost_LIBRARIES} ${TBB_LIBRARIES} ${Boost_LIBRARIES} ${CMAKE_DL_LIBS})
