#include "random_points_on_mesh_intrinsic.h"
#include "cumsum.h"
#include "histc.h"
#include <cassert>
#include <random>

template <
  typename DeriveddblA,
  typename DerivedB, 
  typename DerivedFI,
  typename URBG>
IGL_INLINE void igl::random_points_on_mesh_intrinsic(
  const int n,
  const Eigen::MatrixBase<DeriveddblA > & dblA,
  Eigen::PlainObjectBase<DerivedB > & B,
  Eigen::PlainObjectBase<DerivedFI > & FI,
  URBG && urbg)
{
  using namespace Eigen;
  using namespace std;
  typedef typename DeriveddblA::Scalar Scalar;
  typedef Matrix<Scalar,Dynamic,1> VectorXs;
  VectorXs C;
  VectorXs A0(dblA.size()+1);
  A0(0) = 0;
  A0.bottomRightCorner(dblA.size(),1) = dblA;
  // Even faster would be to use the "Alias Table Method"
  cumsum(A0,1,C);
  const Scalar Cmax = C(C.size()-1);
  assert(Cmax > 0 && "Total surface area should be positive");
  // Why is this more accurate than `C /= C(C.size()-1)` ?
  for(int i = 0;i<C.size();i++) { C(i) = C(i)/Cmax; }
  std::uniform_real_distribution<Scalar> dis(-1.0, 1.0);
  const VectorXs R = (VectorXs::NullaryExpr(n,1,[&](){return dis(urbg);}).array() + 1.)/2.;
  assert(R.minCoeff() >= 0);
  assert(R.maxCoeff() <= 1);
  histc(R,C,FI);
  // fix the bin when R(i) == 1 exactly
  // Gross cast to deal with Windows 
  FI = FI.array().min(static_cast<typename DerivedFI::Scalar>(dblA.rows() - 1));
  const VectorXs S = (VectorXs::NullaryExpr(n,1,[&](){return dis(urbg);}).array() + 1.)/2.;
  const VectorXs T = (VectorXs::NullaryExpr(n,1,[&](){return dis(urbg);}).array() + 1.)/2.;
  B.resize(n,3);
  B.col(0) = 1.-T.array().sqrt();
  B.col(1) = (1.-S.array()) * T.array().sqrt();
  B.col(2) = S.array() * T.array().sqrt();
}

template <
  typename DeriveddblA,
  typename DerivedF,
  typename ScalarB, 
  typename DerivedFI,
  typename URBG>
IGL_INLINE void igl::random_points_on_mesh_intrinsic(
  const int n,
  const Eigen::MatrixBase<DeriveddblA > & dblA,
  const int num_vertices,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::SparseMatrix<ScalarB > & B,
  Eigen::PlainObjectBase<DerivedFI > & FI,
  URBG && urbg)
{
  using namespace Eigen;
  using namespace std;
  Matrix<ScalarB,Dynamic,3> BC;
  // Should be traingle mesh. Although Turk's method 1 generalizes...
  assert(F.cols() == 3);
  random_points_on_mesh_intrinsic(n,dblA,BC,FI,urbg);
  vector<Triplet<ScalarB> > BIJV;
  BIJV.reserve(n*3);
  for(int s = 0;s<n;s++)
  {
    for(int c = 0;c<3;c++)
    {
      assert(FI(s) < dblA.rows());
      assert(FI(s) >= 0);
      const int v = F(FI(s),c);
      BIJV.push_back(Triplet<ScalarB>(s,v,BC(s,c)));
    }
  }
  B.resize(n,num_vertices);
  B.reserve(n*3);
  B.setFromTriplets(BIJV.begin(),BIJV.end());
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::minstd_rand&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::minstd_rand&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::minstd_rand0&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::minstd_rand0&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::mt19937&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::mt19937&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::mt19937_64&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::mt19937_64&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::mt19937&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::mt19937&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::mt19937&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::mt19937&);
template void igl::random_points_on_mesh_intrinsic<Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::mt19937_64&>(int, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, std::mt19937_64&);
#endif
