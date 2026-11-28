#include "isolines_map.h"
#include <iostream>

template <
  typename DerivedCM,
  typename Derivediso_color,
  typename DerivedICM
  >
IGL_INLINE void igl::isolines_map(
  const Eigen::MatrixBase<DerivedCM> & CM, 
  const Eigen::MatrixBase<Derivediso_color> & iso_color,
  const int interval_thickness,
  const int iso_thickness,
  Eigen::PlainObjectBase<DerivedICM> & ICM)
{
  ICM.resize(CM.rows()*interval_thickness+(CM.rows()-1)*iso_thickness,3);
  {
    int k = 0;
    for(int c = 0;c<CM.rows();c++)
    {
      for(int i = 0;i<interval_thickness;i++)
      {
        ICM.row(k++) = CM.row(c);
      }
      if(c+1 != CM.rows())
      {
        for(int i = 0;i<iso_thickness;i++)
        {
          ICM.row(k++) = iso_color;
        }
      }
    }
    assert(k == ICM.rows());
  }
}

template <
  typename DerivedCM,
  typename DerivedICM
  >
IGL_INLINE void igl::isolines_map(
  const Eigen::MatrixBase<DerivedCM> & CM, 
  Eigen::PlainObjectBase<DerivedICM> & ICM)
{
  return isolines_map(
    CM, Eigen::Matrix<typename DerivedCM::Scalar,1,3>(0,0,0), 10, 1, ICM);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::isolines_map<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
