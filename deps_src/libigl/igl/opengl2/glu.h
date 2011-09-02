// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_GLU_H
#define IGL_OPENGL2_GLU_H

#ifdef IGL_OPENGL_GLU_H
#  error "igl/opengl/glu.h already included"
#endif

// Always use this:
//     #include "glu.h"
// Instead of:
//     #include <OpenGL/glu.h>
// or 
//     #include <GL/glu.h>
//

#ifdef __APPLE__
#  include <OpenGL/glu.h>
#else
#  include <GL/glu.h>
#endif

#endif

