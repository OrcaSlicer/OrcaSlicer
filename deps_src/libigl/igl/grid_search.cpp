#include "grid_search.h"
#include <iostream>
#include <cassert>

template <
  typename Scalar, 
  typename DerivedX, 
  typename DerivedLB, 
  typename DerivedUB, 
  typename DerivedI>
IGL_INLINE Scalar igl::grid_search(
  const std::function< Scalar (DerivedX &) > f,
  const Eigen::MatrixBase<DerivedLB> & LB,
  const Eigen::MatrixBase<DerivedUB> & UB,
  const Eigen::MatrixBase<DerivedI> & I,
  DerivedX & X)
{
  Scalar fval = std::numeric_limits<Scalar>::max();
  const int dim = LB.size();
  assert(UB.size() == dim && "UB should match LB size");
  assert(I.size() == dim && "I should match LB size");
  X.resize(dim);

  // Working X value
  DerivedX Xrun(dim);
  std::function<void(const int, DerivedX &)> looper;
  int calls = 0;
  looper = [&](
    const int d,
    DerivedX & Xrun)
  {
    assert(d < dim);
    Eigen::Matrix<Scalar,Eigen::Dynamic,1> vals = 
      Eigen::Matrix<Scalar,Eigen::Dynamic,1>::LinSpaced(I(d),LB(d),UB(d));
    for(int c = 0;c<I(d);c++)
    {
      Xrun(d) = vals(c);
      if(d+1 < dim)
      {
        looper(d+1,Xrun);
      }else
      {
        //std::cout<<"call: "<<calls<<std::endl;
        // Base case
        const Scalar val = f(Xrun);
        calls++;
        if(val < fval)
        {
          fval = val;
          X = Xrun;
          std::cout<<calls<<": "<<fval<<" | "<<X<<std::endl;
        }
      }
    }
  };
  looper(0,Xrun);

  return fval;
}

#ifdef IGL_STATIC_LIBRARY
template double igl::grid_search<double, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<int, 1, 3, 1, 1, 3> >(std::function<double (Eigen::Matrix<double, 1, 3, 1, 1, 3>&)>, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 1, 3, 1, 1, 3> > const&, Eigen::Matrix<double, 1, 3, 1, 1, 3>&);
template float igl::grid_search<float, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<int, 1, -1, 1, 1, -1> >(std::function<float (Eigen::Matrix<float, 1, -1, 1, 1, -1>&)>, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 1, -1, 1, 1, -1> > const&, Eigen::Matrix<float, 1, -1, 1, 1, -1>&);
#endif
