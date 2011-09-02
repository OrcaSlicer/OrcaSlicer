// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cocoa_key_to_anttweakbar_key.h"

#include <AntTweakBar.h>

IGL_INLINE int igl::anttweakbar::cocoa_key_to_anttweakbar_key(int key)
{
  // I've left commented the AntTweakBar key codes that correspond to keys I
  // don't have on my keyboard. Please fill this in if you have those keys
  switch(key)
  {
    case 127:
      return TW_KEY_BACKSPACE;
    case 9:
      return TW_KEY_TAB;
  //TW_KEY_CLEAR        = 0x0c,
    case 3://ENTER
    case 13:
      return TW_KEY_RETURN;
    case 27:
      return TW_KEY_ESCAPE;
    case 32:
      return TW_KEY_SPACE;
    // IN A GLUT APP 40 is (
    //case 40:
    case 63272:
      return TW_KEY_DELETE;
    case 63232:
      return TW_KEY_UP;
    case 63233:
      return TW_KEY_DOWN;
    case 63235:
      return TW_KEY_RIGHT;
    case 63234:
      return TW_KEY_LEFT;
  //TW_KEY_INSERT,
  //TW_KEY_HOME,
  //TW_KEY_END,
  //TW_KEY_PAGE_UP,
  //TW_KEY_PAGE_DOWN,
    case 63236:
      return TW_KEY_F1;
    case 63237:
      return TW_KEY_F2;
    case 63238:
      return TW_KEY_F3;
    case 63239:
      return TW_KEY_F4;
    case 63240:
      return TW_KEY_F5;
    case 63241:
      return TW_KEY_F6;
    case 63242:
      return TW_KEY_F7;
    case 63243:
      return TW_KEY_F8;
    case 63244:
      return TW_KEY_F9;
    case 63245:
      return TW_KEY_F10;
    case 63246:
      return TW_KEY_F11;
    case 63247:
      return TW_KEY_F12;
    case 63248:
      return TW_KEY_F13;
    case 63249:
      return TW_KEY_F14;
    case 63250:
      return TW_KEY_F15;
    default:
      break;
  }
  return key;
}
