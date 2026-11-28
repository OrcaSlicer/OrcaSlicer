#ifndef IGL_IS_STL_H
#define IGL_IS_STL_H
#include "igl_inline.h"
#include <cstdio>
namespace igl
{
  /// Given a file pointer, determine if it contains an .stl file and then
  /// rewind it.
  /// 
  /// @param[in] stl_file  pointer to file 
  /// @param[in] is_ascii  flag whether stl is ascii
  /// @return whether stl_file is an .stl file
  IGL_INLINE bool is_stl(FILE * stl_file, bool & is_ascii);
  IGL_INLINE bool is_stl(FILE * stl_file);
};
#ifndef IGL_STATIC_LIBRARY
#  include "is_stl.cpp"
#endif
#endif
