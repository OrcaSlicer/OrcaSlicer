#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenSSL::SSL" for configuration "Debug"
set_property(TARGET OpenSSL::SSL APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenSSL::SSL PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libssl.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS OpenSSL::SSL )
list(APPEND _IMPORT_CHECK_FILES_FOR_OpenSSL::SSL "${_IMPORT_PREFIX}/lib/libssl.lib" )

# Import target "OpenSSL::Crypto" for configuration "Debug"
set_property(TARGET OpenSSL::Crypto APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenSSL::Crypto PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libcrypto.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS OpenSSL::Crypto )
list(APPEND _IMPORT_CHECK_FILES_FOR_OpenSSL::Crypto "${_IMPORT_PREFIX}/lib/libcrypto.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
