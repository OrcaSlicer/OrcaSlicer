// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PATHINFO_H
#define IGL_PATHINFO_H
#include "igl_inline.h"

#include <string>

namespace igl
{
  //// Decided not to use these
  //const int PATHINFO_DIRNAME 01
  //const int PATHINFO_BASENAME 02
  //const int PATHINFO_EXTENSION 04
  //const int PATHINFO_FILENAME 08

  // Function like PHP's pathinfo
  //  returns information about path
  // Input:
  //  path  string containing input path
  // Outputs:
  //  dirname  string containing dirname (see dirname.h)
  //  basename  string containing basename (see basename.h)
  //  extension  string containing extension (characters after last '.')
  //  filename  string containing filename (characters of basename before last
  //    '.')
  //
  //
  // Examples:
  //
  // input                     | dirname        basename       ext    filename
  // "/"                       | "/"            ""             ""     ""
  // "//"                      | "/"            ""             ""     ""
  // "/foo"                    | "/"            "foo"          ""     "foo"
  // "/foo/"                   | "/"            "foo"          ""     "foo"
  // "/foo//"                  | "/"            "foo"          ""     "foo"
  // "/foo/./"                 | "/foo"         "."            ""     ""
  // "/foo/bar"                | "/foo"         "bar"          ""     "bar"
  // "/foo/bar."               | "/foo"         "bar."         ""     "bar"
  // "/foo/bar.txt"            | "/foo"         "bar.txt"      "txt"  "bar"
  // "/foo/bar.txt.zip"        | "/foo"         "bar.txt.zip"  "zip"  "bar.txt"
  // "/foo/bar.dir/"           | "/foo"         "bar.dir"      "dir"  "bar"
  // "/foo/bar.dir/file"       | "/foo/bar.dir" "file"         ""     "file"
  // "/foo/bar.dir/file.txt"   | "/foo/bar.dir" "file.txt"     "txt"  "file"
  //  See also: basename, dirname
  IGL_INLINE void pathinfo(
    const std::string & path,
    std::string & dirname,
    std::string & basename,
    std::string & extension,
    std::string & filename);

}

#ifndef IGL_STATIC_LIBRARY
#  include "pathinfo.cpp"
#endif

#endif
