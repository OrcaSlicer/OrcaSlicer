// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_STR_H
#define IGL_STR_H
#include <string>
#include <sstream>
/// Convert a stream of things to std:;string
///
/// Suppose you have a function:
/// \code{cpp}
/// void func(std::string s);
/// \endcode
/// Then you can write:
/// \code{cpp}
///   func(C_STR("foo"<<1<<"bar"));
/// \endcode
/// which is equivalent to:
/// \code{cpp}
///   func("foo1bar");
/// \endcode
///
// http://stackoverflow.com/a/2433143/148668
#define STR(X) static_cast<std::ostringstream&>(std::ostringstream().flush() << X).str()
#endif 
