#ifndef IGL_SCREEN_SPACE_SELECTION_H
#define IGL_SCREEN_SPACE_SELECTION_H

#include "igl/igl_inline.h"
#include <Eigen/Core>
#include <vector>
// Forward declaration
namespace igl { template <typename DerivedV, int DIM> class AABB; }

namespace igl
{
  /// Given a mesh, a camera  determine which points are inside of a given 2D
  /// screen space polygon **culling points based on self-occlusion.**
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices into rows of V
  /// @param[in] tree  precomputed bounding volume heirarchy
  /// @param[in] model  4 by 4 camera model-view matrix
  /// @param[in] proj  4 by 4 camera projection matrix (perspective or orthoraphic)
  /// @param[in] viewport  4-vector containing camera viewport
  /// @param[in] L  #L by 2 list of 2D polygon vertices (in order)
  /// @param[out] W  #V by 1 list of winding numbers (|W|>0.5 indicates inside)
  /// @param[out] and_visible  #V by 1 list of visibility values (only correct for vertices
  ///     with |W|>0.5)
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedM,
    typename DerivedN,
    typename DerivedO,
    typename Ltype,
    typename DerivedW,
    typename Deriveda>
  IGL_INLINE void screen_space_selection(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const igl::AABB<DerivedV, 3> & tree,
    const Eigen::MatrixBase<DerivedM>& model,
    const Eigen::MatrixBase<DerivedN>& proj,
    const Eigen::MatrixBase<DerivedO>& viewport,
    const std::vector<Eigen::Matrix<Ltype,1,2> > & L,
    Eigen::PlainObjectBase<DerivedW> & W,
    Eigen::PlainObjectBase<Deriveda> & and_visible);
  /// Given a mesh, a camera  determine which points are inside of a given 2D
  /// screen space polygon
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] model  4 by 4 camera model-view matrix
  /// @param[in] proj  4 by 4 camera projection matrix (perspective or orthoraphic)
  /// @param[in] viewport  4-vector containing camera viewport
  /// @param[in] L  #L by 2 list of 2D polygon vertices (in order)
  /// @param[out] W  #V by 1 list of winding numbers (|W|>0.5 indicates inside)
  template <
    typename DerivedV,
    typename DerivedM,
    typename DerivedN,
    typename DerivedO,
    typename Ltype,
    typename DerivedW>
  IGL_INLINE void screen_space_selection(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedM>& model,
    const Eigen::MatrixBase<DerivedN>& proj,
    const Eigen::MatrixBase<DerivedO>& viewport,
    const std::vector<Eigen::Matrix<Ltype,1,2> > & L,
    Eigen::PlainObjectBase<DerivedW> & W);
  /// Given a mesh, a camera  determine which points are inside of a given 2D
  /// screen space polygon
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] model  4 by 4 camera model-view matrix
  /// @param[in] proj  4 by 4 camera projection matrix (perspective or orthoraphic)
  /// @param[in] viewport  4-vector containing camera viewport
  /// @param[in] P  #P by 2 list of screen space polygon vertices
  /// @param[in] E  #E by 2 list of screen space edges as indices into rows of P
  /// @param[out] W  #V by 1 list of winding numbers (|W|>0.5 indicates inside)
  template <
    typename DerivedV,
    typename DerivedM,
    typename DerivedN,
    typename DerivedO,
    typename DerivedP,
    typename DerivedE,
    typename DerivedW>
  IGL_INLINE void screen_space_selection(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedM>& model,
    const Eigen::MatrixBase<DerivedN>& proj,
    const Eigen::MatrixBase<DerivedO>& viewport,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedE> & E,
    Eigen::PlainObjectBase<DerivedW> & W);
}

#ifndef IGL_STATIC_LIBRARY
#include "screen_space_selection.cpp"
#endif
  
#endif
