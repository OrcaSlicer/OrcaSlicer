# Boost.Locale lists include/boost/locale.hpp in a source_group(TREE) rooted at include/boost/locale.
# Before 4.5, CMake compared the two as strings and accepted it. CMake 4.5 reworked source_group() and
# compares path components, so it stops the configure with "is not a prefix of file". locale.hpp gets its own group on the
# following line of that file, so only the headers under include/boost/locale/ go into the tree.
#
# Run as the PATCH_COMMAND with -DSOURCE_DIR=<Boost source>. A second run leaves the file as it is.
set(_file "${SOURCE_DIR}/libs/locale/CMakeLists.txt")
set(_old [[list(FILTER sources_inc EXCLUDE REGEX "^src/")]])
set(_new [[list(FILTER sources_inc INCLUDE REGEX "^include/boost/locale/")]])

file(READ "${_file}" _content)
string(FIND "${_content}" "${_new}" _patched)
if (_patched EQUAL -1)
    string(FIND "${_content}" "${_old}" _found)
    if (_found EQUAL -1)
        message(FATAL_ERROR "${_file} no longer has the line this script replaces. Check whether this Boost "
                            "version still needs deps/Boost/fix_locale_source_group.cmake.")
    endif ()
    string(REPLACE "${_old}" "${_new}" _content "${_content}")
    file(WRITE "${_file}" "${_content}")
endif ()
