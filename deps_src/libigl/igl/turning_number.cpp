#include "turning_number.h"
#include "PI.h"


template <typename DerivedV>
IGL_INLINE typename DerivedV::Scalar igl::turning_number(
  const Eigen::MatrixBase<DerivedV> & V)
{
  typedef typename DerivedV::Scalar Scalar;
  constexpr Scalar TWO_PI = 2.0 * igl::PI;
    
  const int n = V.rows();
  Scalar total_angle = 0.0;

  for (int i = 0; i < n; i++)
  {
    Eigen::Matrix<Scalar, 1, 2> current = V.row(i).head(2);
    Eigen::Matrix<Scalar, 1, 2> next = V.row((i + 1) % n).head(2);
    
    Eigen::Matrix<Scalar, 1, 2> d1 = next - current;
    Eigen::Matrix<Scalar, 1, 2> d2 = V.row((i + 2) % n).head(2) - next;

    const Scalar angle = atan2(
      d1(0)*d2(1) - d1(1)*d2(0), 
      d1(0)*d2(0) + d1(1)*d2(1));
    total_angle += angle;
  }

  return total_angle / TWO_PI;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template specialization
template double igl::turning_number<Eigen::Matrix<double, -1, 2, 0, -1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 2, 0, -1, 2> > const&);
template double igl::turning_number<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&);
#endif
