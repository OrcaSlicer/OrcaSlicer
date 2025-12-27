//#####################################################################
// Copyright (c) 2010-2011, Eftychios Sifakis.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or
//     other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
// BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
// SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//#####################################################################

#ifdef PRINT_DEBUGGING_OUTPUT
#include <iomanip>
#include <iostream>
#endif

// Prevent warnings
#ifdef ENABLE_SCALAR_IMPLEMENTATION
#  undef ENABLE_SCALAR_IMPLEMENTATION
#endif
#ifdef ENABLE_SSE_IMPLEMENTATION
#  undef ENABLE_SSE_IMPLEMENTATION
#endif
#ifdef ENABLE_AVX_IMPLEMENTATION
#  undef ENABLE_AVX_IMPLEMENTATION
#endif

#ifdef USE_SCALAR_IMPLEMENTATION
#define ENABLE_SCALAR_IMPLEMENTATION(X) X
#else
#define ENABLE_SCALAR_IMPLEMENTATION(X)
#endif

#ifdef USE_SSE_IMPLEMENTATION
#define ENABLE_SSE_IMPLEMENTATION(X) X
#else
#define ENABLE_SSE_IMPLEMENTATION(X)
#endif

#ifdef USE_AVX_IMPLEMENTATION
#include <immintrin.h>
#define ENABLE_AVX_IMPLEMENTATION(X) X
#else
// Stefan: removed include. Why does it import MMX instructions, shouldn't this be under the #ifdef USE_SSE_IMPLEMENTATION above?
//#include <xmmintrin.h>
#define ENABLE_AVX_IMPLEMENTATION(X)
#endif

#ifdef USE_SCALAR_IMPLEMENTATION
// Alec: Why is this using sse intrinsics if it's supposed to be the scalar
// implementation?
#ifdef __SSE__
#include <mmintrin.h>
// Changed to inline
inline float rsqrt(const float f)
{
    float buf[4];
    buf[0]=f;
    __m128 v=_mm_loadu_ps(buf);
    v=_mm_rsqrt_ss(v);
    _mm_storeu_ps(buf,v);
    return buf[0];
}
#else
#include <cmath>
inline float rsqrt(const float f)
{
  return 1./sqrtf(f);
}
#endif
#endif


