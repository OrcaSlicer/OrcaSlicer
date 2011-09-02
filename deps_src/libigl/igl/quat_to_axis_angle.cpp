// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "quat_to_axis_angle.h"
#include "EPS.h"
#include "PI.h"
#include <cmath>
#include <cstdio>
//
// http://www.antisphere.com/Wiki/tools:anttweakbar
template <typename Q_type>
IGL_INLINE void igl::quat_to_axis_angle(
  const Q_type *q,
  Q_type *axis, 
  Q_type & angle)
{
    if( fabs(q[3])>(1.0 + igl::EPS<Q_type>()) )
    {
        //axis[0] = axis[1] = axis[2] = 0; // no, keep the previous value
        angle = 0;
    }
    else
    {
        double a;
        if( q[3]>=1.0f )
            a = 0; // and keep V
        else if( q[3]<=-1.0f )
            a = PI; // and keep V
        else if( fabs(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3])<igl::EPS_SQ<Q_type>())
        {
            a = 0;
        }else
        {
            a = acos(q[3]);
            if( a*angle<0 ) // Preserve the sign of angle
                a = -a;
            double f = 1.0f / sin(a);
            axis[0] = q[0] * f;
            axis[1] = q[1] * f;
            axis[2] = q[2] * f;
        }
        angle = 2.0*a;
    }

    //  if( angle>FLOAT_PI )
    //      angle -= 2.0f*FLOAT_PI;
    //  else if( angle<-FLOAT_PI )
    //      angle += 2.0f*FLOAT_PI;
    //angle = RadToDeg(angle);

    if( fabs(angle)<igl::EPS<Q_type>()&& fabs(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2])<igl::EPS_SQ<Q_type>())
    {
        axis[0] = 1.0e-7;    // all components cannot be null
    }
}

template <typename Q_type>
IGL_INLINE void igl::quat_to_axis_angle_deg(
  const Q_type *q,
  Q_type *axis, 
  Q_type & angle)
{
  igl::quat_to_axis_angle(q,axis,angle);
  angle = angle*(180.0/PI);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::quat_to_axis_angle<float>(float const*, float*, float&);
template void igl::quat_to_axis_angle_deg<float>(float const*, float*, float&);
#endif
