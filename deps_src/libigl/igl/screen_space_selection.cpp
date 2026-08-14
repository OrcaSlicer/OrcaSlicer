#include "screen_space_selection.h"

#include "AABB.h"
#include "PlainMatrix.h"
#include "winding_number.h"
#include "project.h"
#include "unproject.h"
#include "Hit.h"
#include "parallel_for.h"

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedM,
  typename DerivedN,
  typename DerivedO,
  typename Ltype,
  typename DerivedW,
  typename Deriveda>
IGL_INLINE void igl::screen_space_selection(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const igl::AABB<DerivedV, 3> & tree,
  const Eigen::MatrixBase<DerivedM>& model,
  const Eigen::MatrixBase<DerivedN>& proj,
  const Eigen::MatrixBase<DerivedO>& viewport,
  const std::vector<Eigen::Matrix<Ltype,1,2> > & L,
  Eigen::PlainObjectBase<DerivedW> & W,
  Eigen::PlainObjectBase<Deriveda> & and_visible)
{
  typedef typename DerivedV::Scalar Scalar;
  screen_space_selection(V,model,proj,viewport,L,W);
  const Eigen::RowVector3d origin =
    (model.inverse().col(3)).head(3).template cast<Scalar>();
  igl::parallel_for(V.rows(),[&](const int i)
  {
    // Skip unselected points
    if(W(i)<0.5){ return; }
    igl::Hit<typename DerivedV::Scalar> hit;
    tree.intersect_ray(V,F,origin,V.row(i)-origin,hit);
    and_visible(i) = !(hit.t>1e-5 && hit.t<(1-1e-5));
  });
}

template <
  typename DerivedV,
  typename DerivedM,
  typename DerivedN,
  typename DerivedO,
  typename Ltype,
  typename DerivedW>
IGL_INLINE void igl::screen_space_selection(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedM>& model,
  const Eigen::MatrixBase<DerivedN>& proj,
  const Eigen::MatrixBase<DerivedO>& viewport,
  const std::vector<Eigen::Matrix<Ltype,1,2> > & L,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  typedef typename DerivedV::Scalar Scalar;
  Eigen::Matrix<Scalar,Eigen::Dynamic,2> P(L.size(),2);
  Eigen::Matrix<int,Eigen::Dynamic,2> E(L.size(),2);
  for(int i = 0;i<E.rows();i++)
  { 
    P.row(i) = L[i].template cast<Scalar>();
    E(i,0) = i; 
    E(i,1) = (i+1)%E.rows(); 
  }
  return screen_space_selection(V,model,proj,viewport,P,E,W);
}

template <
  typename DerivedV,
  typename DerivedM,
  typename DerivedN,
  typename DerivedO,
  typename DerivedP,
  typename DerivedE,
  typename DerivedW>
IGL_INLINE void igl::screen_space_selection(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedM>& model,
  const Eigen::MatrixBase<DerivedN>& proj,
  const Eigen::MatrixBase<DerivedO>& viewport,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedE> & E,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  // project all mesh vertices to 2D
  PlainMatrix<DerivedV,Eigen::Dynamic,Eigen::Dynamic> V2;
  igl::project(V,model,proj,viewport,V2);
  // In 2D this uses O(N*M) naive algorithm.
  igl::winding_number(P,E,V2,W);
  W = W.array().abs().eval();
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::screen_space_selection<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<float, 4, 4, 0, 4, 4>, Eigen::Matrix<float, 4, 4, 0, 4, 4>, Eigen::Matrix<float, 4, 1, 0, 4, 1>, float, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Array<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 4, 0, 4, 4> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 4, 0, 4, 4> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 1, 0, 4, 1> > const&, std::vector<Eigen::Matrix<float, 1, 2, 1, 1, 2>, std::allocator<Eigen::Matrix<float, 1, 2, 1, 1, 2> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Array<double, -1, 1, 0, -1, 1> >&);
template void igl::screen_space_selection<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<float, 4, 4, 0, 4, 4>, Eigen::Matrix<float, 4, 4, 0, 4, 4>, Eigen::Matrix<float, 4, 1, 0, 4, 1>, float, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 4, 0, 4, 4> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 4, 0, 4, 4> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 4, 1, 0, 4, 1> > const&, std::vector<Eigen::Matrix<float, 1, 2, 1, 1, 2>, std::allocator<Eigen::Matrix<float, 1, 2, 1, 1, 2> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
