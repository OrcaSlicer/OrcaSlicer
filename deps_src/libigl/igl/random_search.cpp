#include "random_search.h"
#include <iostream>
#include <cassert>

template <
  typename Scalar, 
  typename DerivedX, 
  typename DerivedLB, 
  typename DerivedUB>
IGL_INLINE Scalar igl::random_search(
  const std::function< Scalar (DerivedX &) > f,
  const Eigen::MatrixBase<DerivedLB> & LB,
  const Eigen::MatrixBase<DerivedUB> & UB,
  const int iters,
  DerivedX & X)
{
  Scalar min_f = std::numeric_limits<Scalar>::max();
  const int dim = LB.size();
  assert(UB.size() == dim && "UB should match LB size");
  for(int iter = 0;iter<iters;iter++)
  {
    const DerivedX R = DerivedX::Random(dim).array()*0.5+0.5;
    DerivedX Xr = LB.array() + R.array()*(UB-LB).array();
    const Scalar fr = f(Xr);
    if(fr<min_f)
    {
      min_f = fr;
      X = Xr;
    }
  }
  return min_f;
}

#ifdef IGL_STATIC_LIBRARY
template float igl::random_search<float, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1> >(std::function<float (Eigen::Matrix<float, 1, -1, 1, 1, -1>&)>, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, int, Eigen::Matrix<float, 1, -1, 1, 1, -1>&);
#endif
