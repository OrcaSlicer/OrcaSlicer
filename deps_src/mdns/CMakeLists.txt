cmake_minimum_required(VERSION 3.13)

project(mdns)

# Static library wrapping mjansson/mdns (public domain) plus the cxmdns C++
# wrapper from CrealityPrint v7.1.1 (AGPL-3.0). See NOTICE.md for attribution.
add_library(mdns STATIC
    mdns.h
    mdns.c
    cxmdns.h
    cxmdns.cpp
)

target_include_directories(mdns SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (MSVC)
    # mjansson/mdns uses GetAdaptersAddresses (Iphlpapi) and Winsock2 (Ws2_32).
    target_link_libraries(mdns PUBLIC Iphlpapi Ws2_32)
endif()
