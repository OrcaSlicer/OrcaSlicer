// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ANTTWEAKBAR_COCOA_KEY_TO_ANTTWEAKBAR_KEY_H
#define IGL_ANTTWEAKBAR_COCOA_KEY_TO_ANTTWEAKBAR_KEY_H
#include "../igl_inline.h"


namespace igl
{
  namespace anttweakbar
  {
    // Convert an unsigned char (like that from Cocoa apps) to AntTweakBar key
    // code.
    // See also: TranslateKey() in TwMgr.cpp in AntTweakBar source
    // Inputs:
    //   key  unsigned char key from keyboard
    // Returns int of new key code 
    IGL_INLINE int cocoa_key_to_anttweakbar_key(int key);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "cocoa_key_to_anttweakbar_key.cpp"
#endif

#endif
