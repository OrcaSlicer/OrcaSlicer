#include "intersect_with_half_space.h"
#include "mesh_boolean.h"
#include "half_space_box.h"

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedp,
  typename Derivedn,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::intersect_with_half_space(
  const Eigen::MatrixBase<DerivedV > & V,
  const Eigen::MatrixBase<DerivedF > & F,
  const Eigen::MatrixBase<Derivedp > & p,
  const Eigen::MatrixBase<Derivedn > & n,
  Eigen::PlainObjectBase<DerivedVC > & VC,
  Eigen::PlainObjectBase<DerivedFC > & FC,
  Eigen::PlainObjectBase<DerivedJ > & J)
{
  typedef CGAL::Plane_3<CGAL::Epeck> Plane;
  typedef CGAL::Point_3<CGAL::Epeck> Point;
  typedef CGAL::Vector_3<CGAL::Epeck> Vector;
  Plane P(Point(p(0),p(1),p(2)),Vector(n(0),n(1),n(2)));
  return intersect_with_half_space(V,F,P,VC,FC,J);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename Derivedequ,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::intersect_with_half_space(
  const Eigen::MatrixBase<DerivedV > & V,
  const Eigen::MatrixBase<DerivedF > & F,
  const Eigen::MatrixBase<Derivedequ > & equ,
  Eigen::PlainObjectBase<DerivedVC > & VC,
  Eigen::PlainObjectBase<DerivedFC > & FC,
  Eigen::PlainObjectBase<DerivedJ > & J)
{
  typedef CGAL::Plane_3<CGAL::Epeck> Plane;
  Plane P(equ(0),equ(1),equ(2),equ(3));
  return intersect_with_half_space(V,F,P,VC,FC,J);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::intersect_with_half_space(
  const Eigen::MatrixBase<DerivedV > & V,
  const Eigen::MatrixBase<DerivedF > & F,
  const CGAL::Plane_3<CGAL::Epeck> & P,
  Eigen::PlainObjectBase<DerivedVC > & VC,
  Eigen::PlainObjectBase<DerivedFC > & FC,
  Eigen::PlainObjectBase<DerivedJ > & J)
{
  Eigen::Matrix<CGAL::Epeck::FT,8,3> BV;
  Eigen::Matrix<typename DerivedF::Scalar,12,3> BF;
  half_space_box(P,V,BV,BF);
  // Disturbingly, (BV,BF) must be first argument
  const bool ret = mesh_boolean(BV,BF,V,F,MESH_BOOLEAN_TYPE_INTERSECT,VC,FC,J);
  // But now J is wrong...
  std::for_each(
    J.data(),
    J.data()+J.size(),
    [&BF,&F](typename DerivedJ::Scalar & j)
      {j = (j<BF.rows()?F.rows()+j:j-BF.rows());}
    );
  return ret;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::copyleft::cgal::intersect_with_half_space<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::intersect_with_half_space<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::CwiseUnaryOp<Eigen::internal::scalar_opposite_op<double>, Eigen::Matrix<double, 1, 3, 1, 1, 3> const>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::CwiseUnaryOp<Eigen::internal::scalar_opposite_op<double>, Eigen::Matrix<double, 1, 3, 1, 1, 3> const> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::intersect_with_half_space<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::intersect_with_half_space<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::CwiseUnaryOp<Eigen::internal::scalar_opposite_op<float>, Eigen::Matrix<float, 1, 3, 1, 1, 3> const>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::CwiseUnaryOp<Eigen::internal::scalar_opposite_op<float>, Eigen::Matrix<float, 1, 3, 1, 1, 3> const> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
