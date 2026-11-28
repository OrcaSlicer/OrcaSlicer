#ifndef IGL_GUESS_EXTENSION_H
#define IGL_GUESS_EXTENSION_H
#include "igl_inline.h"
#include <string>
#include <cstdio>
namespace igl
{
  /// Given a file pointer at the beginning of a "mesh" file, try to guess the
  /// extension of the file format it comes from. The file pointer is rewound on
  /// return.
  ///
  /// @param[in,out]  fp  file pointer, rewound after
  /// @param[out] guess  extension as string. One of "mesh",{"obj"},"off","ply","stl", or
  ///     "wrl"
  ///
  IGL_INLINE void guess_extension(FILE * fp, std::string & guess);
  /// \overload
  IGL_INLINE std::string guess_extension(FILE * fp);
}
#ifndef IGL_STATIC_LIBRARY
#  include "guess_extension.cpp"
#endif
#endif
