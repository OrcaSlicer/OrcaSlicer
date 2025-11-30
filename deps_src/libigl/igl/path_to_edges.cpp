#include "path_to_edges.h"

template <typename DerivedI, typename DerivedE>
IGL_INLINE void igl::path_to_edges(
  const Eigen::MatrixBase<DerivedI> & I,
  Eigen::PlainObjectBase<DerivedE> & E,
  bool make_loop)
{
  // Check that I is 1 dimensional
  assert(I.size() == I.rows() || I.size() == I.cols());

  if(make_loop) {
    E.conservativeResize(I.size(), 2);
    for(int i = 0; i < I.size() - 1; i++) {
      E(i, 0) = I(i);
      E(i, 1) = I(i + 1);
    }
    E(I.size() - 1, 0) = I(I.size() - 1);
    E(I.size() - 1, 1) = I(0);
  } else {
    E.conservativeResize(I.size()-1, 2);
    for(int i = 0; i < I.size()-1; i++) {
      E(i, 0) = I(i);
      E(i, 1) = I(i+1);
    }
  }
}

template <typename Index, typename DerivedE>
IGL_INLINE void igl::path_to_edges(
  const std::vector<Index> & I,
  Eigen::PlainObjectBase<DerivedE> & E,
  bool make_loop)
{
  igl::path_to_edges(Eigen::Map<const Eigen::Matrix<Index, -1, 1>>(I.data(), I.size()), E, make_loop);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::path_to_edges<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, bool);
template void igl::path_to_edges<int, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::vector<int, std::allocator<int> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, bool);
#endif
  