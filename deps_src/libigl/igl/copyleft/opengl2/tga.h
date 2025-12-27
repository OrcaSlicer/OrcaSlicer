// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_TGA_H
#define IGL_OPENGL_TGA_H
#include "../../igl_inline.h"

#include "../../opengl2/gl.h"
// See license in tga.cpp
/* tga.h - interface for TrueVision (TGA) image file loader */
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace igl
{
namespace opengl
{

typedef struct {

  GLsizei  width;
  GLsizei  height;
  GLint    components;
  GLenum   format;

  GLsizei  cmapEntries;
  GLenum   cmapFormat;
  GLubyte *cmap;

  GLubyte *pixels;
  
} gliGenericImage;

typedef struct {
  unsigned char idLength;
  unsigned char colorMapType;

  /* The image type. */
#define TGA_TYPE_MAPPED 1
#define TGA_TYPE_COLOR 2
#define TGA_TYPE_GRAY 3
#define TGA_TYPE_MAPPED_RLE 9
#define TGA_TYPE_COLOR_RLE 10
#define TGA_TYPE_GRAY_RLE 11
  unsigned char imageType;

  /* Color Map Specification. */
  /* We need to separately specify high and low bytes to avoid endianness
     and alignment problems. */
  unsigned char colorMapIndexLo, colorMapIndexHi;
  unsigned char colorMapLengthLo, colorMapLengthHi;
  unsigned char colorMapSize;

  /* Image Specification. */
  unsigned char xOriginLo, xOriginHi;
  unsigned char yOriginLo, yOriginHi;

  unsigned char widthLo, widthHi;
  unsigned char heightLo, heightHi;

  unsigned char bpp;

  /* Image descriptor.
     3-0: attribute bpp
     4:   left-to-right ordering
     5:   top-to-bottom ordering
     7-6: zero
     */
#define TGA_DESC_ABITS 0x0f
#define TGA_DESC_HORIZONTAL 0x10
#define TGA_DESC_VERTICAL 0x20
  unsigned char descriptor;

} TgaHeader;

typedef struct {
  unsigned int extensionAreaOffset;
  unsigned int developerDirectoryOffset;
#define TGA_SIGNATURE "TRUEVISION-XFILE"
  char signature[16];
  char dot;
  char null;
} TgaFooter;

IGL_INLINE extern gliGenericImage *gliReadTGA(FILE *fp, char *name, int hflip, int vflip);
IGL_INLINE int gli_verbose(int new_verbose);
IGL_INLINE extern int gliVerbose(int newVerbose);

IGL_INLINE void writeTGA( gliGenericImage* image, FILE *fp);



} // end of igl namespace
}

#ifndef IGL_STATIC_LIBRARY
#  include "tga.cpp"
#endif

#endif 
