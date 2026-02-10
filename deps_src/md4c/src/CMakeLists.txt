
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")


# Build rules for MD4C parser library

configure_file(md4c.pc.in md4c.pc @ONLY)
add_library(md4c md4c.c md4c.h)
set_target_properties(md4c PROPERTIES
    COMPILE_FLAGS "-DMD4C_USE_UTF8"
    VERSION ${MD_VERSION}
    SOVERSION ${MD_VERSION_MAJOR}
    PUBLIC_HEADER md4c.h
)

# Build rules for HTML renderer library

configure_file(md4c-html.pc.in md4c-html.pc @ONLY)
add_library(md4c-html md4c-html.c md4c-html.h entity.c entity.h)
set_target_properties(md4c-html PROPERTIES
    VERSION ${MD_VERSION}
    SOVERSION ${MD_VERSION_MAJOR}
    PUBLIC_HEADER md4c-html.h
)
target_link_libraries(md4c-html md4c)


# Install rules

install(
    TARGETS md4c
    EXPORT md4cConfig
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES ${CMAKE_BINARY_DIR}/src/md4c.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(
    TARGETS md4c-html
    EXPORT md4cConfig
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES ${CMAKE_BINARY_DIR}/src/md4c-html.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(EXPORT md4cConfig DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/md4c/ NAMESPACE md4c::)

