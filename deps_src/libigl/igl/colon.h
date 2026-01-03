// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLON_H
#define IGL_COLON_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{

  /// Colon operator like matlab's colon operator. Enumerates values between low
  /// and hi with step step.
  ///
  /// @tparam L  should be a eigen matrix primitive type like int or double
  /// @tparam S  should be a eigen matrix primitive type like int or double
  /// @tparam H  should be a eigen matrix primitive type like int or double
  /// @tparam T  should be a eigen matrix primitive type like int or double
  /// @param[in] low  starting value if step is valid then this is *always* the first
  ///     element of I
  /// @param[in] step  step difference between sequential elements returned in I,
  ///     remember this will be cast to template T at compile time. If low<hi
  ///     then step must be positive. If low>hi then step must be negative.
  ///     Otherwise I will be set to empty.
  /// @param[in] hi  ending value, if (hi-low)%step is zero then this will be the last
  ///     element in I. If step is positive there will be no elements greater
  ///     than hi, vice versa if hi<low
  /// @param[out] I  list of values from low to hi with step size step
  ///
  /// \note
  /// This should be potentially replaced with eigen's LinSpaced() function
  ///
  /// If step = 1, it's about 5 times faster to use:
  ///     X = Eigen::VectorXi::LinSpaced(n,0,n-1);
  /// than 
  ///     X = igl::colon<int>(0,n-1);
  ///
  template <typename L,typename S,typename H,typename T>
  IGL_INLINE void colon(
    const L low, 
    const S step, 
    const H hi, 
    Eigen::Matrix<T,Eigen::Dynamic,1> & I);
  /// Colon operator like matlab's colon operator. Enumerates values between low
  /// and hi with unit step.
  ///
  /// @tparam L  should be a eigen matrix primitive type like int or double
  /// @tparam H  should be a eigen matrix primitive type like int or double
  /// @tparam T  should be a eigen matrix primitive type like int or double
  /// @param[in] low  starting value if step is valid then this is *always* the first
  ///     element of I
  /// @param[in] step  step difference between sequential elements returned in I,
  ///     remember this will be cast to template T at compile time. If low<hi
  ///     then step must be positive. If low>hi then step must be negative.
  ///     Otherwise I will be set to empty.
  /// @param[in] hi  ending value, if (hi-low)%step is zero then this will be the last
  ///     element in I. If step is positive there will be no elements greater
  ///     than hi, vice versa if hi<low
  /// @param[out] I  list of values from low to hi with step size step
  template <typename L,typename H,typename T>
  IGL_INLINE void colon(
    const L low, 
    const H hi, 
    Eigen::Matrix<T,Eigen::Dynamic,1> & I);
  /// @private
  ///
  /// Hiding this from doxygen because it's messing up the indentation.
  ///
  /// Colon operator like matlab's colon operator. Enumerates values between low
  /// and hi with unit step.
  ///
  /// @tparam T  should be a eigen matrix primitive type like int or double
  /// @tparam L  should be a eigen matrix primitive type like int or double
  /// @tparam H  should be a eigen matrix primitive type like int or double
  /// @param[in] low  starting value if step is valid then this is *always* the first
  ///     element of I
  /// @param[in] step  step difference between sequential elements returned in I,
  ///     remember this will be cast to template T at compile time. If low<hi
  ///     then step must be positive. If low>hi then step must be negative.
  ///     Otherwise I will be set to empty.
  /// @param[in] hi  ending value, if (hi-low)%step is zero then this will be the last
  ///     element in I. If step is positive there will be no elements greater
  ///     than hi, vice versa if hi<low
  /// @return list of values from low to hi with step size step
  template <typename T,typename L,typename H>
  IGL_INLINE Eigen::Matrix<T,Eigen::Dynamic,1> colon(
    const L low, 
    const H hi);
}

#ifndef IGL_STATIC_LIBRARY
#  include "colon.cpp"
#endif

#endif
