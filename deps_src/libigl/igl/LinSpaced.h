#ifndef IGL_LINSPACED_H
#define IGL_LINSPACED_H
#include <Eigen/Core>
// This function is not intended to be a permanent function of libigl. Rather
// it is a "drop-in" workaround for documented bug in Eigen:
// http://eigen.tuxfamily.org/bz/show_bug.cgi?id=1383
//
// Replace: 
//
//     Eigen::VectorXi::LinSpaced(size,low,high);
//
// With:
//
//     igl::LinSpaced<Eigen::VectorXi>(size,low,high);
//
// Specifcally, this version will _always_ return an empty vector if size==0,
// regardless of the values for low and high. If size != 0, then this simply
// returns the result of Eigen::Derived::LinSpaced.
//
// Until this bug is fixed, we should also avoid calls to the member function
// `.setLinSpaced`. This means replacing:
//
//     a.setLinSpaced(size,low,high);
//
// with
//
//     a = igl::LinSpaced<decltype(a) >(size,low,high);
//
namespace igl
{
  template <typename Derived>
  //inline typename Eigen::DenseBase< Derived >::RandomAccessLinSpacedReturnType 
  inline Derived LinSpaced(
    typename Derived::Index size,
    const typename Derived::Scalar & low,
    const typename Derived::Scalar & high);
}

// Implementation

template <typename Derived>
//inline typename Eigen::DenseBase< Derived >::RandomAccessLinSpacedReturnType 
inline Derived
igl::LinSpaced(
  typename Derived::Index size,
  const typename Derived::Scalar & low,
  const typename Derived::Scalar & high)
{
  if(size == 0)
  {
    // Force empty vector with correct "RandomAccessLinSpacedReturnType" type.
    return Derived::LinSpaced(0,0,1);
  }else if(high < low)
  {
    return low-Derived::LinSpaced(size,low-low,low-high).array();
  }else{
    return Derived::LinSpaced(size,low,high);
  }
}

#endif
