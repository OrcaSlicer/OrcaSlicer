
if (WIN32)
    set(HIDAPI_IMPL win/hid.c)
elseif (APPLE)
    set(HIDAPI_IMPL mac/hid.c)
else ()
    # Assume Linux or Unix other than Mac OS
    set(HIDAPI_IMPL linux/hid.c)
endif()

include_directories(include)

add_library(hidapi STATIC ${HIDAPI_IMPL})
target_include_directories(hidapi SYSTEM
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	# Don't link the udev library, as there are two versions out there (libudev.so.0, libudev.so.1), so they are linked explicitely.
#	target_link_libraries(hidapi udev)
	target_link_libraries(hidapi dl)
endif()
