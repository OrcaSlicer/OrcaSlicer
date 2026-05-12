// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VIEWPORT_H
#define IGL_VIEWPORT_H

namespace igl
{
  /// @private
  // Simple Viewport class for an opengl context. Handles reshaping and mouse.
  struct Viewport
  {
    int x,y,width,height;
    // Constructors
    Viewport(
      const int x=0, 
      const int y=0, 
      const int width=0,
      const int height=0):
      x(x),
      y(y),
      width(width),
      height(height)
    {
    };
    virtual ~Viewport(){}
    void reshape(
      const int x, 
      const int y, 
      const int width,
      const int height)
    {
      this->x = x;
      this->y = y;
      this->width = width;
      this->height = height;
    };
    // Given mouse_x,mouse_y on the entire window return mouse_x, mouse_y in
    // this viewport.
    //
    // Inputs:
    //   my  mouse y-coordinate
    //   wh  window height
    // Returns y-coordinate in viewport
    int mouse_y(const int my,const int wh)
    {
      return my - (wh - height - y);
    }
    // Inputs:
    //   mx  mouse x-coordinate
    // Returns x-coordinate in viewport
    int mouse_x(const int mx)
    {
      return mx - x;
    }
    // Returns whether point (mx,my) is in extend of Viewport
    bool inside(const int mx, const int my) const
    {
      return 
        mx >= x && my >= y && 
        mx < x+width && my < y+height;
    }
  };
}

#endif
