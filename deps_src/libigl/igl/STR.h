// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_STR_H
#define IGL_STR_H
// http://stackoverflow.com/a/2433143/148668
#include <string>
#include <sstream>
// Suppose you have a function:
//   void func(std::string c);
// Then you can write:
//   func(STR("foo"<<1<<"bar"));
#define STR(X) static_cast<std::ostringstream&>(std::ostringstream().flush() << X).str()
#endif 
