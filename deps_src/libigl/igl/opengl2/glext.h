// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLEXT_H
#define IGL_OPENGL_GLEXT_H

#ifdef IGL_OPENGL2_GLEXT_H
#  error "igl/opengl2/glext.h already included"
#endif

// Always use this:
//     #include "gl.h"
// Instead of:
//     #include <OpenGL/glext.h>
// or 
//     #include <GL/glext.h>
//

#ifdef __APPLE__
#  include <OpenGL/glext.h>
#elif _WIN32
// do nothing(?)
#else 
#  include <GL/glext.h>
#endif

#endif

