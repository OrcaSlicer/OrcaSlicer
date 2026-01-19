#///////////////////////////////////////////////////////////////////////////
#//-------------------------------------------------------------------------
#//
#// Description:
#//      cmake module for finding NLopt installation
#//
#//      Example usage:
#//          find_package(NLopt 1.4 REQUIRED)
#//
#//
#//-------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

unset(_q)
if (NLopt_FIND_QUIETLY)
	set(_q QUIET)
endif ()

find_package(NLopt ${NLopt_VERSION} CONFIG ${_q})
find_package_handle_standard_args(NLopt CONFIG_MODE)

# report result
if(NLopt_FOUND AND NOT _q)
	get_filename_component(NLOPT_LIBRARY_DIRS ${NLOPT_LIBRARY_DIRS} ABSOLUTE)
	get_filename_component(NLOPT_INCLUDE_DIRS ${NLOPT_INCLUDE_DIRS} ABSOLUTE)

	message(STATUS "Found NLopt in '${NLOPT_LIBRARY_DIRS}'.")
	message(STATUS "Using NLopt include directory '${NLOPT_INCLUDE_DIRS}'.")

	get_target_property(_configs NLopt::nlopt IMPORTED_CONFIGURATIONS)
	foreach (_config ${_configs})
		get_target_property(_lib NLopt::nlopt IMPORTED_LOCATION_${_config})
		message(STATUS "Found NLopt ${_config} library: ${_lib}")
	endforeach ()
endif()
