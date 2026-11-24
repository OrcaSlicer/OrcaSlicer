set(_curl_platform_flags 
  -DENABLE_IPV6:BOOL=ON
  -DENABLE_THREADED_RESOLVER:BOOL=ON
  -DENABLE_CURL_MANUAL:BOOL=OFF
  -DCURL_DISABLE_LDAP:BOOL=ON
  -DCURL_DISABLE_LDAPS:BOOL=ON
  -DCURL_DISABLE_RTSP:BOOL=ON
  -DCURL_DISABLE_DICT:BOOL=ON
  -DCURL_DISABLE_TELNET:BOOL=ON
  -DCURL_DISABLE_POP3:BOOL=ON
  -DCURL_DISABLE_IMAP:BOOL=ON
  -DCURL_DISABLE_SMTP:BOOL=ON
  -DCURL_DISABLE_GOPHER:BOOL=ON
  -DCURL_DISABLE_TFTP:BOOL=ON
  -DCURL_DISABLE_MQTT:BOOL=ON
  -DCURL_DISABLE_WEBSOCKETS:BOOL=ON

  # Optional libraries curl 8 enables or probes for by default. Keep the set
  # of features the same as before: OpenSSL and zlib only, so no new shared
  # library sneaks into the AppImage or the macOS bundle.
  -DCURL_USE_GSSAPI:BOOL=OFF
  -DCURL_USE_LIBSSH2:BOOL=OFF
  -DCURL_USE_LIBSSH:BOOL=OFF
  -DCURL_USE_LIBPSL:BOOL=OFF
  -DUSE_LIBIDN2:BOOL=OFF
  -DUSE_NGHTTP2:BOOL=OFF
  -DCURL_USE_MBEDTLS:BOOL=OFF
  -DCURL_ZLIB:STRING=ON
  -DCURL_BROTLI:STRING=OFF
  -DCURL_ZSTD:STRING=OFF
)

if (WIN32)
  #set(_curl_platform_flags  ${_curl_platform_flags} -DCURL_USE_SCHANNEL=ON)
  set(_curl_platform_flags  ${_curl_platform_flags} -DCURL_USE_OPENSSL=ON -DCURL_CA_PATH:STRING=none)
elseif (APPLE)
  set(_curl_platform_flags 
    
    ${_curl_platform_flags}

    #-DCURL_USE_SECTRANSP:BOOL=ON
    -DCURL_USE_OPENSSL:BOOL=ON

    -DCURL_CA_PATH:STRING=none
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_curl_platform_flags 

    ${_curl_platform_flags}

    -DCURL_USE_OPENSSL:BOOL=ON

    -DCURL_CA_PATH:STRING=none
    -DCURL_CA_BUNDLE:STRING=none
    -DCURL_CA_FALLBACK:BOOL=ON
  )
endif ()

if (BUILD_SHARED_LIBS)
  set(_curl_static OFF)
else()
  set(_curl_static ON)
endif()

orcaslicer_add_cmake_project(CURL
  URL                 https://github.com/curl/curl/archive/refs/tags/curl-8_23_0.zip
  URL_HASH            SHA256=4f443969ea4ea6ccb5ac984488683bb92384bc4df0affa3938ce77f20b30d853
  DEPENDS             ${ZLIB_PKG}
  CMAKE_ARGS
    -DBUILD_TESTING:BOOL=OFF
    -DBUILD_CURL_EXE:BOOL=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_STATIC_LIBS:BOOL=${_curl_static}
    ${_curl_platform_flags}
)

if(NOT OPENSSL_FOUND)
  # (openssl may or may not be built)
  add_dependencies(dep_CURL ${OPENSSL_PKG})
endif()

if (MSVC)
    add_debug_dep(dep_CURL)
endif ()
