#ifndef IGL_BOX_FACES_H
#define IGL_BOX_FACES_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace igl
{
  /// Compute the quad faces of an axis-aligned bounding box) shrunk by a given
  /// factor.
  ///
  /// @param[in] box Axis-aligned bounding box
  /// @param[in] shrink Factor by which to shrink the box
  /// @param[out] P  #P by 3 list of vertex positions
  /// @param[out] Q  #Q by 4 list of triangle indices into rows of P
  template <typename DerivedV, typename DerivedQ>
  IGL_INLINE void box_faces(
    const Eigen::AlignedBox<typename DerivedV::Scalar,3> & box,
    const typename DerivedV::Scalar shrink,
    Eigen::PlainObjectBase<DerivedV> & P,
    Eigen::PlainObjectBase<DerivedQ> & Q);
  // Forward declaration
  template <typename DerivedV, int DIM> class AABB;
  /// Compute the quad faces of a tree of axis-aligned bounding boxes.
  ///
  /// @param[in] tree Tree of axis-aligned bounding boxes
  /// @param[out] P  #P by 3 list of vertex positions
  /// @param[out] Q  #Q by 4 list of triangle indices into rows of P
  /// @param[out] D  #Q list of tree depths (0==root)
  template <
    typename DerivedV,
    typename DerivedP,
    typename DerivedQ,
    typename DerivedD >
  IGL_INLINE void box_faces(
    const igl::AABB<DerivedV,3> & tree,
    Eigen::PlainObjectBase<DerivedP> & P,
    Eigen::PlainObjectBase<DerivedQ> & Q,
    Eigen::PlainObjectBase<DerivedD> & D);
}

#ifndef IGL_STATIC_LIBRARY
#  include "box_faces.cpp"
#endif

#endif
