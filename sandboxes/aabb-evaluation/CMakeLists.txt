add_executable(aabb-evaluation aabb-evaluation.cpp)
target_link_libraries(aabb-evaluation libslic3r ${Boost_LIBRARIES} ${TBB_LIBRARIES} ${Boost_LIBRARIES} ${CMAKE_DL_LIBS})
