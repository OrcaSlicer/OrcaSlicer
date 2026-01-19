// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
// This should *NOT* be contained in a IGL_*_H ifdef, since it may be defined
// differently based on when it is included
#ifdef IGL_INLINE
#undef IGL_INLINE
#endif

#ifndef IGL_STATIC_LIBRARY
#  define IGL_INLINE inline
#else
#  define IGL_INLINE
#endif
