// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "line_segment_in_rectangle.h"

IGL_INLINE bool igl::line_segment_in_rectangle(
  const Eigen::Vector2d & s,
  const Eigen::Vector2d & d,
  const Eigen::Vector2d & A,
  const Eigen::Vector2d & B)
{
  using namespace std;
  using namespace Eigen;
  // http://stackoverflow.com/a/100165/148668
  const auto SegmentIntersectRectangle = [](double a_rectangleMinX,
                                 double a_rectangleMinY,
                                 double a_rectangleMaxX,
                                 double a_rectangleMaxY,
                                 double a_p1x,
                                 double a_p1y,
                                 double a_p2x,
                                 double a_p2y)->bool
  {
    // Find min and max X for the segment

    double minX = a_p1x;
    double maxX = a_p2x;

    if(a_p1x > a_p2x)
    {
      minX = a_p2x;
      maxX = a_p1x;
    }

    // Find the intersection of the segment's and rectangle's x-projections

    if(maxX > a_rectangleMaxX)
    {
      maxX = a_rectangleMaxX;
    }

    if(minX < a_rectangleMinX)
    {
      minX = a_rectangleMinX;
    }

    if(minX > maxX) // If their projections do not intersect return false
    {
      return false;
    }

    // Find corresponding min and max Y for min and max X we found before

    double minY = a_p1y;
    double maxY = a_p2y;

    double dx = a_p2x - a_p1x;

    if(fabs(dx) > 0.0000001)
    {
      double a = (a_p2y - a_p1y) / dx;
      double b = a_p1y - a * a_p1x;
      minY = a * minX + b;
      maxY = a * maxX + b;
    }

    if(minY > maxY)
    {
      double tmp = maxY;
      maxY = minY;
      minY = tmp;
    }

    // Find the intersection of the segment's and rectangle's y-projections

    if(maxY > a_rectangleMaxY)
    {
      maxY = a_rectangleMaxY;
    }

    if(minY < a_rectangleMinY)
    {
      minY = a_rectangleMinY;
    }

    if(minY > maxY) // If Y-projections do not intersect return false
    {
      return false;
    }

    return true;
  };
  const double minX = std::min(A(0),B(0));
  const double minY = std::min(A(1),B(1));
  const double maxX = std::max(A(0),B(0));
  const double maxY = std::max(A(1),B(1));
  bool ret = SegmentIntersectRectangle(minX,minY,maxX,maxY,s(0),s(1),d(0),d(1));
  return ret;
}
