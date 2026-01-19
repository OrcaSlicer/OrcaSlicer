#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenSSL::SSL" for configuration "Release"
set_property(TARGET OpenSSL::SSL APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenSSL::SSL PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libssl.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS OpenSSL::SSL )
list(APPEND _IMPORT_CHECK_FILES_FOR_OpenSSL::SSL "${_IMPORT_PREFIX}/lib/libssl.lib" )

# Import target "OpenSSL::Crypto" for configuration "Debug"
set_property(TARGET OpenSSL::Crypto APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenSSL::Crypto PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcrypto.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS OpenSSL::Crypto )
list(APPEND _IMPORT_CHECK_FILES_FOR_OpenSSL::Crypto "${_IMPORT_PREFIX}/lib/libcrypto.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
