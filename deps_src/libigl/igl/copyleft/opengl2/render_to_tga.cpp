// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "render_to_tga.h"
#include "tga.h"

#include "../../opengl2/gl.h"

#include <cstdlib>

IGL_INLINE bool igl::opengl::render_to_tga(
  const std::string tga_file,
  const int width,
  const int height,
  const bool alpha)
{

  size_t components = 3;
  GLenum format = GL_BGR;
  if(alpha)
  {
    format = GL_BGRA;
    components = 4;
  }
  GLubyte * cmap = NULL;

  // OpenGL by default tries to read data in multiples of 4, if our data is
  // only RGB or BGR and the width is not divible by 4 then we need to alert
  // opengl
  if((width % 4) != 0 && 
   (format == GL_RGB || 
    format == GL_BGR))
  {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
  }
  GLubyte *pixels;
  pixels = (unsigned char *) malloc (width * height * components);
  glReadPixels(
    0,
    0,
    width,
    height,
    format,
    GL_UNSIGNED_BYTE,
    pixels);

  // set up generic image struct
  gliGenericImage * genericImage;
  genericImage = (gliGenericImage*) malloc(sizeof(gliGenericImage));
  genericImage->width = width;
  genericImage->height = height;
  genericImage->format = format;
  genericImage->components = components;
  genericImage->pixels = pixels;
  // CMAP is not supported, but we need to put something here
  genericImage->cmapEntries = 0;
  genericImage->cmapFormat = GL_BGR;
  genericImage->cmap = cmap;

  // write pixels to tga file
  FILE * imgFile;
  // "-" as output file name is code for write to stdout
  if(tga_file.compare("-") == 0)
  {
    imgFile = stdout;
  }else{
    imgFile = fopen(tga_file.c_str(),"w");
    if(NULL==imgFile)
    {
      printf("IOError: %s could not be opened...\n",tga_file.c_str());
      free(genericImage);
      return false;
    }
  }

  writeTGA(genericImage,imgFile);

  free(genericImage);
  return fclose(imgFile) == 0;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
