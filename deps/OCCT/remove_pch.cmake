# Deletes the precompiled headers OCCT leaves in its build tree (see OCCT.cmake).
file(GLOB_RECURSE _pch "${DIR}/*.pch" "${DIR}/*.gch")
if (_pch)
    file(REMOVE ${_pch})
endif ()