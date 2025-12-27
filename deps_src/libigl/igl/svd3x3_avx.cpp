// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifdef __AVX__
#include "svd3x3_avx.h"

#include <cmath>
#include <algorithm>

#undef USE_SCALAR_IMPLEMENTATION
#undef USE_SSE_IMPLEMENTATION
#define USE_AVX_IMPLEMENTATION
#define COMPUTE_U_AS_MATRIX
#define COMPUTE_V_AS_MATRIX
#include "Singular_Value_Decomposition_Preamble.hpp"

#pragma runtime_checks( "u", off )  // disable runtime asserts on xor eax,eax type of stuff (doesn't always work, disable explicitly in compiler settings)
template<typename T>
IGL_INLINE void igl::svd3x3_avx(
  const Eigen::Matrix<T, 3*8, 3>& A,
  Eigen::Matrix<T, 3*8, 3> &U,
  Eigen::Matrix<T, 3*8, 1> &S,
  Eigen::Matrix<T, 3*8, 3>&V)
{
  // this code assumes USE_AVX_IMPLEMENTATION is defined
  float Ashuffle[9][8], Ushuffle[9][8], Vshuffle[9][8], Sshuffle[3][8];
  for (int i=0; i<3; i++)
  {
    for (int j=0; j<3; j++)
    {
      for (int k=0; k<8; k++)
      {
        Ashuffle[i + j*3][k] = A(i + 3*k, j);
      }
    }
  }

#include "Singular_Value_Decomposition_Kernel_Declarations.hpp"

  ENABLE_AVX_IMPLEMENTATION(Va11=_mm256_loadu_ps(Ashuffle[0]);)
  ENABLE_AVX_IMPLEMENTATION(Va21=_mm256_loadu_ps(Ashuffle[1]);)
  ENABLE_AVX_IMPLEMENTATION(Va31=_mm256_loadu_ps(Ashuffle[2]);)
  ENABLE_AVX_IMPLEMENTATION(Va12=_mm256_loadu_ps(Ashuffle[3]);)
  ENABLE_AVX_IMPLEMENTATION(Va22=_mm256_loadu_ps(Ashuffle[4]);)
  ENABLE_AVX_IMPLEMENTATION(Va32=_mm256_loadu_ps(Ashuffle[5]);)
  ENABLE_AVX_IMPLEMENTATION(Va13=_mm256_loadu_ps(Ashuffle[6]);)
  ENABLE_AVX_IMPLEMENTATION(Va23=_mm256_loadu_ps(Ashuffle[7]);)
  ENABLE_AVX_IMPLEMENTATION(Va33=_mm256_loadu_ps(Ashuffle[8]);)

#include "Singular_Value_Decomposition_Main_Kernel_Body.hpp"

  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[0],Vu11);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[1],Vu21);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[2],Vu31);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[3],Vu12);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[4],Vu22);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[5],Vu32);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[6],Vu13);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[7],Vu23);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Ushuffle[8],Vu33);)

  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[0],Vv11);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[1],Vv21);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[2],Vv31);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[3],Vv12);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[4],Vv22);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[5],Vv32);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[6],Vv13);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[7],Vv23);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Vshuffle[8],Vv33);)

  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Sshuffle[0],Va11);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Sshuffle[1],Va22);)
  ENABLE_AVX_IMPLEMENTATION(_mm256_storeu_ps(Sshuffle[2],Va33);)

  for (int i=0; i<3; i++)
  {
    for (int j=0; j<3; j++)
    {
      for (int k=0; k<8; k++)
      {
        U(i + 3*k, j) = Ushuffle[i + j*3][k];
        V(i + 3*k, j) = Vshuffle[i + j*3][k];
      }
    }
  }

  for (int i=0; i<3; i++)
  {
    for (int k=0; k<8; k++)
    {
      S(i + 3*k, 0) = Sshuffle[i][k];
    }
  }
}
#pragma runtime_checks( "u", restore )

#ifdef IGL_STATIC_LIBRARY
// forced instantiation
//template void igl::svd3x3_avx(const Eigen::Matrix<float, 3*8, 3>& A, Eigen::Matrix<float, 3*8, 3> &U, Eigen::Matrix<float, 3*8, 1> &S, Eigen::Matrix<float, 3*8, 3>&V);
// doesn't even make sense with double because the wunder-SVD code is only single precision anyway...
template void igl::svd3x3_avx<float>(Eigen::Matrix<float, 24, 3, 0, 24, 3> const&, Eigen::Matrix<float, 24, 3, 0, 24, 3>&, Eigen::Matrix<float, 24, 1, 0, 24, 1>&, Eigen::Matrix<float, 24, 3, 0, 24, 3>&);
#endif
#endif
