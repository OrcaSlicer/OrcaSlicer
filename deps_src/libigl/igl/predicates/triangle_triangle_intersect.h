#ifndef IGL_PREDICATES_TRIANGLE_TRIANGLE_INTERSECT_H
#define IGL_PREDICATES_TRIANGLE_TRIANGLE_INTERSECT_H
#include "../igl_inline.h"

namespace igl
{
  namespace predicates
  {
    template <typename Vector3D>
    IGL_INLINE  bool triangle_triangle_intersect(
      const Vector3D & a1,
      const Vector3D & a2,
      const Vector3D & a3,
      const Vector3D & b1,
      const Vector3D & b2,
      const Vector3D & b3,
      bool & coplanar);
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "triangle_triangle_intersect.cpp"
#endif

#endif 
