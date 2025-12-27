#ifndef IGL_COPYLEFT_CGAL_INTERSECT_WITH_HALF_SPACE_H
#define IGL_COPYLEFT_CGAL_INTERSECT_WITH_HALF_SPACE_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Plane_3.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Intersect a PWN mesh with a half-space. Point on plane, normal pointing
      // outward.
      //
      // Inputs:
      //   V  #V by 3 list of mesh vertex positions
      //   p  3d point on plane
      //   n  3d vector of normal of plane pointing away from inside
      // Outputs:
      //   VC  #VC by 3 list of vertex positions of boolean result mesh
      //   FC  #FC by 3 list of triangle indices into VC
      //   J  #FC list of indices into [F;F.rows()+[1;2]] revealing "birth"
      //     facet
      template <
        typename DerivedV,
        typename DerivedF,
        typename Derivedp,
        typename Derivedn,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool intersect_with_half_space(
        const Eigen::MatrixBase<DerivedV > & V,
        const Eigen::MatrixBase<DerivedF > & F,
        const Eigen::MatrixBase<Derivedp > & p,
        const Eigen::MatrixBase<Derivedn > & n,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC,
        Eigen::PlainObjectBase<DerivedJ > & J);
      // Intersect a PWN mesh with a half-space. Plane equation.
      //
      // Inputs:
      //   V  #V by 3 list of mesh vertex positions
      //   equ  plane equation: P(x,y,z) = a*x+b*y+c*z + d = 0, P(x,y,z) < 0 is
      //     _inside_.
      // Outputs:
      //   VC  #VC by 3 list of vertex positions of boolean result mesh
      //   FC  #FC by 3 list of triangle indices into VC
      //   J  #FC list of indices into [F;F.rows()+[1;2]] revealing "birth" facet
      template <
        typename DerivedV,
        typename DerivedF,
        typename Derivedequ,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool intersect_with_half_space(
        const Eigen::MatrixBase<DerivedV > & V,
        const Eigen::MatrixBase<DerivedF > & F,
        const Eigen::MatrixBase<Derivedequ > & equ,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC,
        Eigen::PlainObjectBase<DerivedJ > & J);
      // Intersect a PWN mesh with a half-space. CGAL Plane.
      //
      // Inputs:
      //   V  #V by 3 list of mesh vertex positions
      //   P  plane 
      // Outputs:
      //   VC  #VC by 3 list of vertex positions of boolean result mesh
      //   FC  #FC by 3 list of triangle indices into VC
      //   J  #FC list of indices into [F;F.rows()+[1;2]] revealing "birth" facet
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedVC,
        typename DerivedFC,
        typename DerivedJ>
      IGL_INLINE bool intersect_with_half_space(
        const Eigen::MatrixBase<DerivedV > & V,
        const Eigen::MatrixBase<DerivedF > & F,
        const CGAL::Plane_3<CGAL::Epeck> & P,
        Eigen::PlainObjectBase<DerivedVC > & VC,
        Eigen::PlainObjectBase<DerivedFC > & FC,
        Eigen::PlainObjectBase<DerivedJ > & J);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "intersect_with_half_space.cpp"
#endif

#endif
