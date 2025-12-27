// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DEPRECATED_H
#define IGL_DEPRECATED_H
// Macro for marking a function as deprecated.
// 
// http://stackoverflow.com/a/295229/148668
#ifdef __GNUC__
#define IGL_DEPRECATED(func) func __attribute__ ((deprecated))
#elif defined(_MSC_VER)
#define IGL_DEPRECATED(func) __declspec(deprecated) func
#else
#pragma message("WARNING: You need to implement IGL_DEPRECATED for this compiler")
#define IGL_DEPRECATED(func) func
#endif
// Usage:
//
//     template <typename T> IGL_INLINE void my_func(Arg1 a);
//
// becomes 
//
//     template <typename T> IGL_INLINE IGL_DEPRECATED(void my_func(Arg1 a));
#endif
