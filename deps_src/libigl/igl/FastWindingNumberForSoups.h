// This header created by issuing: `echo "// This header created by issuing: \`$BASH_COMMAND\` $(echo "" | cat - LICENSE README.md | sed -e "s#^..*#\/\/ &#") $(echo "" | cat - SYS_Types.h SYS_Math.h VM_SSEFunc.h VM_SIMDFunc.h VM_SIMD.h UT_Array.h UT_ArrayImpl.h UT_SmallArray.h UT_FixedVector.h UT_ParallelUtil.h UT_BVH.h UT_BVHImpl.h UT_SolidAngle.h UT_Array.cpp UT_SolidAngle.cpp | sed -e "s/^#.*include  *\".*$//g")" > ~/Repos/libigl/include/igl/FastWindingNumberForSoups.h` 
// MIT License

// Copyright (c) 2018 Side Effects Software Inc.

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// # Fast Winding Numbers for Soups

// https://github.com/alecjacobson/WindingNumber

// Implementation of the _ACM SIGGRAPH_ 2018 paper, 

// "Fast Winding Numbers for Soups and Clouds" 

// Gavin Barill¹, Neil Dickson², Ryan Schmidt³, David I.W. Levin¹, Alec Jacobson¹

// ¹University of Toronto, ²SideFX, ³Gradient Space


// _Note: this implementation is for triangle soups only, not point clouds._

// This version does _not_ depend on Intel TBB. Instead it depends on
// [libigl](https://github.com/libigl/libigl)'s simpler `igl::parallel_for` (which
// uses `std::thread`)

// <del>This code, as written, depends on Intel's Threading Building Blocks (TBB) library for parallelism, but it should be fairly easy to change it to use any other means of threading, since it only uses parallel for loops with simple partitioning.</del>

// The main class of interest is UT_SolidAngle and its init and computeSolidAngle functions, which you can use by including UT_SolidAngle.h, and whose implementation is mostly in UT_SolidAngle.cpp, using a 4-way bounding volume hierarchy (BVH) implemented in the UT_BVH.h and UT_BVHImpl.h headers.  The rest of the files are mostly various supporting code.  UT_SubtendedAngle, for computing angles subtended by 2D curves, can also be found in UT_SolidAngle.h and UT_SolidAngle.cpp .

// An example of very similar code and how to use it to create a geometry operator (SOP) in Houdini can be found in the HDK examples (toolkit/samples/SOP/SOP_WindingNumber) for Houdini 16.5.121 and later.  Query points go in the first input and the mesh geometry goes in the second input.


// Create a single header using:

//     echo "// This header created by issuing: \`$BASH_COMMAND\` $(echo "" | cat - LICENSE README.md | sed -e "s#^..*#\/\/ &#") $(echo "" | cat - SYS_Types.h SYS_Math.h VM_SSEFunc.h VM_SIMD.h UT_Array.h UT_ArrayImpl.h UT_SmallArray.h UT_FixedVector.h UT_ParallelUtil.h UT_BVH.h UT_BVHImpl.h UT_SolidAngle.h UT_Array.cpp UT_SolidAngle.cpp | sed -e "s/^#.*include  *\".*$//g")" 
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Common type definitions.
 */

#pragma once

#ifndef __SYS_Types__
#define __SYS_Types__

/* Include system types */
#include <limits>
#include <type_traits>
#include <sys/types.h>
#include <stdint.h>

namespace igl {
  /// @private
  namespace FastWindingNumber {

/*
 * Integer types
 */
typedef signed char	int8;
typedef	unsigned char	uint8;
typedef short		int16;
typedef unsigned short	uint16;
typedef	int		int32;
typedef unsigned int	uint32;

#ifndef MBSD
typedef unsigned int	uint;
#endif

/*
 * Avoid using uint64.
 * The extra bit of precision is NOT worth the cost in pain and suffering
 * induced by use of unsigned.
 */
#if defined(_WIN32)
    typedef __int64		int64;
    typedef unsigned __int64	uint64;
#elif defined(MBSD)
    // On MBSD, int64/uint64 are also defined in the system headers so we must
    // declare these in the same way or else we get conflicts.
    typedef int64_t		int64;
    typedef uint64_t		uint64;
#elif defined(AMD64)
    typedef long		int64;
    typedef unsigned long	uint64;
#else
    typedef long long		int64;
    typedef unsigned long long	uint64;
#endif

/// The problem with int64 is that it implies that it is a fixed 64-bit quantity
/// that is saved to disk. Therefore, we need another integral type for
/// indexing our arrays.
typedef int64 exint;

/// Mark function to be inlined. If this is done, taking the address of such
/// a function is not allowed.
#if defined(__GNUC__) || defined(__clang__)
#define SYS_FORCE_INLINE	__attribute__ ((always_inline)) inline
#elif defined(_MSC_VER)
#define SYS_FORCE_INLINE	__forceinline
#else
#define SYS_FORCE_INLINE	inline
#endif

/// Floating Point Types
typedef float   fpreal32;
typedef double  fpreal64;

/// SYS_FPRealUnionT for type-safe casting with integral types
template <typename T>
union SYS_FPRealUnionT;

template <>
union SYS_FPRealUnionT<fpreal32>
{
    typedef int32	int_type;
    typedef uint32	uint_type;
    typedef fpreal32	fpreal_type;

    enum {
	EXPONENT_BITS = 8,
	MANTISSA_BITS = 23,
	EXPONENT_BIAS = 127 };

    int_type		ival;
    uint_type		uval;
    fpreal_type		fval;
    
    struct
    {
	uint_type mantissa_val: 23;
	uint_type exponent_val: 8;
	uint_type sign_val: 1;
    };
};

template <>
union SYS_FPRealUnionT<fpreal64>
{
    typedef int64	int_type;
    typedef uint64	uint_type;
    typedef fpreal64	fpreal_type;

    enum {
	EXPONENT_BITS = 11,
	MANTISSA_BITS = 52,
	EXPONENT_BIAS = 1023 };

    int_type		ival;
    uint_type		uval;
    fpreal_type		fval;
    
    struct
    {
	uint_type mantissa_val: 52;
	uint_type exponent_val: 11;
	uint_type sign_val: 1;
    };
};

typedef union SYS_FPRealUnionT<fpreal32>    SYS_FPRealUnionF;
typedef union SYS_FPRealUnionT<fpreal64>    SYS_FPRealUnionD;

/// Asserts are disabled
/// @{
#define UT_IGL_ASSERT_P(ZZ)         ((void)0)
#define UT_IGL_ASSERT(ZZ)           ((void)0)
#define UT_IGL_ASSERT_MSG_P(ZZ, MM) ((void)0)
#define UT_IGL_ASSERT_MSG(ZZ, MM)   ((void)0)
/// @}
}}

#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Miscellaneous math functions. 
 */

#pragma once

#ifndef __SYS_Math__
#define __SYS_Math__



#include <float.h>
#include <limits>
#include <math.h>

namespace igl {
  /// @private
  namespace FastWindingNumber {

// NOTE:
// These have been carefully written so that in the case of equality
// we always return the first parameter.  This is so that NANs in
// in the second parameter are suppressed.
#define h_min(a, b)	(((a) > (b)) ? (b) : (a))
#define h_max(a, b)	(((a) < (b)) ? (b) : (a))
// DO NOT CHANGE THE ABOVE WITHOUT READING THE COMMENT
#define h_abs(a)	(((a) > 0) ? (a) : -(a))

static constexpr inline  int16 SYSmin(int16 a, int16 b)		{ return h_min(a,b); }
static constexpr inline  int16 SYSmax(int16 a, int16 b)		{ return h_max(a,b); }
static constexpr inline  int16 SYSabs(int16 a)			{ return h_abs(a); }
static constexpr inline  int32 SYSmin(int32 a, int32 b)		{ return h_min(a,b); }
static constexpr inline  int32 SYSmax(int32 a, int32 b)		{ return h_max(a,b); }
static constexpr inline  int32 SYSabs(int32 a)			{ return h_abs(a); }
static constexpr inline  int64 SYSmin(int64 a, int64 b)		{ return h_min(a,b); }
static constexpr inline  int64 SYSmax(int64 a, int64 b)		{ return h_max(a,b); }
static constexpr inline  int64 SYSmin(int32 a, int64 b)		{ return h_min(a,b); }
static constexpr inline  int64 SYSmax(int32 a, int64 b)		{ return h_max(a,b); }
static constexpr inline  int64 SYSmin(int64 a, int32 b)		{ return h_min(a,b); }
static constexpr inline  int64 SYSmax(int64 a, int32 b)		{ return h_max(a,b); }
static constexpr inline  int64 SYSabs(int64 a)			{ return h_abs(a); }
static constexpr inline uint16 SYSmin(uint16 a, uint16 b)		{ return h_min(a,b); }
static constexpr inline uint16 SYSmax(uint16 a, uint16 b)		{ return h_max(a,b); }
static constexpr inline uint32 SYSmin(uint32 a, uint32 b)		{ return h_min(a,b); }
static constexpr inline uint32 SYSmax(uint32 a, uint32 b)		{ return h_max(a,b); }
static constexpr inline uint64 SYSmin(uint64 a, uint64 b)		{ return h_min(a,b); }
static constexpr inline uint64 SYSmax(uint64 a, uint64 b)		{ return h_max(a,b); }
static constexpr inline fpreal32 SYSmin(fpreal32 a, fpreal32 b)	{ return h_min(a,b); }
static constexpr inline fpreal32 SYSmax(fpreal32 a, fpreal32 b)	{ return h_max(a,b); }
static constexpr inline fpreal64 SYSmin(fpreal64 a, fpreal64 b)	{ return h_min(a,b); }
static constexpr inline fpreal64 SYSmax(fpreal64 a, fpreal64 b)	{ return h_max(a,b); }

// Some systems have size_t as a seperate type from uint.  Some don't.
#if (defined(LINUX) && defined(IA64)) || defined(MBSD)
static constexpr inline size_t SYSmin(size_t a, size_t b)		{ return h_min(a,b); }
static constexpr inline size_t SYSmax(size_t a, size_t b)		{ return h_max(a,b); }
#endif

#undef h_min
#undef h_max
#undef h_abs

#define h_clamp(val, min, max, tol)	\
	    ((val <= min+tol) ? min : ((val >= max-tol) ? max : val))

    static constexpr inline int
    SYSclamp(int v, int min, int max)
	{ return h_clamp(v, min, max, 0); }

    static constexpr inline uint
    SYSclamp(uint v, uint min, uint max)
	{ return h_clamp(v, min, max, 0); }

    static constexpr inline int64
    SYSclamp(int64 v, int64 min, int64 max)
	{ return h_clamp(v, min, max, int64(0)); }

    static constexpr inline uint64
    SYSclamp(uint64 v, uint64 min, uint64 max)
	{ return h_clamp(v, min, max, uint64(0)); }

    static constexpr inline fpreal32
    SYSclamp(fpreal32 v, fpreal32 min, fpreal32 max, fpreal32 tol=(fpreal32)0)
	{ return h_clamp(v, min, max, tol); }

    static constexpr inline fpreal64
    SYSclamp(fpreal64 v, fpreal64 min, fpreal64 max, fpreal64 tol=(fpreal64)0)
	{ return h_clamp(v, min, max, tol); }

#undef h_clamp

static inline fpreal64 SYSsqrt(fpreal64 arg)
{ return ::sqrt(arg); }
static inline fpreal32 SYSsqrt(fpreal32 arg)
{ return ::sqrtf(arg); }
static inline fpreal64 SYSatan2(fpreal64 a, fpreal64 b)
{ return ::atan2(a, b); }
static inline fpreal32 SYSatan2(fpreal32 a, fpreal32 b)
{ return ::atan2(a, b); }

static inline fpreal32 SYSabs(fpreal32 a) { return ::fabsf(a); }
static inline fpreal64 SYSabs(fpreal64 a) { return ::fabs(a); }

}}

#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      SIMD wrapper functions for SSE instructions
 */

#pragma once
#ifdef __SSE__

#ifndef __VM_SSEFunc__
#define __VM_SSEFunc__



#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable:4799)
#endif

#define CPU_HAS_SIMD_INSTR	1
#define VM_SSE_STYLE		1

#include <emmintrin.h>

#if defined(__SSE4_1__)
#define VM_SSE41_STYLE		1
#include <smmintrin.h>
#endif

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

namespace igl {
  /// @private
  namespace FastWindingNumber {

typedef __m128	v4sf;
typedef __m128i	v4si;

// Plain casting (no conversion)
// MSVC has problems casting between __m128 and __m128i, so we implement a
// custom casting routine specifically for windows.

#if defined(_MSC_VER)

static SYS_FORCE_INLINE v4sf
vm_v4sf(const v4si &a)
{
    union {
	v4si ival;
	v4sf fval;
    };
    ival = a;
    return fval;
}

static SYS_FORCE_INLINE v4si
vm_v4si(const v4sf &a)
{
    union {
	v4si ival;
	v4sf fval;
    };
    fval = a;
    return ival;
}

#define V4SF(A)		vm_v4sf(A)
#define V4SI(A)		vm_v4si(A)

#else

#define V4SF(A)		(v4sf)A
#define V4SI(A)		(v4si)A

#endif

#define VM_SHUFFLE_MASK(a0,a1, b0,b1)	((b1)<<6|(b0)<<4 | (a1)<<2|(a0))

template <int mask>
static SYS_FORCE_INLINE v4sf
vm_shuffle(const v4sf &a, const v4sf &b)
{
    return _mm_shuffle_ps(a, b, mask);
}

template <int mask>
static SYS_FORCE_INLINE v4si
vm_shuffle(const v4si &a, const v4si &b)
{
    return V4SI(_mm_shuffle_ps(V4SF(a), V4SF(b), mask));
}

template <int A, int B, int C, int D, typename T>
static SYS_FORCE_INLINE T
vm_shuffle(const T &a, const T &b)
{
    return vm_shuffle<VM_SHUFFLE_MASK(A,B,C,D)>(a, b);
}

template <int mask, typename T>
static SYS_FORCE_INLINE T
vm_shuffle(const T &a)
{
    return vm_shuffle<mask>(a, a);
}

template <int A, int B, int C, int D, typename T>
static SYS_FORCE_INLINE T
vm_shuffle(const T &a)
{
    return vm_shuffle<A,B,C,D>(a, a);
}

#if defined(VM_SSE41_STYLE)

static SYS_FORCE_INLINE v4si
vm_insert(const v4si v, int32 a, int n)
{
    switch (n)
    {
    case 0: return _mm_insert_epi32(v, a, 0);
    case 1: return _mm_insert_epi32(v, a, 1);
    case 2: return _mm_insert_epi32(v, a, 2);
    case 3: return _mm_insert_epi32(v, a, 3);
    }
    return v;
}

static SYS_FORCE_INLINE v4sf
vm_insert(const v4sf v, float a, int n)
{
    switch (n)
    {
    case 0: return _mm_insert_ps(v, _mm_set_ss(a), _MM_MK_INSERTPS_NDX(0,0,0));
    case 1: return _mm_insert_ps(v, _mm_set_ss(a), _MM_MK_INSERTPS_NDX(0,1,0));
    case 2: return _mm_insert_ps(v, _mm_set_ss(a), _MM_MK_INSERTPS_NDX(0,2,0));
    case 3: return _mm_insert_ps(v, _mm_set_ss(a), _MM_MK_INSERTPS_NDX(0,3,0));
    }
    return v;
}

static SYS_FORCE_INLINE int
vm_extract(const v4si v, int n)
{
    switch (n)
    {
    case 0: return _mm_extract_epi32(v, 0);
    case 1: return _mm_extract_epi32(v, 1);
    case 2: return _mm_extract_epi32(v, 2);
    case 3: return _mm_extract_epi32(v, 3);
    }
    return 0;
}

static SYS_FORCE_INLINE float
vm_extract(const v4sf v, int n)
{
    SYS_FPRealUnionF	tmp;
    switch (n)
    {
    case 0: tmp.ival = _mm_extract_ps(v, 0); break;
    case 1: tmp.ival = _mm_extract_ps(v, 1); break;
    case 2: tmp.ival = _mm_extract_ps(v, 2); break;
    case 3: tmp.ival = _mm_extract_ps(v, 3); break;
    }
    return tmp.fval;
}

#else

static SYS_FORCE_INLINE v4si
vm_insert(const v4si v, int32 a, int n)
{
    union { v4si vector; int32 comp[4]; };
    vector = v;
    comp[n] = a;
    return vector;
}

static SYS_FORCE_INLINE v4sf
vm_insert(const v4sf v, float a, int n)
{
    union { v4sf vector; float comp[4]; };
    vector = v;
    comp[n] = a;
    return vector;
}

static SYS_FORCE_INLINE int
vm_extract(const v4si v, int n)
{
    union { v4si vector; int32 comp[4]; };
    vector = v;
    return comp[n];
}

static SYS_FORCE_INLINE float
vm_extract(const v4sf v, int n)
{
    union { v4sf vector; float comp[4]; };
    vector = v;
    return comp[n];
}

#endif

static SYS_FORCE_INLINE v4sf
vm_splats(float a)
{
    return _mm_set1_ps(a);
}

static SYS_FORCE_INLINE v4si
vm_splats(uint32 a)
{
    SYS_FPRealUnionF	tmp;
    tmp.uval = a;
    return V4SI(vm_splats(tmp.fval));
}

static SYS_FORCE_INLINE v4si
vm_splats(int32 a)
{
    SYS_FPRealUnionF	tmp;
    tmp.ival = a;
    return V4SI(vm_splats(tmp.fval));
}

static SYS_FORCE_INLINE v4sf
vm_splats(float a, float b, float c, float d)
{
    return vm_shuffle<0,2,0,2>(
	    vm_shuffle<0>(_mm_set_ss(a), _mm_set_ss(b)),
	    vm_shuffle<0>(_mm_set_ss(c), _mm_set_ss(d)));
}

static SYS_FORCE_INLINE v4si
vm_splats(uint32 a, uint32 b, uint32 c, uint32 d)
{
    SYS_FPRealUnionF	af, bf, cf, df;
    af.uval = a;
    bf.uval = b;
    cf.uval = c;
    df.uval = d;
    return V4SI(vm_splats(af.fval, bf.fval, cf.fval, df.fval));
}

static SYS_FORCE_INLINE v4si
vm_splats(int32 a, int32 b, int32 c, int32 d)
{
    SYS_FPRealUnionF	af, bf, cf, df;
    af.ival = a;
    bf.ival = b;
    cf.ival = c;
    df.ival = d;
    return V4SI(vm_splats(af.fval, bf.fval, cf.fval, df.fval));
}

static SYS_FORCE_INLINE v4si
vm_load(const int32 v[4])
{
    return V4SI(_mm_loadu_ps((const float *)v));
}

static SYS_FORCE_INLINE v4sf
vm_load(const float v[4])
{
    return _mm_loadu_ps(v);
}

static SYS_FORCE_INLINE void
vm_store(float dst[4], v4sf value)
{
    _mm_storeu_ps(dst, value);
}

static SYS_FORCE_INLINE v4sf
vm_negate(v4sf a)
{
    return _mm_sub_ps(_mm_setzero_ps(), a);
}

static SYS_FORCE_INLINE v4sf
vm_abs(v4sf a)
{
    return _mm_max_ps(a, vm_negate(a));
}

static SYS_FORCE_INLINE v4sf
vm_fdiv(v4sf a, v4sf b)
{
    return _mm_mul_ps(a, _mm_rcp_ps(b));
}

static SYS_FORCE_INLINE v4sf
vm_fsqrt(v4sf a)
{
    return _mm_rcp_ps(_mm_rsqrt_ps(a));
}

static SYS_FORCE_INLINE v4sf
vm_madd(v4sf a, v4sf b, v4sf c)
{
    return _mm_add_ps(_mm_mul_ps(a, b), c);
}

static const v4si	theSSETrue = vm_splats(0xFFFFFFFF);

static SYS_FORCE_INLINE bool
vm_allbits(const v4si &a)
{
    return _mm_movemask_ps(V4SF(_mm_cmpeq_epi32(a, theSSETrue))) == 0xF;
}


#define VM_EXTRACT	vm_extract
#define VM_INSERT	vm_insert
#define VM_SPLATS	vm_splats
#define VM_LOAD		vm_load
#define VM_STORE	vm_store

#define VM_CMPLT(A,B)	V4SI(_mm_cmplt_ps(A,B))
#define VM_CMPLE(A,B)	V4SI(_mm_cmple_ps(A,B))
#define VM_CMPGT(A,B)	V4SI(_mm_cmpgt_ps(A,B))
#define VM_CMPGE(A,B)	V4SI(_mm_cmpge_ps(A,B))
#define VM_CMPEQ(A,B)	V4SI(_mm_cmpeq_ps(A,B))
#define VM_CMPNE(A,B)	V4SI(_mm_cmpneq_ps(A,B))

#define VM_ICMPLT	_mm_cmplt_epi32
#define VM_ICMPGT	_mm_cmpgt_epi32
#define VM_ICMPEQ	_mm_cmpeq_epi32

#define VM_IADD		_mm_add_epi32
#define VM_ISUB		_mm_sub_epi32

#define VM_ADD		_mm_add_ps
#define VM_SUB		_mm_sub_ps
#define VM_MUL		_mm_mul_ps
#define VM_DIV		_mm_div_ps
#define VM_SQRT		_mm_sqrt_ps
#define VM_ISQRT	_mm_rsqrt_ps
#define VM_INVERT	_mm_rcp_ps
#define VM_ABS		vm_abs

#define VM_FDIV		vm_fdiv
#define VM_NEG		vm_negate
#define VM_FSQRT	vm_fsqrt
#define VM_MADD		vm_madd

#define VM_MIN		_mm_min_ps
#define VM_MAX		_mm_max_ps

#define VM_AND		_mm_and_si128
#define VM_ANDNOT	_mm_andnot_si128
#define VM_OR		_mm_or_si128
#define VM_XOR		_mm_xor_si128

#define VM_ALLBITS	vm_allbits

#define VM_SHUFFLE	vm_shuffle

// Integer to float conversions
#define VM_SSE_ROUND_MASK	0x6000
#define VM_SSE_ROUND_ZERO	0x6000
#define VM_SSE_ROUND_UP		0x4000
#define VM_SSE_ROUND_DOWN	0x2000
#define VM_SSE_ROUND_NEAR	0x0000

#define GETROUND()	(_mm_getcsr()&VM_SSE_ROUND_MASK)
#define SETROUND(x)	(_mm_setcsr(x|(_mm_getcsr()&~VM_SSE_ROUND_MASK)))

// The P functions must be invoked before FLOOR, the E functions invoked
// afterwards to reset the state.

#define VM_P_FLOOR()	uint rounding = GETROUND(); \
			    SETROUND(VM_SSE_ROUND_DOWN);
#define VM_FLOOR	_mm_cvtps_epi32
#define VM_INT		_mm_cvttps_epi32
#define VM_E_FLOOR()	SETROUND(rounding);

// Float to integer conversion
#define VM_IFLOAT	_mm_cvtepi32_ps
}}

#endif
#endif
#pragma once
#ifndef __SSE__
#ifndef __VM_SIMDFunc__
#define __VM_SIMDFunc__



#include <cmath>

namespace igl {
  /// @private
  namespace FastWindingNumber {

struct v4si {
	int32 v[4];
};

struct v4sf {
	float v[4];
};

static SYS_FORCE_INLINE v4sf V4SF(const v4si &v) {
	static_assert(sizeof(v4si) == sizeof(v4sf) && alignof(v4si) == alignof(v4sf), "v4si and v4sf must be compatible");
	return *(const v4sf*)&v;
}

static SYS_FORCE_INLINE v4si V4SI(const v4sf &v) {
	static_assert(sizeof(v4si) == sizeof(v4sf) && alignof(v4si) == alignof(v4sf), "v4si and v4sf must be compatible");
	return *(const v4si*)&v;
}

static SYS_FORCE_INLINE int32 conditionMask(bool c) {
	return c ? int32(0xFFFFFFFF) : 0;
}

static SYS_FORCE_INLINE v4sf
VM_SPLATS(float f) {
	return v4sf{{f, f, f, f}};
}

static SYS_FORCE_INLINE v4si
VM_SPLATS(uint32 i) {
	return v4si{{int32(i), int32(i), int32(i), int32(i)}};
}

static SYS_FORCE_INLINE v4si
VM_SPLATS(int32 i) {
	return v4si{{i, i, i, i}};
}

static SYS_FORCE_INLINE v4sf
VM_SPLATS(float a, float b, float c, float d) {
	return v4sf{{a, b, c, d}};
}

static SYS_FORCE_INLINE v4si
VM_SPLATS(uint32 a, uint32 b, uint32 c, uint32 d) {
	return v4si{{int32(a), int32(b), int32(c), int32(d)}};
}

static SYS_FORCE_INLINE v4si
VM_SPLATS(int32 a, int32 b, int32 c, int32 d) {
	return v4si{{a, b, c, d}};
}

static SYS_FORCE_INLINE v4si
VM_LOAD(const int32 v[4]) {
	return v4si{{v[0], v[1], v[2], v[3]}};
}

static SYS_FORCE_INLINE v4sf
VM_LOAD(const float v[4]) {
	return v4sf{{v[0], v[1], v[2], v[3]}};
}


static inline v4si VM_ICMPEQ(v4si a, v4si b) {
	return v4si{{
		conditionMask(a.v[0] == b.v[0]),
		conditionMask(a.v[1] == b.v[1]),
		conditionMask(a.v[2] == b.v[2]),
		conditionMask(a.v[3] == b.v[3])
	}};
}

static inline v4si VM_ICMPGT(v4si a, v4si b) {
	return v4si{{
		conditionMask(a.v[0] > b.v[0]),
		conditionMask(a.v[1] > b.v[1]),
		conditionMask(a.v[2] > b.v[2]),
		conditionMask(a.v[3] > b.v[3])
	}};
}

static inline v4si VM_ICMPLT(v4si a, v4si b) {
	return v4si{{
		conditionMask(a.v[0] < b.v[0]),
		conditionMask(a.v[1] < b.v[1]),
		conditionMask(a.v[2] < b.v[2]),
		conditionMask(a.v[3] < b.v[3])
	}};
}

static inline v4si VM_IADD(v4si a, v4si b) {
	return v4si{{
		(a.v[0] + b.v[0]),
		(a.v[1] + b.v[1]),
		(a.v[2] + b.v[2]),
		(a.v[3] + b.v[3])
	}};
}

static inline v4si VM_ISUB(v4si a, v4si b) {
	return v4si{{
		(a.v[0] - b.v[0]),
		(a.v[1] - b.v[1]),
		(a.v[2] - b.v[2]),
		(a.v[3] - b.v[3])
	}};
}

static inline v4si VM_OR(v4si a, v4si b) {
	return v4si{{
		(a.v[0] | b.v[0]),
		(a.v[1] | b.v[1]),
		(a.v[2] | b.v[2]),
		(a.v[3] | b.v[3])
	}};
}

static inline v4si VM_AND(v4si a, v4si b) {
	return v4si{{
		(a.v[0] & b.v[0]),
		(a.v[1] & b.v[1]),
		(a.v[2] & b.v[2]),
		(a.v[3] & b.v[3])
	}};
}

static inline v4si VM_ANDNOT(v4si a, v4si b) {
	return v4si{{
		((~a.v[0]) & b.v[0]),
		((~a.v[1]) & b.v[1]),
		((~a.v[2]) & b.v[2]),
		((~a.v[3]) & b.v[3])
	}};
}

static inline v4si VM_XOR(v4si a, v4si b) {
	return v4si{{
		(a.v[0] ^ b.v[0]),
		(a.v[1] ^ b.v[1]),
		(a.v[2] ^ b.v[2]),
		(a.v[3] ^ b.v[3])
	}};
}

static SYS_FORCE_INLINE int
VM_EXTRACT(const v4si v, int index) {
	return v.v[index];
}

static SYS_FORCE_INLINE float
VM_EXTRACT(const v4sf v, int index) {
	return v.v[index];
}

static SYS_FORCE_INLINE v4si
VM_INSERT(v4si v, int32 value, int index) {
	v.v[index] = value;
	return v;
}

static SYS_FORCE_INLINE v4sf
VM_INSERT(v4sf v, float value, int index) {
	v.v[index] = value;
	return v;
}

static inline v4si VM_CMPEQ(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] == b.v[0]),
		conditionMask(a.v[1] == b.v[1]),
		conditionMask(a.v[2] == b.v[2]),
		conditionMask(a.v[3] == b.v[3])
	}};
}

static inline v4si VM_CMPNE(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] != b.v[0]),
		conditionMask(a.v[1] != b.v[1]),
		conditionMask(a.v[2] != b.v[2]),
		conditionMask(a.v[3] != b.v[3])
	}};
}

static inline v4si VM_CMPGT(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] > b.v[0]),
		conditionMask(a.v[1] > b.v[1]),
		conditionMask(a.v[2] > b.v[2]),
		conditionMask(a.v[3] > b.v[3])
	}};
}

static inline v4si VM_CMPLT(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] < b.v[0]),
		conditionMask(a.v[1] < b.v[1]),
		conditionMask(a.v[2] < b.v[2]),
		conditionMask(a.v[3] < b.v[3])
	}};
}

static inline v4si VM_CMPGE(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] >= b.v[0]),
		conditionMask(a.v[1] >= b.v[1]),
		conditionMask(a.v[2] >= b.v[2]),
		conditionMask(a.v[3] >= b.v[3])
	}};
}

static inline v4si VM_CMPLE(v4sf a, v4sf b) {
	return v4si{{
		conditionMask(a.v[0] <= b.v[0]),
		conditionMask(a.v[1] <= b.v[1]),
		conditionMask(a.v[2] <= b.v[2]),
		conditionMask(a.v[3] <= b.v[3])
	}};
}

static inline v4sf VM_ADD(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] + b.v[0]),
		(a.v[1] + b.v[1]),
		(a.v[2] + b.v[2]),
		(a.v[3] + b.v[3])
	}};
}

static inline v4sf VM_SUB(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] - b.v[0]),
		(a.v[1] - b.v[1]),
		(a.v[2] - b.v[2]),
		(a.v[3] - b.v[3])
	}};
}

static inline v4sf VM_NEG(v4sf a) {
	return v4sf{{
		(-a.v[0]),
		(-a.v[1]),
		(-a.v[2]),
		(-a.v[3])
	}};
}

static inline v4sf VM_MUL(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] * b.v[0]),
		(a.v[1] * b.v[1]),
		(a.v[2] * b.v[2]),
		(a.v[3] * b.v[3])
	}};
}

static inline v4sf VM_DIV(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] / b.v[0]),
		(a.v[1] / b.v[1]),
		(a.v[2] / b.v[2]),
		(a.v[3] / b.v[3])
	}};
}

static inline v4sf VM_MADD(v4sf a, v4sf b, v4sf c) {
	return v4sf{{
		(a.v[0] * b.v[0]) + c.v[0],
		(a.v[1] * b.v[1]) + c.v[1],
		(a.v[2] * b.v[2]) + c.v[2],
		(a.v[3] * b.v[3]) + c.v[3]
	}};
}

static inline v4sf VM_ABS(v4sf a) {
	return v4sf{{
		(a.v[0] < 0) ? -a.v[0] : a.v[0],
		(a.v[1] < 0) ? -a.v[1] : a.v[1],
		(a.v[2] < 0) ? -a.v[2] : a.v[2],
		(a.v[3] < 0) ? -a.v[3] : a.v[3]
	}};
}

static inline v4sf VM_MAX(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] < b.v[0]) ? b.v[0] : a.v[0],
		(a.v[1] < b.v[1]) ? b.v[1] : a.v[1],
		(a.v[2] < b.v[2]) ? b.v[2] : a.v[2],
		(a.v[3] < b.v[3]) ? b.v[3] : a.v[3]
	}};
}

static inline v4sf VM_MIN(v4sf a, v4sf b) {
	return v4sf{{
		(a.v[0] > b.v[0]) ? b.v[0] : a.v[0],
		(a.v[1] > b.v[1]) ? b.v[1] : a.v[1],
		(a.v[2] > b.v[2]) ? b.v[2] : a.v[2],
		(a.v[3] > b.v[3]) ? b.v[3] : a.v[3]
	}};
}

static inline v4sf VM_INVERT(v4sf a) {
	return v4sf{{
		(1.0f/a.v[0]),
		(1.0f/a.v[1]),
		(1.0f/a.v[2]),
		(1.0f/a.v[3])
	}};
}

static inline v4sf VM_SQRT(v4sf a) {
	return v4sf{{
		std::sqrt(a.v[0]),
		std::sqrt(a.v[1]),
		std::sqrt(a.v[2]),
		std::sqrt(a.v[3])
	}};
}

static inline v4si VM_INT(v4sf a) {
	return v4si{{
		int32(a.v[0]),
		int32(a.v[1]),
		int32(a.v[2]),
		int32(a.v[3])
	}};
}

static inline v4sf VM_IFLOAT(v4si a) {
	return v4sf{{
		float(a.v[0]),
		float(a.v[1]),
		float(a.v[2]),
		float(a.v[3])
	}};
}

static SYS_FORCE_INLINE void VM_P_FLOOR() {}

static SYS_FORCE_INLINE int32 singleIntFloor(float f) {
	// Casting to int32 usually truncates toward zero, instead of rounding down,
	// so subtract one if the result is above f.
	int32 i = int32(f);
	i -= (float(i) > f);
	return i;
}
static inline v4si VM_FLOOR(v4sf a) {
	return v4si{{
		singleIntFloor(a.v[0]),
		singleIntFloor(a.v[1]),
		singleIntFloor(a.v[2]),
		singleIntFloor(a.v[3])
	}};
}

static SYS_FORCE_INLINE void VM_E_FLOOR() {}

static SYS_FORCE_INLINE bool vm_allbits(v4si a) {
	return (
		(a.v[0] == -1) && 
		(a.v[1] == -1) && 
		(a.v[2] == -1) && 
		(a.v[3] == -1)
	);
}

int SYS_FORCE_INLINE _mm_movemask_ps(const v4si& v) {
	return (
		int(v.v[0] < 0) |
		(int(v.v[1] < 0)<<1) |
		(int(v.v[2] < 0)<<2) |
		(int(v.v[3] < 0)<<3)
	);
}

int SYS_FORCE_INLINE _mm_movemask_ps(const v4sf& v) {
	// Use std::signbit just in case it needs to distinguish between +0 and -0
	// or between positive and negative NaN values (e.g. these could really
	// be integers instead of floats).
	return (
		int(std::signbit(v.v[0])) |
		(int(std::signbit(v.v[1]))<<1) |
		(int(std::signbit(v.v[2]))<<2) |
		(int(std::signbit(v.v[3]))<<3)
	);
}
}}
#endif
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      SIMD wrapper classes for 4 floats or 4 ints
 */

#pragma once

#ifndef __HDK_VM_SIMD__
#define __HDK_VM_SIMD__


#include <cstdint>

//#define FORCE_NON_SIMD




namespace igl {
  /// @private
  namespace FastWindingNumber {

class v4uf;

class v4uu {
public:
    SYS_FORCE_INLINE v4uu() {}
    SYS_FORCE_INLINE v4uu(const v4si &v) : vector(v) {}
    SYS_FORCE_INLINE v4uu(const v4uu &v) : vector(v.vector) {}
    explicit SYS_FORCE_INLINE v4uu(int32 v) { vector = VM_SPLATS(v); }
    explicit SYS_FORCE_INLINE v4uu(const int32 v[4])
    { vector = VM_LOAD(v); }
    SYS_FORCE_INLINE v4uu(int32 a, int32 b, int32 c, int32 d)
    { vector = VM_SPLATS(a, b, c, d); }

    // Assignment
    SYS_FORCE_INLINE v4uu operator=(int32 v)
    { vector = v4uu(v).vector; return *this; }
    SYS_FORCE_INLINE v4uu operator=(v4si v)
    { vector = v; return *this; }
    SYS_FORCE_INLINE v4uu operator=(const v4uu &v)
    { vector = v.vector; return *this; }

    SYS_FORCE_INLINE void condAssign(const v4uu &val, const v4uu &c)
    { *this = (c & val) | ((!c) & *this); }

    // Comparison
    SYS_FORCE_INLINE v4uu operator == (const v4uu &v) const
    { return v4uu(VM_ICMPEQ(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator != (const v4uu &v) const
    { return ~(*this == v); }
    SYS_FORCE_INLINE v4uu operator >  (const v4uu &v) const
    { return v4uu(VM_ICMPGT(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator <  (const v4uu &v) const
    { return v4uu(VM_ICMPLT(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator >= (const v4uu &v) const
    { return ~(*this < v); }
    SYS_FORCE_INLINE v4uu operator <= (const v4uu &v) const
    { return ~(*this > v); }

    SYS_FORCE_INLINE v4uu operator == (int32 v) const { return *this == v4uu(v); }
    SYS_FORCE_INLINE v4uu operator != (int32 v) const { return *this != v4uu(v); }
    SYS_FORCE_INLINE v4uu operator >  (int32 v) const { return *this > v4uu(v); }
    SYS_FORCE_INLINE v4uu operator <  (int32 v) const { return *this < v4uu(v); }
    SYS_FORCE_INLINE v4uu operator >= (int32 v) const { return *this >= v4uu(v); }
    SYS_FORCE_INLINE v4uu operator <= (int32 v) const { return *this <= v4uu(v); }

    // Basic math
    SYS_FORCE_INLINE v4uu operator+(const v4uu &r) const
    { return v4uu(VM_IADD(vector, r.vector)); }
    SYS_FORCE_INLINE v4uu operator-(const v4uu &r) const
    { return v4uu(VM_ISUB(vector, r.vector)); }
    SYS_FORCE_INLINE v4uu operator+=(const v4uu &r) { return (*this = *this + r); }
    SYS_FORCE_INLINE v4uu operator-=(const v4uu &r) { return (*this = *this - r); }
    SYS_FORCE_INLINE v4uu operator+(int32 r) const { return *this + v4uu(r); }
    SYS_FORCE_INLINE v4uu operator-(int32 r) const { return *this - v4uu(r); }
    SYS_FORCE_INLINE v4uu operator+=(int32 r) { return (*this = *this + r); }
    SYS_FORCE_INLINE v4uu operator-=(int32 r) { return (*this = *this - r); }

    // logical/bitwise

    SYS_FORCE_INLINE v4uu operator||(const v4uu &r) const
    { return v4uu(VM_OR(vector, r.vector)); }
    SYS_FORCE_INLINE v4uu operator&&(const v4uu &r) const
    { return v4uu(VM_AND(vector, r.vector)); }
    SYS_FORCE_INLINE v4uu operator^(const v4uu &r) const
    { return v4uu(VM_XOR(vector, r.vector)); }
    SYS_FORCE_INLINE v4uu operator!() const
    { return *this == v4uu(0); }

    SYS_FORCE_INLINE v4uu operator|(const v4uu &r) const { return *this || r; }
    SYS_FORCE_INLINE v4uu operator&(const v4uu &r) const { return *this && r; }
    SYS_FORCE_INLINE v4uu operator~() const
    { return *this ^ v4uu(0xFFFFFFFF); }

    // component
    SYS_FORCE_INLINE int32 operator[](int idx) const { return VM_EXTRACT(vector, idx); }
    SYS_FORCE_INLINE void setComp(int idx, int32 v) { vector = VM_INSERT(vector, v, idx); }

    v4uf toFloat() const;

public:
    v4si vector;
};

class v4uf {
public:
    SYS_FORCE_INLINE v4uf() {}
    SYS_FORCE_INLINE v4uf(const v4sf &v) : vector(v) {}
    SYS_FORCE_INLINE v4uf(const v4uf &v) : vector(v.vector) {}
    explicit SYS_FORCE_INLINE v4uf(float v) { vector = VM_SPLATS(v); }
    explicit SYS_FORCE_INLINE v4uf(const float v[4])
    { vector = VM_LOAD(v); }
    SYS_FORCE_INLINE v4uf(float a, float b, float c, float d)
    { vector = VM_SPLATS(a, b, c, d); }

    // Assignment
    SYS_FORCE_INLINE v4uf operator=(float v)
    { vector = v4uf(v).vector; return *this; }
    SYS_FORCE_INLINE v4uf operator=(v4sf v)
    { vector = v; return *this; }
    SYS_FORCE_INLINE v4uf operator=(const v4uf &v)
    { vector = v.vector; return *this; }

    SYS_FORCE_INLINE void condAssign(const v4uf &val, const v4uu &c)
    { *this = (val & c) | (*this & ~c); }

    // Comparison
    SYS_FORCE_INLINE v4uu operator == (const v4uf &v) const
    { return v4uu(VM_CMPEQ(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator != (const v4uf &v) const
    { return v4uu(VM_CMPNE(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator >  (const v4uf &v) const
    { return v4uu(VM_CMPGT(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator <  (const v4uf &v) const
    { return v4uu(VM_CMPLT(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator >= (const v4uf &v) const
    { return v4uu(VM_CMPGE(vector, v.vector)); }
    SYS_FORCE_INLINE v4uu operator <= (const v4uf &v) const
    { return v4uu(VM_CMPLE(vector, v.vector)); }

    SYS_FORCE_INLINE v4uu operator == (float v) const { return *this == v4uf(v); }
    SYS_FORCE_INLINE v4uu operator != (float v) const { return *this != v4uf(v); }
    SYS_FORCE_INLINE v4uu operator >  (float v) const { return *this > v4uf(v); }
    SYS_FORCE_INLINE v4uu operator <  (float v) const { return *this < v4uf(v); }
    SYS_FORCE_INLINE v4uu operator >= (float v) const { return *this >= v4uf(v); }
    SYS_FORCE_INLINE v4uu operator <= (float v) const { return *this <= v4uf(v); }


    // Basic math
    SYS_FORCE_INLINE v4uf operator+(const v4uf &r) const
    { return v4uf(VM_ADD(vector, r.vector)); }
    SYS_FORCE_INLINE v4uf operator-(const v4uf &r) const
    { return v4uf(VM_SUB(vector, r.vector)); }
    SYS_FORCE_INLINE v4uf operator-() const
    { return v4uf(VM_NEG(vector)); }
    SYS_FORCE_INLINE v4uf operator*(const v4uf &r) const
    { return v4uf(VM_MUL(vector, r.vector)); }
    SYS_FORCE_INLINE v4uf operator/(const v4uf &r) const
    { return v4uf(VM_DIV(vector, r.vector)); }

    SYS_FORCE_INLINE v4uf operator+=(const v4uf &r) { return (*this = *this + r); }
    SYS_FORCE_INLINE v4uf operator-=(const v4uf &r) { return (*this = *this - r); }
    SYS_FORCE_INLINE v4uf operator*=(const v4uf &r) { return (*this = *this * r); }
    SYS_FORCE_INLINE v4uf operator/=(const v4uf &r) { return (*this = *this / r); }

    SYS_FORCE_INLINE v4uf operator+(float r) const { return *this + v4uf(r); }
    SYS_FORCE_INLINE v4uf operator-(float r) const { return *this - v4uf(r); }
    SYS_FORCE_INLINE v4uf operator*(float r) const { return *this * v4uf(r); }
    SYS_FORCE_INLINE v4uf operator/(float r) const { return *this / v4uf(r); }
    SYS_FORCE_INLINE v4uf operator+=(float r) { return (*this = *this + r); }
    SYS_FORCE_INLINE v4uf operator-=(float r) { return (*this = *this - r); }
    SYS_FORCE_INLINE v4uf operator*=(float r) { return (*this = *this * r); }
    SYS_FORCE_INLINE v4uf operator/=(float r) { return (*this = *this / r); }

    // logical/bitwise

    SYS_FORCE_INLINE v4uf operator||(const v4uu &r) const
    { return v4uf(V4SF(VM_OR(V4SI(vector), r.vector))); }
    SYS_FORCE_INLINE v4uf operator&&(const v4uu &r) const
    { return v4uf(V4SF(VM_AND(V4SI(vector), r.vector))); }
    SYS_FORCE_INLINE v4uf operator^(const v4uu &r) const
    { return v4uf(V4SF(VM_XOR(V4SI(vector), r.vector))); }
    SYS_FORCE_INLINE v4uf operator!() const
    { return v4uf(V4SF((*this == v4uf(0.0F)).vector)); }

    SYS_FORCE_INLINE v4uf operator||(const v4uf &r) const
    { return v4uf(V4SF(VM_OR(V4SI(vector), V4SI(r.vector)))); }
    SYS_FORCE_INLINE v4uf operator&&(const v4uf &r) const
    { return v4uf(V4SF(VM_AND(V4SI(vector), V4SI(r.vector)))); }
    SYS_FORCE_INLINE v4uf operator^(const v4uf &r) const
    { return v4uf(V4SF(VM_XOR(V4SI(vector), V4SI(r.vector)))); }

    SYS_FORCE_INLINE v4uf operator|(const v4uu &r) const { return *this || r; }
    SYS_FORCE_INLINE v4uf operator&(const v4uu &r) const { return *this && r; }
    SYS_FORCE_INLINE v4uf operator~() const
    { return *this ^ v4uu(0xFFFFFFFF); }

    SYS_FORCE_INLINE v4uf operator|(const v4uf &r) const { return *this || r; }
    SYS_FORCE_INLINE v4uf operator&(const v4uf &r) const { return *this && r; }

    // component
    SYS_FORCE_INLINE float operator[](int idx) const { return VM_EXTRACT(vector, idx); }
    SYS_FORCE_INLINE void setComp(int idx, float v) { vector = VM_INSERT(vector, v, idx); }

    // more math
    SYS_FORCE_INLINE v4uf abs() const { return v4uf(VM_ABS(vector)); }
    SYS_FORCE_INLINE v4uf clamp(const v4uf &low, const v4uf &high) const
    { return v4uf(
        VM_MIN(VM_MAX(vector, low.vector), high.vector)); }
    SYS_FORCE_INLINE v4uf clamp(float low, float high) const
    { return v4uf(VM_MIN(VM_MAX(vector,
        v4uf(low).vector), v4uf(high).vector)); }
    SYS_FORCE_INLINE v4uf recip() const { return v4uf(VM_INVERT(vector)); }

    /// This is a lie, it is a signed int.
    SYS_FORCE_INLINE v4uu toUnsignedInt() const { return VM_INT(vector); }
    SYS_FORCE_INLINE v4uu toSignedInt() const { return VM_INT(vector); }

    v4uu floor() const
    {
        VM_P_FLOOR();
        v4uu result = VM_FLOOR(vector);
        VM_E_FLOOR();
        return result;
    }

    /// Returns the integer part of this float, this becomes the
    /// 0..1 fractional component.
    v4uu splitFloat()
    {
        v4uu base = toSignedInt();
        *this -= base.toFloat();
        return base;
    }

#ifdef __SSE__
    template <int A, int B, int C, int D>
    SYS_FORCE_INLINE v4uf swizzle() const
    { 
        return VM_SHUFFLE<A,B,C,D>(vector);
    }
#endif

    SYS_FORCE_INLINE v4uu isFinite() const
    {
        // If the exponent is the maximum value, it's either infinite or NaN.
        const v4si mask = VM_SPLATS(0x7F800000);
        return ~v4uu(VM_ICMPEQ(VM_AND(V4SI(vector), mask), mask));
    }

public:
    v4sf vector;
};

SYS_FORCE_INLINE v4uf
v4uu::toFloat() const
{
    return v4uf(VM_IFLOAT(vector));
}

//
// Custom vector operations
//

static SYS_FORCE_INLINE v4uf
sqrt(const v4uf &a)
{
    return v4uf(VM_SQRT(a.vector));
}

static SYS_FORCE_INLINE v4uf
fabs(const v4uf &a)
{
    return a.abs();
}

// Use this operation to mask disabled values to 0
// rval = !a ? b : 0;

static SYS_FORCE_INLINE v4uf
andn(const v4uu &a, const v4uf &b)
{
    return v4uf(V4SF(VM_ANDNOT(a.vector, V4SI(b.vector))));
}

static SYS_FORCE_INLINE v4uu
andn(const v4uu &a, const v4uu &b)
{
    return v4uu(VM_ANDNOT(a.vector, b.vector));
}

// rval = a ? b : c;
static SYS_FORCE_INLINE v4uf
ternary(const v4uu &a, const v4uf &b, const v4uf &c)
{
    return (b & a) | andn(a, c);
}

static SYS_FORCE_INLINE v4uu
ternary(const v4uu &a, const v4uu &b, const v4uu &c)
{
    return (b & a) | andn(a, c);
}

// rval = !(a && b)
static SYS_FORCE_INLINE v4uu
nand(const v4uu &a, const v4uu &b)
{
    return !v4uu(VM_AND(a.vector, b.vector));
}

static SYS_FORCE_INLINE v4uf
vmin(const v4uf &a, const v4uf &b)
{
    return v4uf(VM_MIN(a.vector, b.vector));
}

static SYS_FORCE_INLINE v4uf
vmax(const v4uf &a, const v4uf &b)
{
    return v4uf(VM_MAX(a.vector, b.vector));
}

static SYS_FORCE_INLINE v4uf
clamp(const v4uf &a, const v4uf &b, const v4uf &c)
{
    return vmax(vmin(a, c), b);
}

static SYS_FORCE_INLINE v4uf
clamp(const v4uf &a, float b, float c)
{
    return vmax(vmin(a, v4uf(c)), v4uf(b));
}

static SYS_FORCE_INLINE bool
allbits(const v4uu &a)
{
    return vm_allbits(a.vector);
}

static SYS_FORCE_INLINE bool
anybits(const v4uu &a)
{
    return !allbits(~a);
}

static SYS_FORCE_INLINE v4uf
madd(const v4uf &v, const v4uf &f, const v4uf &a)
{
    return v4uf(VM_MADD(v.vector, f.vector, a.vector));
}

static SYS_FORCE_INLINE v4uf
madd(const v4uf &v, float f, float a)
{
    return v4uf(VM_MADD(v.vector, v4uf(f).vector, v4uf(a).vector));
}

static SYS_FORCE_INLINE v4uf
madd(const v4uf &v, float f, const v4uf &a)
{
    return v4uf(VM_MADD(v.vector, v4uf(f).vector, a.vector));
}

static SYS_FORCE_INLINE v4uf
msub(const v4uf &v, const v4uf &f, const v4uf &s)
{
    return madd(v, f, -s);
}

static SYS_FORCE_INLINE v4uf
msub(const v4uf &v, float f, float s)
{
    return madd(v, f, -s);
}

static SYS_FORCE_INLINE v4uf
lerp(const v4uf &a, const v4uf &b, const v4uf &w)
{
    v4uf w1 = v4uf(1.0F) - w;
    return madd(a, w1, b*w);
}

static SYS_FORCE_INLINE v4uf
luminance(const v4uf &r, const v4uf &g, const v4uf &b,
    float rw, float gw, float bw)
{
    return v4uf(madd(r, v4uf(rw), madd(g, v4uf(gw), b * bw)));
}

static SYS_FORCE_INLINE float
dot3(const v4uf &a, const v4uf &b)
{
    v4uf res = a*b;
    return res[0] + res[1] + res[2];
}

static SYS_FORCE_INLINE float
dot4(const v4uf &a, const v4uf &b)
{
    v4uf res = a*b;
    return res[0] + res[1] + res[2] + res[3];
}

static SYS_FORCE_INLINE float
length(const v4uf &a)
{
    return SYSsqrt(dot3(a, a));
}

static SYS_FORCE_INLINE v4uf
normalize(const v4uf &a)
{
    return a / length(a);
}

static SYS_FORCE_INLINE v4uf
cross(const v4uf &a, const v4uf &b)
{
    return v4uf(a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0], 0);
}

// Currently there is no specific support for signed integers
typedef v4uu v4ui;

// Assuming that ptr is an array of elements of type STYPE, this operation
// will return the index of the first element that is aligned to (1<<ASIZE)
// bytes.
#define VM_ALIGN(ptr, ASIZE, STYPE)	\
		((((1<<ASIZE)-(intptr_t)ptr)&((1<<ASIZE)-1))/sizeof(STYPE))

}}
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      This is the array class implementation used by almost everything here.
 */

#pragma once

#ifndef __UT_ARRAY_H_INCLUDED__
#define __UT_ARRAY_H_INCLUDED__



#include <algorithm>
#include <functional>
#include <type_traits>
#include <string.h>

namespace igl {
  /// @private
  namespace FastWindingNumber {

 /// This routine describes how to change the size of an array.
 /// It must increase the current_size by at least one!
 ///
 /// Current expected sequence of small sizes:
 ///    4,    8,   16,   32,   48,   64,   80,   96,  112,
 ///  128,  256,  384,  512,  640,  768,  896, 1024,
 /// (increases by approx factor of 1.125 each time after this)
template <typename T>
static inline T
UTbumpAlloc(T current_size)
{
    // NOTE: These must be powers of two.  See below.
    constexpr T SMALL_ALLOC(16);
    constexpr T BIG_ALLOC(128);

    // For small values, we increment by fixed amounts.  For
    // large values, we increment by one eighth of the current size.
    // This prevents n^2 behaviour with allocation one element at a time.
    // A factor of 1/8 will waste 1/16 the memory on average, and will
    // double the size of the array in approximately 6 reallocations.
    if (current_size < T(8))
    {
        return (current_size < T(4)) ? T(4) : T(8);
    }
    if (current_size < T(BIG_ALLOC))
    {
        // Snap up to next multiple of SMALL_ALLOC (must be power of 2)
        return (current_size + T(SMALL_ALLOC)) & ~T(SMALL_ALLOC-1);
    }
    if (current_size < T(BIG_ALLOC * 8))
    {
        // Snap up to next multiple of BIG_ALLOC (must be power of 2)
        return (current_size + T(BIG_ALLOC)) & ~T(BIG_ALLOC-1);
    }

    T bump = current_size >> 3; // Divided by 8.
    current_size += bump;
    return current_size;
}

template <typename T>
class UT_Array
{
public:
    typedef T value_type;

    typedef int (*Comparator)(const T *, const T *);

    /// Copy constructor. It duplicates the data.
    /// It's marked explicit so that it's not accidentally passed by value.
    /// You can always pass by reference and then copy it, if needed.
    /// If you have a line like:
    /// UT_Array<int> a = otherarray;
    /// and it really does need to copy instead of referencing,
    /// you can rewrite it as:
    /// UT_Array<int> a(otherarray);
    inline explicit UT_Array(const UT_Array<T> &a);
    
    /// Move constructor. Steals the working data from the original.
    inline UT_Array(UT_Array<T> &&a) noexcept;
    
    /// Construct based on given capacity and size
    UT_Array(exint capacity, exint size)
    {
        myData = capacity ? allocateCapacity(capacity) : NULL;
        if (capacity < size)
            size = capacity;
        mySize = size;
        myCapacity = capacity;
        trivialConstructRange(myData, mySize);
    }

    /// Construct based on given capacity with a size of 0
    explicit UT_Array(exint capacity = 0) : myCapacity(capacity), mySize(0)
    {
        myData = capacity ? allocateCapacity(capacity) : NULL;
    }

    /// Construct with the contents of an initializer list
    inline explicit UT_Array(std::initializer_list<T> init);

    inline ~UT_Array();

    inline void	    swap(UT_Array<T> &other);

    /// Append an element to the current elements and return its index in the
    /// array, or insert the element at a specified position; if necessary,
    /// insert() grows the array to accommodate the element. The insert
    /// methods use the assignment operator '=' to place the element into the
    /// right spot; be aware that '=' works differently on objects and pointers.
    /// The test for duplicates uses the logical equal operator '=='; as with
    /// '=', the behaviour of the equality operator on pointers versus objects
    /// is not the same.
    /// Use the subscript operators instead of insert() if you are appending
    /// to the array, or if  you don't mind overwriting the element already 
    /// inserted at the given index.
    exint           append(void) { return insert(mySize); }
    exint           append(const T &t) { return appendImpl(t); }
    exint           append(T &&t) { return appendImpl(std::move(t)); }
    inline void            append(const T *pt, exint count);
    inline void	    appendMultiple(const T &t, exint count);
    inline exint	    insert(exint index);
    exint	    insert(const T &t, exint i)
                        { return insertImpl(t, i); }
    exint	    insert(T &&t, exint i)
                        { return insertImpl(std::move(t), i); }

    /// Adds a new element to the array (resizing if necessary) and forwards
    /// the given arguments to T's constructor.
    /// NOTE: Unlike append(), the arguments cannot reference any existing
    /// elements in the array. Checking for and handling such cases would
    /// remove most of the performance gain versus append(T(...)). Debug builds
    /// will assert that the arguments are valid.
    template <typename... S>
    inline exint	    emplace_back(S&&... s);

    /// Takes another T array and concatenate it onto my end
    inline exint	    concat(const UT_Array<T> &a);

    /// Insert an element "count" times at the given index. Return the index.
    inline exint	    multipleInsert(exint index, exint count);

    /// An alias for unique element insertion at a certain index. Also used by
    /// the other insertion methods.
    exint	    insertAt(const T &t, exint index)
                        { return insertImpl(t, index); }

    /// Return true if given index is valid.
    bool	    isValidIndex(exint index) const
			{ return (index >= 0 && index < mySize); }

    /// Remove one element from the array given its
    /// position in the list, and fill the gap by shifting the elements down
    /// by one position.  Return the index of the element removed or -1 if
    /// the index was out of bounds.
    exint	    removeIndex(exint index)
    {
        return isValidIndex(index) ? removeAt(index) : -1;
    }
    void	    removeLast()
    {
        if (mySize) removeAt(mySize-1);
    }

    /// Remove the range [begin_i,end_i) of elements from the array.
    inline void	    removeRange(exint begin_i, exint end_i);

    /// Remove the range [begin_i, end_i) of elements from this array and place
    /// them in the dest array, shrinking/growing the dest array as necessary.
    inline void            extractRange(exint begin_i, exint end_i,
                                 UT_Array<T>& dest);

    /// Removes all matching elements from the list, shuffling down and changing
    /// the size appropriately.
    /// Returns the number of elements left.
    template <typename IsEqual>
    inline exint	    removeIf(IsEqual is_equal);

    /// Remove all matching elements. Also sets the capacity of the array.
    template <typename IsEqual>
    void	    collapseIf(IsEqual is_equal)
    {
        removeIf(is_equal);
        setCapacity(size());
    }

    /// Move howMany objects starting at index srcIndex to destIndex;
    /// This method will remove the elements at [srcIdx, srcIdx+howMany) and
    /// then insert them at destIdx.  This method can be used in place of
    /// the old shift() operation.
    inline void	    move(exint srcIdx, exint destIdx, exint howMany);

    /// Cyclically shifts the entire array by howMany
    inline void	    cycle(exint howMany);

    /// Quickly set the array to a single value.
    inline void	    constant(const T &v);
    /// Zeros the array if a POD type, else trivial constructs if a class type.
    inline void	    zero();

    /// The fastest search possible, which does pointer arithmetic to find the
    /// index of the element. WARNING: index() does no out-of-bounds checking.
    exint	    index(const T &t) const { return &t - myData; }
    exint	    safeIndex(const T &t) const
    {
        return (&t >= myData && &t < (myData + mySize))
            ? &t - myData : -1;
    }

    /// Set the capacity of the array, i.e. grow it or shrink it. The
    /// function copies the data after reallocating space for the array.
    inline void            setCapacity(exint newcapacity);
    void            setCapacityIfNeeded(exint mincapacity)
    {
        if (capacity() < mincapacity)
            setCapacity(mincapacity);
    }
    /// If the capacity is smaller than mincapacity, expand the array
    /// to at least mincapacity and to at least a constant factor of the
    /// array's previous capacity, to avoid having a linear number of
    /// reallocations in a linear number of calls to bumpCapacity.
    void            bumpCapacity(exint mincapacity)
    {
        if (capacity() >= mincapacity)
            return;
        // The following 4 lines are just
        // SYSmax(mincapacity, UTbumpAlloc(capacity())), avoiding SYSmax
        exint bumped = UTbumpAlloc(capacity());
        exint newcapacity = mincapacity;
        if (bumped > mincapacity)
            newcapacity = bumped;
        setCapacity(newcapacity);
    }

    /// First bumpCapacity to ensure that there's space for newsize,
    /// expanding either not at all or by at least a constant factor
    /// of the array's previous capacity,
    /// then set the size to newsize.
    void            bumpSize(exint newsize)
    {
        bumpCapacity(newsize);
        setSize(newsize);
    }
    /// NOTE: bumpEntries() will be deprecated in favour of bumpSize() in a
    ///       future version.
    void            bumpEntries(exint newsize)
    {
        bumpSize(newsize);
    }

    /// Query the capacity, i.e. the allocated length of the array.
    /// NOTE: capacity() >= size().
    exint           capacity() const { return myCapacity; }
    /// Query the size, i.e. the number of occupied elements in the array.
    /// NOTE: capacity() >= size().
    exint           size() const     { return mySize; }
    /// Alias of size().  size() is preferred.
    exint           entries() const  { return mySize; }
    /// Returns true iff there are no occupied elements in the array.
    bool            isEmpty() const  { return mySize==0; }

    /// Set the size, the number of occupied elements in the array.
    /// NOTE: This will not do bumpCapacity, so if you call this
    ///       n times to increase the size, it may take
    ///       n^2 time.
    void            setSize(exint newsize)
    {
        if (newsize < 0)
            newsize = 0;
        if (newsize == mySize)
            return;
        setCapacityIfNeeded(newsize);
        if (mySize > newsize)
            trivialDestructRange(myData + newsize, mySize - newsize);
        else // newsize > mySize
            trivialConstructRange(myData + mySize, newsize - mySize);
        mySize = newsize;
    }
    /// Alias of setSize().  setSize() is preferred.
    void            entries(exint newsize)
    {
        setSize(newsize);
    }
    /// Set the size, but unlike setSize(newsize), this function
    /// will not initialize new POD elements to zero. Non-POD data types
    /// will still have their constructors called.
    /// This function is faster than setSize(ne) if you intend to fill in
    /// data for all elements.
    void            setSizeNoInit(exint newsize)
    {
        if (newsize < 0)
            newsize = 0;
        if (newsize == mySize)
            return;
        setCapacityIfNeeded(newsize);
        if (mySize > newsize)
            trivialDestructRange(myData + newsize, mySize - newsize);
        else if (!isPOD()) // newsize > mySize
            trivialConstructRange(myData + mySize, newsize - mySize);
        mySize = newsize;
    }

    /// Decreases, but never expands, to the given maxsize.
    void            truncate(exint maxsize)
    {
        if (maxsize >= 0 && size() > maxsize)
            setSize(maxsize);
    }
    /// Resets list to an empty list.
    void            clear() {
        // Don't call setSize(0) since that would require a valid default
        // constructor.
        trivialDestructRange(myData, mySize);
        mySize = 0;
    }

    /// Assign array a to this array by copying each of a's elements with
    /// memcpy for POD types, and with copy construction for class types.
    inline UT_Array<T> &   operator=(const UT_Array<T> &a);

    /// Replace the contents with those from the initializer_list ilist
    inline UT_Array<T> &   operator=(std::initializer_list<T> ilist);

    /// Move the contents of array a to this array. 
    inline UT_Array<T> &   operator=(UT_Array<T> &&a);

    /// Compare two array and return true if they are equal and false otherwise.
    /// Two elements are checked against each other using operator '==' or
    /// compare() respectively.
    /// NOTE: The capacities of the arrays are not checked when
    ///       determining whether they are equal.
    inline bool            operator==(const UT_Array<T> &a) const;
    inline bool            operator!=(const UT_Array<T> &a) const;
     
    /// Subscript operator
    /// NOTE: This does NOT do any bounds checking unless paranoid
    ///       asserts are enabled.
    T &		    operator()(exint i)
    {
        UT_IGL_ASSERT_P(i >= 0 && i < mySize);
        return myData[i];
    }
    /// Const subscript operator
    /// NOTE: This does NOT do any bounds checking unless paranoid
    ///       asserts are enabled.
    const T &	    operator()(exint i) const
    {
	UT_IGL_ASSERT_P(i >= 0 && i < mySize);
	return myData[i];
    }

    /// Subscript operator
    /// NOTE: This does NOT do any bounds checking unless paranoid
    ///       asserts are enabled.
    T &		    operator[](exint i)
    {
        UT_IGL_ASSERT_P(i >= 0 && i < mySize);
        return myData[i];
    }
    /// Const subscript operator
    /// NOTE: This does NOT do any bounds checking unless paranoid
    ///       asserts are enabled.
    const T &	    operator[](exint i) const
    {
	UT_IGL_ASSERT_P(i >= 0 && i < mySize);
	return myData[i];
    }
    
    /// forcedRef(exint) will grow the array if necessary, initializing any
    /// new elements to zero for POD types and default constructing for
    /// class types.
    T &             forcedRef(exint i)
    {
        UT_IGL_ASSERT_P(i >= 0);
        if (i >= mySize)
            bumpSize(i+1);
        return myData[i];
    }

    /// forcedGet(exint) does NOT grow the array, and will return default
    /// objects for out of bound array indices.
    T               forcedGet(exint i) const
    {
        return (i >= 0 && i < mySize) ? myData[i] : T();
    }

    T &		    last()
    {
        UT_IGL_ASSERT_P(mySize);
        return myData[mySize-1];
    }
    const T &	    last() const
    {
        UT_IGL_ASSERT_P(mySize);
        return myData[mySize-1];
    }

    T *		    getArray() const		    { return myData; }
    const T *	    getRawArray() const		    { return myData; }

    T *		    array()			    { return myData; }
    const T *	    array() const		    { return myData; }

    T *		    data()			    { return myData; }
    const T *	    data() const		    { return myData; }

    /// This method allows you to swap in a new raw T array, which must be
    /// the same size as myCapacity. Use caution with this method.
    T *		    aliasArray(T *newdata)
    { T *data = myData; myData = newdata; return data; }

    template <typename IT, bool FORWARD>
    class base_iterator : 
	public std::iterator<std::random_access_iterator_tag, T, exint> 
    {
        public:
	    typedef IT&		reference;
	    typedef IT*		pointer;
	
	    // Note: When we drop gcc 4.4 support and allow range-based for
	    // loops, we should also drop atEnd(), which means we can drop
	    // myEnd here.
	    base_iterator() : myCurrent(NULL), myEnd(NULL) {}
	    
	      // Allow iterator to const_iterator conversion
	    template<typename EIT>
	    base_iterator(const base_iterator<EIT, FORWARD> &src)
		: myCurrent(src.myCurrent), myEnd(src.myEnd) {}
	    
	    pointer	operator->() const 
			{ return FORWARD ? myCurrent : myCurrent - 1; }
	    
	    reference	operator*() const
			{ return FORWARD ? *myCurrent : myCurrent[-1]; }

	    reference	item() const
			{ return FORWARD ? *myCurrent : myCurrent[-1]; }
	    
	    reference	operator[](exint n) const
			{ return FORWARD ? myCurrent[n] : myCurrent[-n - 1]; } 

	    /// Pre-increment operator
            base_iterator &operator++()
			{
        		    if (FORWARD) ++myCurrent; else --myCurrent;
			    return *this;
			}
	    /// Post-increment operator
	    base_iterator operator++(int)
			{
			    base_iterator tmp = *this;
        		    if (FORWARD) ++myCurrent; else --myCurrent;
        		    return tmp;
			}
	    /// Pre-decrement operator
	    base_iterator &operator--()
			{
			    if (FORWARD) --myCurrent; else ++myCurrent;
			    return *this;
			}
	    /// Post-decrement operator
	    base_iterator operator--(int)
			{
			    base_iterator tmp = *this;
			    if (FORWARD) --myCurrent; else ++myCurrent;
			    return tmp;
			}

	    base_iterator &operator+=(exint n)   
			{
			    if (FORWARD)
				myCurrent += n;
			    else
				myCurrent -= n;
			    return *this;
			}
            base_iterator operator+(exint n) const
			{
			    if (FORWARD)
				return base_iterator(myCurrent + n, myEnd);
			    else
				return base_iterator(myCurrent - n, myEnd);
			}
	    
            base_iterator &operator-=(exint n)
        		{ return (*this) += (-n); }
            base_iterator operator-(exint n) const
			{ return (*this) + (-n); }
            
	    bool	 atEnd() const		{ return myCurrent == myEnd; }
	    void	 advance()		{ this->operator++(); }
	    
	    // Comparators
	    template<typename ITR, bool FR>
	    bool 	 operator==(const base_iterator<ITR, FR> &r) const
			 { return myCurrent == r.myCurrent; }
	    
	    template<typename ITR, bool FR>
	    bool 	 operator!=(const base_iterator<ITR, FR> &r) const
			 { return myCurrent != r.myCurrent; }
	    
	    template<typename ITR>
	    bool	 operator<(const base_iterator<ITR, FORWARD> &r) const
	    {
		if (FORWARD) 
		    return myCurrent < r.myCurrent;
		else
		    return r.myCurrent < myCurrent;
	    }
	    
	    template<typename ITR>
	    bool	 operator>(const base_iterator<ITR, FORWARD> &r) const
	    {
		if (FORWARD) 
		    return myCurrent > r.myCurrent;
		else
		    return r.myCurrent > myCurrent;
	    }

	    template<typename ITR>
	    bool	 operator<=(const base_iterator<ITR, FORWARD> &r) const
	    {
		if (FORWARD) 
		    return myCurrent <= r.myCurrent;
		else
		    return r.myCurrent <= myCurrent;
	    }

	    template<typename ITR>
	    bool	 operator>=(const base_iterator<ITR, FORWARD> &r) const
	    {
		if (FORWARD) 
		    return myCurrent >= r.myCurrent;
		else
		    return r.myCurrent >= myCurrent;
	    }
	    
	    // Difference operator for std::distance
	    template<typename ITR>
	    exint	 operator-(const base_iterator<ITR, FORWARD> &r) const
	    {
		if (FORWARD) 
		    return exint(myCurrent - r.myCurrent);
		else
		    return exint(r.myCurrent - myCurrent);
	    }
	    
	    
        protected:
	    friend class UT_Array<T>;
	    base_iterator(IT *c, IT *e) : myCurrent(c), myEnd(e) {}
	private:

	    IT			*myCurrent;
	    IT			*myEnd;
    };
    
    typedef base_iterator<T, true>		iterator;
    typedef base_iterator<const T, true>	const_iterator;
    typedef base_iterator<T, false>		reverse_iterator;
    typedef base_iterator<const T, false>	const_reverse_iterator;
    typedef const_iterator	traverser; // For backward compatibility

    /// Begin iterating over the array.  The contents of the array may be 
    /// modified during the traversal.
    iterator		begin()
			{
			    return iterator(myData, myData + mySize);
			}
    /// End iterator.
    iterator		end()
			{
			    return iterator(myData + mySize,
					    myData + mySize);
			}

    /// Begin iterating over the array.  The array may not be modified during
    /// the traversal.
    const_iterator	begin() const
			{
			    return const_iterator(myData, myData + mySize);
			}
    /// End const iterator.  Consider using it.atEnd() instead.
    const_iterator	end() const
			{
			    return const_iterator(myData + mySize,
						  myData + mySize);
			}
    
    /// Begin iterating over the array in reverse. 
    reverse_iterator	rbegin()
			{
			    return reverse_iterator(myData + mySize,
			                            myData);
			}
    /// End reverse iterator.
    reverse_iterator	rend()
			{
			    return reverse_iterator(myData, myData);
			}
    /// Begin iterating over the array in reverse. 
    const_reverse_iterator rbegin() const
			{
			    return const_reverse_iterator(myData + mySize,
							  myData);
			}
    /// End reverse iterator.  Consider using it.atEnd() instead.
    const_reverse_iterator rend() const
			{
			    return const_reverse_iterator(myData, myData);
			}
    
    /// Remove item specified by the reverse_iterator.
    void		removeItem(const reverse_iterator &it)
			{
			    removeAt(&it.item() - myData);
			}


    /// Very dangerous methods to share arrays.
    /// The array is not aware of the sharing, so ensure you clear
    /// out the array prior a destructor or setCapacity operation.
    void	    unsafeShareData(UT_Array<T> &src)
		    {
			myData = src.myData;
			myCapacity = src.myCapacity;
			mySize = src.mySize;
		    }
    void	    unsafeShareData(T *src, exint srcsize)
		    {
			myData = src;
			myCapacity = srcsize;
			mySize = srcsize;
		    }
    void	    unsafeShareData(T *src, exint size, exint capacity)
		    {
			myData = src;
			mySize = size;
			myCapacity = capacity;
		    }
    void	    unsafeClearData()
		    {
			myData = NULL;
			myCapacity = 0;
			mySize = 0;
		    }

    /// Returns true if the data used by the array was allocated on the heap.
    inline bool	    isHeapBuffer() const
		    {
			return (myData != (T *)(((char*)this) + sizeof(*this)));
		    }
    inline bool	    isHeapBuffer(T* data) const
		    {
			return (data != (T *)(((char*)this) + sizeof(*this)));
		    }

protected:
    // Check whether T may have a constructor, destructor, or copy
    // constructor.  This test is conservative in that some POD types will
    // not be recognized as POD by this function. To mark your type as POD,
    // use the SYS_DECLARE_IS_POD() macro in SYS_TypeDecorate.h.
    static constexpr SYS_FORCE_INLINE bool isPOD()
    {
        return std::is_pod<T>::value;
    }

    /// Implements both append(const T &) and append(T &&) via perfect
    /// forwarding. Unlike the variadic emplace_back(), its argument may be a
    /// reference to another element in the array.
    template <typename S>
    inline exint           appendImpl(S &&s);

    /// Similar to appendImpl() but for insertion.
    template <typename S>
    inline exint           insertImpl(S &&s, exint index);

    // Construct the given type
    template <typename... S>
    static void	    construct(T &dst, S&&... s)
		    {
                        new (&dst) T(std::forward<S>(s)...);
		    }

    // Copy construct the given type
    static void	    copyConstruct(T &dst, const T &src)
		    {
			if (isPOD())
			    dst = src;
			else
			    new (&dst) T(src);
		    }
    static void	    copyConstructRange(T *dst, const T *src, exint n)
		    {
			if (isPOD())
                        {
                            if (n > 0)
                            {
                                ::memcpy((void *)dst, (const void *)src,
                                         n * sizeof(T));
                            }
                        }
			else
			{
			    for (exint i = 0; i < n; i++)
				new (&dst[i]) T(src[i]);
			}
		    }

    /// Element Constructor
    static void	    trivialConstruct(T &dst)
		    {
			if (!isPOD())
			    new (&dst) T();
			else
			    memset((void *)&dst, 0, sizeof(T));
		    }
    static void	    trivialConstructRange(T *dst, exint n)
		    {
			if (!isPOD())
			{
			    for (exint i = 0; i < n; i++)
				new (&dst[i]) T();
			}
			else if (n == 1)
			{
			    // Special case for n == 1. If the size parameter
			    // passed to memset is known at compile time, this
			    // function call will be inlined. This results in
			    // much faster performance than a real memset
			    // function call which is required in the case
			    // below, where n is not known until runtime.
			    // This makes calls to append() much faster.
			    memset((void *)dst, 0, sizeof(T));
			}
			else
			    memset((void *)dst, 0, sizeof(T) * n);
		    }

    /// Element Destructor
    static void	    trivialDestruct(T &dst)
		    {
			if (!isPOD())
			    dst.~T();
		    }
    static void	    trivialDestructRange(T *dst, exint n)
		    {
			if (!isPOD())
			{
			    for (exint i = 0; i < n; i++)
				dst[i].~T();
			}
		    }

private:
    /// Pointer to the array of elements of type T
    T *myData;

    /// The number of elements for which we have allocated memory
    exint myCapacity;

    /// The actual number of valid elements in the array
    exint mySize;

    // The guts of the remove() methods.
    inline exint	    removeAt(exint index);

    inline T *		    allocateCapacity(exint num_items);
};
}}



#endif // __UT_ARRAY_H_INCLUDED__
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      This is meant to be included by UT_Array.h and includes
 *      the template implementations needed by external code.
 */

#pragma once

#ifndef __UT_ARRAYIMPL_H_INCLUDED__
#define __UT_ARRAYIMPL_H_INCLUDED__




#include <algorithm>
#include <utility>
#include <stdlib.h>
#include <string.h>

namespace igl { 
  /// @private
  namespace FastWindingNumber {

// Implemented in UT_Array.C
extern void ut_ArrayImplFree(void *p);


template <typename T>
inline UT_Array<T>::UT_Array(const UT_Array<T> &a)
    : myCapacity(a.size()), mySize(a.size())
{
    if (myCapacity)
    {
	myData = allocateCapacity(myCapacity);
	copyConstructRange(myData, a.array(), mySize);
    }
    else
    {
	myData = nullptr;
    }
}

template <typename T>
inline UT_Array<T>::UT_Array(std::initializer_list<T> init)
    : myCapacity(init.size()), mySize(init.size())
{
    if (myCapacity)
    {
	myData = allocateCapacity(myCapacity);
	copyConstructRange(myData, init.begin(), mySize);
    }
    else
    {
	myData = nullptr;
    }
}

template <typename T>
inline UT_Array<T>::UT_Array(UT_Array<T> &&a) noexcept
{
    if (!a.isHeapBuffer())
    {
	myData = nullptr;
	myCapacity = 0;
	mySize = 0;
	operator=(std::move(a));
	return;
    }

    myCapacity = a.myCapacity;
    mySize = a.mySize;
    myData = a.myData;
    a.myCapacity = a.mySize = 0;
    a.myData = nullptr;
}


template <typename T>
inline UT_Array<T>::~UT_Array()
{
    // NOTE: We call setCapacity to ensure that we call trivialDestructRange,
    //       then call free on myData.
    setCapacity(0);
}

template <typename T>
inline T *
UT_Array<T>::allocateCapacity(exint capacity)
{
    T *data = (T *)malloc(capacity * sizeof(T));
    // Avoid degenerate case if we happen to be aliased the wrong way
    if (!isHeapBuffer(data))
    {
	T *prev = data;
	data = (T *)malloc(capacity * sizeof(T));
	ut_ArrayImplFree(prev);
    }
    return data;
}

template <typename T>
inline void
UT_Array<T>::swap( UT_Array<T> &other )
{
    std::swap( myData, other.myData );
    std::swap( myCapacity, other.myCapacity );
    std::swap( mySize, other.mySize );
}


template <typename T>
inline exint	
UT_Array<T>::insert(exint index)
{
    if (index >= mySize)
    {
	bumpCapacity(index + 1);

	trivialConstructRange(myData + mySize, index - mySize + 1);

	mySize = index+1;
	return index;
    }
    bumpCapacity(mySize + 1);

    UT_IGL_ASSERT_P(index >= 0);
    ::memmove((void *)&myData[index+1], (void *)&myData[index],
              ((mySize-index)*sizeof(T)));

    trivialConstruct(myData[index]);

    mySize++;
    return index;
}

template <typename T>
template <typename S>
inline exint
UT_Array<T>::appendImpl(S &&s)
{
    if (mySize == myCapacity)
    {
	exint idx = safeIndex(s);

        // NOTE: UTbumpAlloc always returns a strictly larger value.
	setCapacity(UTbumpAlloc(myCapacity));
        if (idx >= 0)
            construct(myData[mySize], std::forward<S>(myData[idx]));
        else
            construct(myData[mySize], std::forward<S>(s));
    }
    else
    {
	construct(myData[mySize], std::forward<S>(s));
    }
    return mySize++;
}

template <typename T>
template <typename... S>
inline exint
UT_Array<T>::emplace_back(S&&... s)
{
    if (mySize == myCapacity)
	setCapacity(UTbumpAlloc(myCapacity));

    construct(myData[mySize], std::forward<S>(s)...);
    return mySize++;
}

template <typename T>
inline void
UT_Array<T>::append(const T *pt, exint count)
{
    bumpCapacity(mySize + count);
    copyConstructRange(myData + mySize, pt, count);
    mySize += count;
}

template <typename T>
inline void
UT_Array<T>::appendMultiple(const T &t, exint count)
{
    UT_IGL_ASSERT_P(count >= 0);
    if (count <= 0)
	return;
    if (mySize + count >= myCapacity)
    {
	exint tidx = safeIndex(t);

        bumpCapacity(mySize + count);

	for (exint i = 0; i < count; i++)
	    copyConstruct(myData[mySize+i], tidx >= 0 ? myData[tidx] : t);
    }
    else
    {
	for (exint i = 0; i < count; i++)
	    copyConstruct(myData[mySize+i], t);
    }
    mySize += count;
}

template <typename T>
inline exint
UT_Array<T>::concat(const UT_Array<T> &a)
{
    bumpCapacity(mySize + a.mySize);
    copyConstructRange(myData + mySize, a.myData, a.mySize);
    mySize += a.mySize;

    return mySize;
}

template <typename T>
inline exint
UT_Array<T>::multipleInsert(exint beg_index, exint count)
{
    exint end_index = beg_index + count;

    if (beg_index >= mySize)
    {
	bumpCapacity(end_index);

	trivialConstructRange(myData + mySize, end_index - mySize);

	mySize = end_index;
	return beg_index;
    }
    bumpCapacity(mySize+count);

    ::memmove((void *)&myData[end_index], (void *)&myData[beg_index],
              ((mySize-beg_index)*sizeof(T)));
    mySize += count;

    trivialConstructRange(myData + beg_index, count);

    return beg_index;
}

template <typename T>
template <typename S>
inline exint
UT_Array<T>::insertImpl(S &&s, exint index)
{
    if (index == mySize)
    {
        // This case avoids an extraneous call to trivialConstructRange()
        // which the compiler may not optimize out.
        (void) appendImpl(std::forward<S>(s));
    }
    else if (index > mySize)
    {
	exint src_i = safeIndex(s);

	bumpCapacity(index + 1);

	trivialConstructRange(myData + mySize, index - mySize);

        if (src_i >= 0)
	    construct(myData[index], std::forward<S>(myData[src_i]));
        else
	    construct(myData[index], std::forward<S>(s));

	mySize = index + 1;
    }
    else // (index < mySize)
    {
	exint src_i = safeIndex(s);

        bumpCapacity(mySize + 1);

        ::memmove((void *)&myData[index+1], (void *)&myData[index],
                  ((mySize-index)*sizeof(T)));

        if (src_i >= index)
            ++src_i;

        if (src_i >= 0)
	    construct(myData[index], std::forward<S>(myData[src_i]));
        else
	    construct(myData[index], std::forward<S>(s));

        ++mySize;
    }

    return index;
}

template <typename T>
inline exint
UT_Array<T>::removeAt(exint idx)
{
    trivialDestruct(myData[idx]);
    if (idx != --mySize)
    {
	::memmove((void *)&myData[idx], (void *)&myData[idx+1],
	          ((mySize-idx)*sizeof(T)));
    }

    return idx;
}

template <typename T>
inline void
UT_Array<T>::removeRange(exint begin_i, exint end_i)
{
    UT_IGL_ASSERT(begin_i <= end_i);
    UT_IGL_ASSERT(end_i <= size());
    if (end_i < size())
    {
	trivialDestructRange(myData + begin_i, end_i - begin_i);
	::memmove((void *)&myData[begin_i], (void *)&myData[end_i],
	          (mySize - end_i)*sizeof(T));
    }
    setSize(mySize - (end_i - begin_i));
}

template <typename T>
inline void
UT_Array<T>::extractRange(exint begin_i, exint end_i, UT_Array<T>& dest)
{
    UT_IGL_ASSERT_P(begin_i >= 0);
    UT_IGL_ASSERT_P(begin_i <= end_i);
    UT_IGL_ASSERT_P(end_i <= size());
    UT_IGL_ASSERT(this != &dest);

    exint nelements = end_i - begin_i;

    // grow the raw array if necessary.
    dest.setCapacityIfNeeded(nelements);

    ::memmove((void*)dest.myData, (void*)&myData[begin_i],
              nelements * sizeof(T));
    dest.mySize = nelements;

    // we just asserted this was true, but just in case
    if (this != &dest)
    {
        if (end_i < size())
        {
            ::memmove((void*)&myData[begin_i], (void*)&myData[end_i],
                      (mySize - end_i) * sizeof(T));
        }
        setSize(mySize - nelements);
    }
}

template <typename T>
inline void
UT_Array<T>::move(exint srcIdx, exint destIdx, exint howMany)
{
    // Make sure all the parameters are valid.
    if( srcIdx < 0 )
	srcIdx = 0;
    if( destIdx < 0 )
	destIdx = 0;
    // If we are told to move a set of elements that would extend beyond the
    // end of the current array, trim the group.
    if( srcIdx + howMany > size() )
	howMany = size() - srcIdx;
    // If the destIdx would have us move the source beyond the end of the
    // current array, move the destIdx back.
    if( destIdx + howMany > size() )
	destIdx = size() - howMany;
    if( srcIdx != destIdx && howMany > 0 )
    {
	void		**tmp = 0;
	exint	  	savelen;

	savelen = SYSabs(srcIdx - destIdx);
	tmp = (void **)::malloc(savelen*sizeof(T));
	if( srcIdx > destIdx && howMany > 0 )
	{
	    // We're moving the group backwards. Save all the stuff that
	    // we would overwrite, plus everything beyond that to the
	    // start of the source group. Then move the source group, then
	    // tack the saved data onto the end of the moved group.
	    ::memcpy(tmp, (void *)&myData[destIdx],  (savelen*sizeof(T)));
	    ::memmove((void *)&myData[destIdx], (void *)&myData[srcIdx],
	              (howMany*sizeof(T)));
	    ::memcpy((void *)&myData[destIdx+howMany], tmp, (savelen*sizeof(T)));
	}
	if( srcIdx < destIdx && howMany > 0 )
	{
	    // We're moving the group forwards. Save from the end of the
	    // group being moved to the end of the where the destination
	    // group will end up. Then copy the source to the destination.
	    // Then move back up to the original source location and drop
	    // in our saved data.
	    ::memcpy(tmp, (void *)&myData[srcIdx+howMany],  (savelen*sizeof(T)));
	    ::memmove((void *)&myData[destIdx], (void *)&myData[srcIdx],
	              (howMany*sizeof(T)));
	    ::memcpy((void *)&myData[srcIdx], tmp, (savelen*sizeof(T)));
	}
	::free(tmp);
    }
}

template <typename T>
template <typename IsEqual>
inline exint
UT_Array<T>::removeIf(IsEqual is_equal)
{
    // Move dst to the first element to remove.
    exint dst;
    for (dst = 0; dst < mySize; dst++)
    {
	if (is_equal(myData[dst]))
	    break;
    }
    // Now start looking at all the elements past the first one to remove.
    for (exint idx = dst+1; idx < mySize; idx++)
    {
	if (!is_equal(myData[idx]))
	{
	    UT_IGL_ASSERT(idx != dst);
	    myData[dst] = myData[idx];
	    dst++;
	}
	// On match, ignore.
    }
    // New size
    mySize = dst;
    return mySize;
}

template <typename T>
inline void
UT_Array<T>::cycle(exint howMany)
{
    char	*tempPtr;
    exint	 numShift;	//  The number of items we shift
    exint   	 remaining;	//  mySize - numShift

    if (howMany == 0 || mySize < 1) return;

    numShift = howMany % (exint)mySize;
    if (numShift < 0) numShift += mySize;
    remaining = mySize - numShift;
    tempPtr = new char[numShift*sizeof(T)];

    ::memmove(tempPtr, (void *)&myData[remaining], (numShift * sizeof(T)));
    ::memmove((void *)&myData[numShift], (void *)&myData[0], (remaining * sizeof(T)));
    ::memmove((void *)&myData[0], tempPtr, (numShift * sizeof(T)));

    delete [] tempPtr;
}

template <typename T>
inline void
UT_Array<T>::constant(const T &value)
{
    for (exint i = 0; i < mySize; i++)
    {
	myData[i] = value;
    }
}

template <typename T>
inline void
UT_Array<T>::zero()
{
    if (isPOD())
	::memset((void *)myData, 0, mySize*sizeof(T));
    else
	trivialConstructRange(myData, mySize);
}

template <typename T>
inline void		
UT_Array<T>::setCapacity(exint capacity)
{
    // Do nothing when new capacity is the same as the current
    if (capacity == myCapacity)
	return;

    // Special case for non-heap buffers
    if (!isHeapBuffer())
    {
	if (capacity < mySize)
	{
	    // Destroy the extra elements without changing myCapacity
	    trivialDestructRange(myData + capacity, mySize - capacity);
	    mySize = capacity;
	}
	else if (capacity > myCapacity)
	{
	    T *prev = myData;
	    myData = (T *)malloc(sizeof(T) * capacity);
	    // myData is safe because we're already a stack buffer
	    UT_IGL_ASSERT_P(isHeapBuffer());
	    if (mySize > 0)
		memcpy((void *)myData, (void *)prev, sizeof(T) * mySize);
	    myCapacity = capacity;
	}
	else 
	{
	    // Keep myCapacity unchanged in this case
	    UT_IGL_ASSERT_P(capacity >= mySize && capacity <= myCapacity);
	}
	return;
    }

    if (capacity == 0)
    {
	if (myData)
	{
	    trivialDestructRange(myData, mySize);
	    free(myData);
	}
	myData     = 0;
	myCapacity = 0;
        mySize = 0;
	return;
    }

    if (capacity < mySize)
    {
	trivialDestructRange(myData + capacity, mySize - capacity);
	mySize = capacity;
    }

    if (myData)
	myData = (T *)realloc(myData, capacity*sizeof(T));
    else
	myData = (T *)malloc(sizeof(T) * capacity);

    // Avoid degenerate case if we happen to be aliased the wrong way
    if (!isHeapBuffer())
    {
	T *prev = myData;
	myData = (T *)malloc(sizeof(T) * capacity);
	if (mySize > 0)
	    memcpy((void *)myData, (void *)prev, sizeof(T) * mySize);
	ut_ArrayImplFree(prev);
    }

    myCapacity = capacity;
    UT_IGL_ASSERT(myData);
}

template <typename T>
inline UT_Array<T> &
UT_Array<T>::operator=(const UT_Array<T> &a)
{
    if (this == &a)
	return *this;

    // Grow the raw array if necessary.
    setCapacityIfNeeded(a.size());

    // Make sure destructors and constructors are called on all elements
    // being removed/added.
    trivialDestructRange(myData, mySize);
    copyConstructRange(myData, a.myData, a.size());

    mySize = a.size();

    return *this;
}

template <typename T>
inline UT_Array<T> &
UT_Array<T>::operator=(std::initializer_list<T> a)
{
    const exint new_size = a.size();

    // Grow the raw array if necessary.
    setCapacityIfNeeded(new_size);

    // Make sure destructors and constructors are called on all elements
    // being removed/added.
    trivialDestructRange(myData, mySize);

    copyConstructRange(myData, a.begin(), new_size);

    mySize = new_size;

    return *this;
}

template <typename T>
inline UT_Array<T> &
UT_Array<T>::operator=(UT_Array<T> &&a)
{
    if (!a.isHeapBuffer())
    {
	// Cannot steal from non-heap buffers
	clear();
	const exint n = a.size();
	setCapacityIfNeeded(n);
	if (isPOD())
	{
	    if (n > 0)
		memcpy(myData, a.myData, n * sizeof(T));
	}
	else
	{
	    for (exint i = 0; i < n; ++i)
		new (&myData[i]) T(std::move(a.myData[i]));
	}
	mySize = a.mySize;
	a.mySize = 0;
	return *this;
    }
    // else, just steal even if we're a small buffer

    // Destroy all the elements we're currently holding.
    if (myData)
    {
	trivialDestructRange(myData, mySize);
	if (isHeapBuffer())
	    ::free(myData);
    }
    
    // Move the contents of the other array to us and empty the other container
    // so that it destructs cleanly.
    myCapacity = a.myCapacity;
    mySize = a.mySize;
    myData = a.myData;
    a.myCapacity = a.mySize = 0;
    a.myData = nullptr;

    return *this;
}


template <typename T>
inline bool
UT_Array<T>::operator==(const UT_Array<T> &a) const
{
    if (this == &a) return true;
    if (mySize != a.size()) return false;
    for (exint i = 0; i < mySize; i++)
	if (!(myData[i] == a(i))) return false;
    return true;
}

template <typename T>
inline bool
UT_Array<T>::operator!=(const UT_Array<T> &a) const
{
    return (!operator==(a));
}

}}

#endif // __UT_ARRAYIMPL_H_INCLUDED__
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Special case for arrays that are usually small,
 *      to avoid a heap allocation when the array really is small.
 */

#pragma once

#ifndef __UT_SMALLARRAY_H_INCLUDED__
#define __UT_SMALLARRAY_H_INCLUDED__



#include <utility>
#include <stddef.h>
namespace igl {
  /// @private
  namespace FastWindingNumber {

/// An array class with the small buffer optimization, making it ideal for
/// cases when you know it will only contain a few elements at the expense of
/// increasing the object size by MAX_BYTES (subject to alignment).
template <typename T, size_t MAX_BYTES = 64>
class UT_SmallArray : public UT_Array<T>
{
    // As many elements that fit into MAX_BYTES with 1 item minimum
    enum { MAX_ELEMS = MAX_BYTES/sizeof(T) < 1 ? 1 : MAX_BYTES/sizeof(T) };

public:

// gcc falsely warns about our use of offsetof() on non-POD types. We can't
// easily suppress this because it has to be done in the caller at
// instantiation time. Instead, punt to a runtime check instead.
#if defined(__clang__) || defined(_MSC_VER)
    #define UT_SMALL_ARRAY_SIZE_IGL_ASSERT()    \
        using ThisT = UT_SmallArray<T,MAX_BYTES>; \
	static_assert(offsetof(ThisT, myBuffer) == sizeof(UT_Array<T>), \
            "In order for UT_Array's checks for whether it needs to free the buffer to work, " \
            "the buffer must be exactly following the base class memory.")
#else
    #define UT_SMALL_ARRAY_SIZE_IGL_ASSERT()    \
	UT_IGL_ASSERT_P(!UT_Array<T>::isHeapBuffer());
#endif

    /// Default construction
    UT_SmallArray()
	: UT_Array<T>(/*capacity*/0)
    {
	UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
	UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
    }
    
    /// Copy constructor
    /// @{
    explicit UT_SmallArray(const UT_Array<T> &copy)
	: UT_Array<T>(/*capacity*/0)
    {
	UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
	UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
	UT_Array<T>::operator=(copy);
    }
    explicit UT_SmallArray(const UT_SmallArray<T,MAX_BYTES> &copy)
	: UT_Array<T>(/*capacity*/0)
    {
	UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
	UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
	UT_Array<T>::operator=(copy);
    }
    /// @}

    /// Move constructor
    /// @{
    UT_SmallArray(UT_Array<T> &&movable) noexcept
    {
	UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
	UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
	UT_Array<T>::operator=(std::move(movable));
    }
    UT_SmallArray(UT_SmallArray<T,MAX_BYTES> &&movable) noexcept
    {
	UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
	UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
	UT_Array<T>::operator=(std::move(movable));
    }
    /// @}

    /// Initializer list constructor
    explicit UT_SmallArray(std::initializer_list<T> init)
    {
        UT_Array<T>::unsafeShareData((T*)myBuffer, 0, MAX_ELEMS);
        UT_SMALL_ARRAY_SIZE_IGL_ASSERT();
        UT_Array<T>::operator=(init);
    }

#undef UT_SMALL_ARRAY_SIZE_IGL_ASSERT

    /// Assignment operator
    /// @{
    UT_SmallArray<T,MAX_BYTES> &
    operator=(const UT_SmallArray<T,MAX_BYTES> &copy)
    {
	UT_Array<T>::operator=(copy);
	return *this;
    }
    UT_SmallArray<T,MAX_BYTES> &
    operator=(const UT_Array<T> &copy)
    {
	UT_Array<T>::operator=(copy);
	return *this;
    }
    /// @}

    /// Move operator
    /// @{
    UT_SmallArray<T,MAX_BYTES> &
    operator=(UT_SmallArray<T,MAX_BYTES> &&movable)
    {
	UT_Array<T>::operator=(std::move(movable));
	return *this;
    }
    UT_SmallArray<T,MAX_BYTES> &
    operator=(UT_Array<T> &&movable)
    {
        UT_Array<T>::operator=(std::move(movable));
        return *this;
    }
    /// @}

    UT_SmallArray<T,MAX_BYTES> &
    operator=(std::initializer_list<T> src)
    {
        UT_Array<T>::operator=(src);
        return *this;
    }
private:
    alignas(T) char myBuffer[MAX_ELEMS*sizeof(T)];
};
}}

#endif // __UT_SMALLARRAY_H_INCLUDED__
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      A vector class templated on its size and data type.
 */

#pragma once

#ifndef __UT_FixedVector__
#define __UT_FixedVector__




namespace igl {
  /// @private
  namespace FastWindingNumber {

template<typename T,exint SIZE,bool INSTANTIATED=false>
class UT_FixedVector
{
public:
    typedef UT_FixedVector<T,SIZE,INSTANTIATED> ThisType;
    typedef T value_type;
    typedef T theType;
    static const exint theSize = SIZE;

    T vec[SIZE];

    SYS_FORCE_INLINE UT_FixedVector() = default;

    /// Initializes every component to the same value
    SYS_FORCE_INLINE explicit UT_FixedVector(T that) noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = that;
    }

    SYS_FORCE_INLINE UT_FixedVector(const ThisType &that) = default;
    SYS_FORCE_INLINE UT_FixedVector(ThisType &&that) = default;

    /// Converts vector of S into vector of T,
    /// or just copies if same type.
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE UT_FixedVector(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = that[i];
    }

    template<typename S>
    SYS_FORCE_INLINE UT_FixedVector(const S that[SIZE]) noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = that[i];
    }

    SYS_FORCE_INLINE const T &operator[](exint i) const noexcept
    {
        UT_IGL_ASSERT_P(i >= 0 && i < SIZE);
        return vec[i];
    }
    SYS_FORCE_INLINE T &operator[](exint i) noexcept
    {
        UT_IGL_ASSERT_P(i >= 0 && i < SIZE);
        return vec[i];
    }

    SYS_FORCE_INLINE constexpr const T *data() const noexcept
    {
        return vec;
    }
    SYS_FORCE_INLINE T *data() noexcept
    {
        return vec;
    }

    SYS_FORCE_INLINE ThisType &operator=(const ThisType &that) = default;
    SYS_FORCE_INLINE ThisType &operator=(ThisType &&that) = default;

    template <typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE ThisType &operator=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = that[i];
        return *this;
    }
    SYS_FORCE_INLINE const ThisType &operator=(T that) noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = that;
        return *this;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE void operator+=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] += that[i];
    }
    SYS_FORCE_INLINE void operator+=(T that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] += that;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto operator+(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> UT_FixedVector<decltype(vec[0]+that[0]),SIZE>
    {
        using Type = decltype(vec[0]+that[0]);
        UT_FixedVector<Type,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = vec[i] + that[i];
        return result;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE void operator-=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] -= that[i];
    }
    SYS_FORCE_INLINE void operator-=(T that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] -= that;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto operator-(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> UT_FixedVector<decltype(vec[0]-that[0]),SIZE>
    {
        using Type = decltype(vec[0]-that[0]);
        UT_FixedVector<Type,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = vec[i] - that[i];
        return result;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE void operator*=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] *= that[i];
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto operator*(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> UT_FixedVector<decltype(vec[0]*that[0]),SIZE>
    {
        using Type = decltype(vec[0]*that[0]);
        UT_FixedVector<Type,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = vec[i] * that[i];
        return result;
    }
    SYS_FORCE_INLINE void operator*=(T that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] *= that;
    }
    SYS_FORCE_INLINE UT_FixedVector<T,SIZE> operator*(T that) const
    {
        UT_FixedVector<T,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = vec[i] * that;
        return result;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE void operator/=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that)
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] /= that[i];
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto operator/(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> UT_FixedVector<decltype(vec[0]/that[0]),SIZE>
    {
        using Type = decltype(vec[0]/that[0]);
        UT_FixedVector<Type,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = vec[i] / that[i];
        return result;
    }

    SYS_FORCE_INLINE void operator/=(T that)
    {
        if (std::is_integral<T>::value)
        {
            for (exint i = 0; i < SIZE; ++i)
                vec[i] /= that;
        }
        else
        {
            that = 1/that;
            for (exint i = 0; i < SIZE; ++i)
                vec[i] *= that;
        }
    }
    SYS_FORCE_INLINE UT_FixedVector<T,SIZE> operator/(T that) const
    {
        UT_FixedVector<T,SIZE> result;
        if (std::is_integral<T>::value)
        {
            for (exint i = 0; i < SIZE; ++i)
                result[i] = vec[i] / that;
        }
        else
        {
            that = 1/that;
            for (exint i = 0; i < SIZE; ++i)
                result[i] = vec[i] * that;
        }
        return result;
    }
    SYS_FORCE_INLINE void negate()
    {
        for (exint i = 0; i < SIZE; ++i)
            vec[i] = -vec[i];
    }

    SYS_FORCE_INLINE UT_FixedVector<T,SIZE> operator-() const
    {
        UT_FixedVector<T,SIZE> result;
        for (exint i = 0; i < SIZE; ++i)
            result[i] = -vec[i];
        return result;
    }

    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE bool operator==(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
        {
            if (vec[i] != T(that[i]))
                return false;
        }
        return true;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE bool operator!=(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const noexcept
    {
        return !(*this==that);
    }
    SYS_FORCE_INLINE bool isZero() const noexcept
    {
        for (exint i = 0; i < SIZE; ++i)
        {
            if (vec[i] != T(0))
                return false;
        }
        return true;
    }
    SYS_FORCE_INLINE T maxComponent() const
    {
        T v = vec[0];
        for (exint i = 1; i < SIZE; ++i)
            v = (vec[i] > v) ? vec[i] : v;
        return v;
    }
    SYS_FORCE_INLINE T minComponent() const
    {
        T v = vec[0];
        for (exint i = 1; i < SIZE; ++i)
            v = (vec[i] < v) ? vec[i] : v;
        return v;
    }
    SYS_FORCE_INLINE T avgComponent() const
    {
        T v = vec[0];
        for (exint i = 1; i < SIZE; ++i)
            v += vec[i];
        return v / SIZE;
    }

    SYS_FORCE_INLINE T length2() const noexcept
    {
        T a0(vec[0]);
        T result(a0*a0);
        for (exint i = 1; i < SIZE; ++i)
        {
            T ai(vec[i]);
            result += ai*ai;
        }
        return result;
    }
    SYS_FORCE_INLINE T length() const
    {
        T len2 = length2();
        return SYSsqrt(len2);
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto dot(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> decltype(vec[0]*that[0])
    {
        using TheType = decltype(vec[0]*that.vec[0]);
        TheType result(vec[0]*that[0]);
        for (exint i = 1; i < SIZE; ++i)
            result += vec[i]*that[i];
        return result;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto distance2(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> decltype(vec[0]-that[0])
    {
        using TheType = decltype(vec[0]-that[0]);
        TheType v(vec[0] - that[0]);
        TheType result(v*v);
        for (exint i = 1; i < SIZE; ++i)
        {
            v = vec[i] - that[i];
            result += v*v;
        }
        return result;
    }
    template<typename S,bool S_INSTANTIATED>
    SYS_FORCE_INLINE auto distance(const UT_FixedVector<S,SIZE,S_INSTANTIATED> &that) const -> decltype(vec[0]-that[0])
    {
        auto dist2 = distance2(that);
        return SYSsqrt(dist2);
    }

    SYS_FORCE_INLINE T normalize()
    {
        T len2 = length2();
        if (len2 == T(0))
            return T(0);
        if (len2 == T(1))
            return T(1);
        T len = SYSsqrt(len2);
        // Check if the square root is equal 1.  sqrt(1+dx) ~ 1+dx/2,
        // so it may get rounded to 1 when it wasn't 1 before.
        if (len != T(1))
            (*this) /= len;
        return len;
    }
};

/// NOTE: Strictly speaking, this should use decltype(that*a[0]),
///       but in the interests of avoiding accidental precision escalation,
///       it uses T.
template<typename T,exint SIZE,bool INSTANTIATED,typename S>
SYS_FORCE_INLINE UT_FixedVector<T,SIZE> operator*(const S &that,const UT_FixedVector<T,SIZE,INSTANTIATED> &a)
{
    T t(that);
    UT_FixedVector<T,SIZE> result;
    for (exint i = 0; i < SIZE; ++i)
        result[i] = t * a[i];
    return result;
}

template<typename T, exint SIZE, bool INSTANTIATED, typename S, bool S_INSTANTIATED>
SYS_FORCE_INLINE auto
dot(const UT_FixedVector<T,SIZE,INSTANTIATED> &a, const UT_FixedVector<S,SIZE,S_INSTANTIATED> &b) -> decltype(a[0]*b[0])
{
    return a.dot(b);
}

template<typename T, exint SIZE, bool INSTANTIATED, typename S, bool S_INSTANTIATED>
SYS_FORCE_INLINE auto
SYSmin(const UT_FixedVector<T,SIZE,INSTANTIATED> &a, const UT_FixedVector<S,SIZE,S_INSTANTIATED> &b) -> UT_FixedVector<decltype(a[0]+b[1]), SIZE>
{
    using Type = decltype(a[0]+b[1]);
    UT_FixedVector<Type, SIZE> result;
    for (exint i = 0; i < SIZE; ++i)
        result[i] = SYSmin(Type(a[i]), Type(b[i]));
    return result;
}

template<typename T, exint SIZE, bool INSTANTIATED, typename S, bool S_INSTANTIATED>
SYS_FORCE_INLINE auto
SYSmax(const UT_FixedVector<T,SIZE,INSTANTIATED> &a, const UT_FixedVector<S,SIZE,S_INSTANTIATED> &b) -> UT_FixedVector<decltype(a[0]+b[1]), SIZE>
{
    using Type = decltype(a[0]+b[1]);
    UT_FixedVector<Type, SIZE> result;
    for (exint i = 0; i < SIZE; ++i)
        result[i] = SYSmax(Type(a[i]), Type(b[i]));
    return result;
}

template<typename T>
struct UT_FixedVectorTraits
{
    typedef UT_FixedVector<T,1> FixedVectorType;
    typedef T DataType;
    static const exint TupleSize = 1;
    static const bool isVectorType = false;
};

template<typename T,exint SIZE,bool INSTANTIATED>
struct UT_FixedVectorTraits<UT_FixedVector<T,SIZE,INSTANTIATED> >
{
    typedef UT_FixedVector<T,SIZE,INSTANTIATED> FixedVectorType;
    typedef T DataType;
    static const exint TupleSize = SIZE;
    static const bool isVectorType = true;
};
}}

#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Simple wrappers on tbb interface
 */

#ifndef __UT_ParallelUtil__
#define __UT_ParallelUtil__



#include <thread> // This is just included for std::thread::hardware_concurrency()
namespace igl {
  /// @private
  namespace FastWindingNumber {
namespace UT_Thread { inline int getNumProcessors() {
    return std::thread::hardware_concurrency();
}}

//#include "tbb/blocked_range.h"
//#include "tbb/parallel_for.h"
////namespace tbb { class split; }
//
///// Declare prior to use.
//template <typename T> 
//using UT_BlockedRange = tbb::blocked_range<T>;
//
//// Default implementation that calls range.size()
//template< typename RANGE >
//struct UT_EstimatorNumItems
//{
//    UT_EstimatorNumItems() {}
//
//    size_t operator()(const RANGE& range) const
//    {
//	return range.size();
//    }
//};
//
///// This is needed by UT_CoarsenedRange
//template <typename RANGE>
//inline size_t UTestimatedNumItems(const RANGE& range)
//{
//    return UT_EstimatorNumItems<RANGE>()(range);
//}
//
///// UT_CoarsenedRange: This should be used only inside 
///// UT_ParallelFor and UT_ParallelReduce
///// This class wraps an existing range with a new range.
///// This allows us to use simple_partitioner, rather than
///// auto_partitioner, which has disastrous performance with
///// the default grain size in ttb 4.
//template< typename RANGE >
//class UT_CoarsenedRange : public RANGE
//{
//public:
//    // Compiler-generated versions are fine:
//    // ~UT_CoarsenedRange();
//    // UT_CoarsenedRange(const UT_CoarsenedRange&);
//
//    // Split into two sub-ranges:
//    UT_CoarsenedRange(UT_CoarsenedRange& range, tbb::split spl) :
//        RANGE(range, spl),
//        myGrainSize(range.myGrainSize)
//    {        
//    }
//
//    // Inherited: bool empty() const
//
//    bool is_divisible() const
//    {
//        return 
//            RANGE::is_divisible() &&
//            (UTestimatedNumItems(static_cast<const RANGE&>(*this)) > myGrainSize);
//    }
//
//private:
//    size_t myGrainSize;
//
//    UT_CoarsenedRange(const RANGE& base_range, const size_t grain_size) :
//        RANGE(base_range),
//        myGrainSize(grain_size)
//    {        
//    }
//
//    template <typename Range, typename Body>
//    friend void UTparallelFor(
//        const Range &range, const Body &body,
//        const int subscribe_ratio, const int min_grain_size
//    );
//};
//
///// Run the @c body function over a range in parallel.
///// UTparallelFor attempts to spread the range out over at most 
///// subscribe_ratio * num_processor tasks.
///// The factor subscribe_ratio can be used to help balance the load.
///// UTparallelFor() uses tbb for its implementation.
///// The used grain size is the maximum of min_grain_size and
///// if UTestimatedNumItems(range) / (subscribe_ratio * num_processor).
///// If subscribe_ratio == 0, then a grain size of min_grain_size will be used.
///// A range can be split only when UTestimatedNumItems(range) exceeds the
///// grain size the range is divisible. 
//
/////
///// Requirements for the Range functor are:
/////   - the requirements of the tbb Range Concept
/////   - UT_estimatorNumItems<Range> must return the the estimated number of work items
/////     for the range. When Range::size() is not the correct estimate, then a 
/////     (partial) specialization of UT_estimatorNumItemsimatorRange must be provided
/////     for the type Range.
/////
///// Requirements for the Body function are:
/////  - @code Body(const Body &); @endcode @n
/////	Copy Constructor
/////  - @code Body()::~Body(); @endcode @n
/////	Destructor
/////  - @code void Body::operator()(const Range &range) const; @endcode
/////	Function call to perform operation on the range.  Note the operator is
/////	@b const.
/////
///// The requirements for a Range object are:
/////  - @code Range::Range(const Range&); @endcode @n
/////	Copy constructor
/////  - @code Range::~Range(); @endcode @n
/////	Destructor
/////  - @code bool Range::is_divisible() const; @endcode @n
/////	True if the range can be partitioned into two sub-ranges
/////  - @code bool Range::empty() const; @endcode @n
/////	True if the range is empty
/////  - @code Range::Range(Range &r, UT_Split) const; @endcode @n
/////	Split the range @c r into two sub-ranges (i.e. modify @c r and *this)
/////
///// Example: @code
/////     class Square {
/////     public:
/////         Square(double *data) : myData(data) {}
/////         ~Square();
/////         void operator()(const UT_BlockedRange<int64> &range) const
/////         {
/////             for (int64 i = range.begin(); i != range.end(); ++i)
/////                 myData[i] *= myData[i];
/////         }
/////         double *myData;
/////     };
/////     ...
/////
/////     void
/////     parallel_square(double *array, int64 length)
/////     {
/////         UTparallelFor(UT_BlockedRange<int64>(0, length), Square(array));
/////     }
///// @endcode
/////	
///// @see UTparallelReduce(), UT_BlockedRange()
//
//template <typename Range, typename Body>
//void UTparallelFor(
//    const Range &range, const Body &body,
//    const int subscribe_ratio = 2,
//    const int min_grain_size = 1
//)
//{
//    const size_t num_processors( UT_Thread::getNumProcessors() );
//
//    UT_IGL_ASSERT( num_processors >= 1 );
//    UT_IGL_ASSERT( min_grain_size >= 1 );
//    UT_IGL_ASSERT( subscribe_ratio >= 0 );
//
//    const size_t est_range_size( UTestimatedNumItems(range) );
//
//    // Don't run on an empty range!
//    if (est_range_size == 0)
//        return;
//
//    // Avoid tbb overhead if entire range needs to be single threaded
//    if (num_processors == 1 || est_range_size <= min_grain_size)
//    {
//        body(range);
//        return;
//    }
//
//    size_t grain_size(min_grain_size);
//    if( subscribe_ratio > 0 )
//        grain_size = std::max(
//                         grain_size, 
//                         est_range_size / (subscribe_ratio * num_processors)
//                     );
//
//    UT_CoarsenedRange< Range > coarsened_range(range, grain_size);
//
//    tbb::parallel_for(coarsened_range, body, tbb::simple_partitioner());
//}
//
///// Version of UTparallelFor that is tuned for the case where the range
///// consists of lightweight items, for example,
///// float additions or matrix-vector multiplications.
//template <typename Range, typename Body>
//void
//UTparallelForLightItems(const Range &range, const Body &body)
//{
//    UTparallelFor(range, body, 2, 1024);
//}
//
///// UTserialFor can be used as a debugging tool to quickly replace a parallel
///// for with a serial for.
//template <typename Range, typename Body>
//void UTserialFor(const Range &range, const Body &body)
//	{ body(range); }
//
}}
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Bounding Volume Hierarchy (BVH) implementation.
 *      To call functions not implemented here, also include UT_BVHImpl.h
 */

#pragma once

#ifndef __HDK_UT_BVH_h__
#define __HDK_UT_BVH_h__




#include <limits>
#include <memory>
namespace igl { 
  /// @private
  namespace FastWindingNumber {

template<typename T> class UT_Array;
class v4uf;
class v4uu;

namespace HDK_Sample {

namespace UT {

template<typename T,uint NAXES>
struct Box {
    T vals[NAXES][2];

    SYS_FORCE_INLINE Box() noexcept = default;
    SYS_FORCE_INLINE constexpr Box(const Box &other) noexcept = default;
    SYS_FORCE_INLINE constexpr Box(Box &&other) noexcept = default;
    SYS_FORCE_INLINE Box& operator=(const Box &other) noexcept = default;
    SYS_FORCE_INLINE Box& operator=(Box &&other) noexcept = default;

    template<typename S>
    SYS_FORCE_INLINE Box(const Box<S,NAXES>& other) noexcept {
        static_assert((std::is_pod<Box<T,NAXES>>::value) || !std::is_pod<T>::value,
            "UT::Box should be POD, for better performance in UT_Array, etc.");

        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = T(other.vals[axis][0]);
            vals[axis][1] = T(other.vals[axis][1]);
        }
    }
    template<typename S,bool INSTANTIATED>
    SYS_FORCE_INLINE Box(const UT_FixedVector<S,NAXES,INSTANTIATED>& pt) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = pt[axis];
            vals[axis][1] = pt[axis];
        }
    }
    template<typename S>
    SYS_FORCE_INLINE Box& operator=(const Box<S,NAXES>& other) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = T(other.vals[axis][0]);
            vals[axis][1] = T(other.vals[axis][1]);
        }
        return *this;
    }

    SYS_FORCE_INLINE const T* operator[](const size_t axis) const noexcept {
        UT_IGL_ASSERT_P(axis < NAXES);
        return vals[axis];
    }
    SYS_FORCE_INLINE T* operator[](const size_t axis) noexcept {
        UT_IGL_ASSERT_P(axis < NAXES);
        return vals[axis];
    }

    SYS_FORCE_INLINE void initBounds() noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = std::numeric_limits<T>::max();
            vals[axis][1] = -std::numeric_limits<T>::max();
        }
    }
    /// Copy the source box.
    /// NOTE: This is so that in templated code that may have a Box or a
    ///       UT_FixedVector, it can call initBounds and still work.
    SYS_FORCE_INLINE void initBounds(const Box<T,NAXES>& src) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = src.vals[axis][0];
            vals[axis][1] = src.vals[axis][1];
        }
    }
    /// Initialize with the union of the source boxes.
    /// NOTE: This is so that in templated code that may have Box's or a
    ///       UT_FixedVector's, it can call initBounds and still work.
    SYS_FORCE_INLINE void initBoundsUnordered(const Box<T,NAXES>& src0, const Box<T,NAXES>& src1) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = SYSmin(src0.vals[axis][0], src1.vals[axis][0]);
            vals[axis][1] = SYSmax(src0.vals[axis][1], src1.vals[axis][1]);
        }
    }
    SYS_FORCE_INLINE void combine(const Box<T,NAXES>& src) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            T& minv = vals[axis][0];
            T& maxv = vals[axis][1];
            const T curminv = src.vals[axis][0];
            const T curmaxv = src.vals[axis][1];
            minv = (minv < curminv) ? minv : curminv;
            maxv = (maxv > curmaxv) ? maxv : curmaxv;
        }
    }
    SYS_FORCE_INLINE void enlargeBounds(const Box<T,NAXES>& src) noexcept {
        combine(src);
    }

    template<typename S,bool INSTANTIATED>
    SYS_FORCE_INLINE
    void initBounds(const UT_FixedVector<S,NAXES,INSTANTIATED>& pt) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = pt[axis];
            vals[axis][1] = pt[axis];
        }
    }
    template<bool INSTANTIATED>
    SYS_FORCE_INLINE
    void initBounds(const UT_FixedVector<T,NAXES,INSTANTIATED>& min, const UT_FixedVector<T,NAXES,INSTANTIATED>& max) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = min[axis];
            vals[axis][1] = max[axis];
        }
    }
    template<bool INSTANTIATED>
    SYS_FORCE_INLINE
    void initBoundsUnordered(const UT_FixedVector<T,NAXES,INSTANTIATED>& p0, const UT_FixedVector<T,NAXES,INSTANTIATED>& p1) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = SYSmin(p0[axis], p1[axis]);
            vals[axis][1] = SYSmax(p0[axis], p1[axis]);
        }
    }
    template<bool INSTANTIATED>
    SYS_FORCE_INLINE
    void enlargeBounds(const UT_FixedVector<T,NAXES,INSTANTIATED>& pt) noexcept {
        for (uint axis = 0; axis < NAXES; ++axis) {
            vals[axis][0] = SYSmin(vals[axis][0], pt[axis]);
            vals[axis][1] = SYSmax(vals[axis][1], pt[axis]);
        }
    }

    SYS_FORCE_INLINE
    UT_FixedVector<T,NAXES> getMin() const noexcept {
        UT_FixedVector<T,NAXES> v;
        for (uint axis = 0; axis < NAXES; ++axis) {
            v[axis] = vals[axis][0];
        }
        return v;
    }

    SYS_FORCE_INLINE
    UT_FixedVector<T,NAXES> getMax() const noexcept {
        UT_FixedVector<T,NAXES> v;
        for (uint axis = 0; axis < NAXES; ++axis) {
            v[axis] = vals[axis][1];
        }
        return v;
    }

    T diameter2() const noexcept {
        T diff = (vals[0][1]-vals[0][0]);
        T sum = diff*diff;
        for (uint axis = 1; axis < NAXES; ++axis) {
            diff = (vals[axis][1]-vals[axis][0]);
            sum += diff*diff;
        }
        return sum;
    }
    T volume() const noexcept {
        T product = (vals[0][1]-vals[0][0]);
        for (uint axis = 1; axis < NAXES; ++axis) {
            product *= (vals[axis][1]-vals[axis][0]);
        }
        return product;
    }
    T half_surface_area() const noexcept {
        if (NAXES==1) {
            // NOTE: Although this should technically be 1,
            //       that doesn't make any sense as a heuristic,
            //       so we fall back to the "volume" of this box.
            return (vals[0][1]-vals[0][0]);
        }
        if (NAXES==2) {
            const T d0 = (vals[0][1]-vals[0][0]);
            const T d1 = (vals[1][1]-vals[1][0]);
            return d0 + d1;
        }
        if (NAXES==3) {
            const T d0 = (vals[0][1]-vals[0][0]);
            const T d1 = (vals[1][1]-vals[1][0]);
            const T d2 = (vals[2][1]-vals[2][0]);
            return d0*d1 + d1*d2 + d2*d0;
        }
        if (NAXES==4) {
            const T d0 = (vals[0][1]-vals[0][0]);
            const T d1 = (vals[1][1]-vals[1][0]);
            const T d2 = (vals[2][1]-vals[2][0]);
            const T d3 = (vals[3][1]-vals[3][0]);
            // This is just d0d1d2 + d1d2d3 + d2d3d0 + d3d0d1 refactored.
            const T d0d1 = d0*d1;
            const T d2d3 = d2*d3;
            return d0d1*(d2+d3) + d2d3*(d0+d1);
        }

        T sum = 0;
        for (uint skipped_axis = 0; skipped_axis < NAXES; ++skipped_axis) {
            T product = 1;
            for (uint axis = 0; axis < NAXES; ++axis) {
                if (axis != skipped_axis) {
                    product *= (vals[axis][1]-vals[axis][0]);
                }
            }
            sum += product;
        }
        return sum;
    }
    T axis_sum() const noexcept {
        T sum = (vals[0][1]-vals[0][0]);
        for (uint axis = 1; axis < NAXES; ++axis) {
            sum += (vals[axis][1]-vals[axis][0]);
        }
        return sum;
    }
    template<bool INSTANTIATED0,bool INSTANTIATED1>
    SYS_FORCE_INLINE void intersect(
        T &box_tmin,
        T &box_tmax,
        const UT_FixedVector<uint,NAXES,INSTANTIATED0> &signs,
        const UT_FixedVector<T,NAXES,INSTANTIATED1> &origin,
        const UT_FixedVector<T,NAXES,INSTANTIATED1> &inverse_direction
    ) const noexcept {
        for (int axis = 0; axis < NAXES; ++axis)
        {
            uint sign = signs[axis];
            T t1 = (vals[axis][sign]   - origin[axis]) * inverse_direction[axis];
            T t2 = (vals[axis][sign^1] - origin[axis]) * inverse_direction[axis];
            box_tmin = SYSmax(t1, box_tmin);
            box_tmax = SYSmin(t2, box_tmax);
        }
    }
    SYS_FORCE_INLINE void intersect(const Box& other, Box& dest) const noexcept {
        for (int axis = 0; axis < NAXES; ++axis)
        {
            dest.vals[axis][0] = SYSmax(vals[axis][0], other.vals[axis][0]);
            dest.vals[axis][1] = SYSmin(vals[axis][1], other.vals[axis][1]);
        }
    }
    template<bool INSTANTIATED>
    SYS_FORCE_INLINE T minDistance2(
        const UT_FixedVector<T,NAXES,INSTANTIATED> &p
    ) const noexcept {
        T diff = SYSmax(SYSmax(vals[0][0]-p[0], p[0]-vals[0][1]), T(0.0f));
        T d2 = diff*diff;
        for (int axis = 1; axis < NAXES; ++axis)
        {
            diff = SYSmax(SYSmax(vals[axis][0]-p[axis], p[axis]-vals[axis][1]), T(0.0f));
            d2 += diff*diff;
        }
        return d2;
    }
    template<bool INSTANTIATED>
    SYS_FORCE_INLINE T maxDistance2(
        const UT_FixedVector<T,NAXES,INSTANTIATED> &p
    ) const noexcept {
        T diff = SYSmax(p[0]-vals[0][0], vals[0][1]-p[0]);
        T d2 = diff*diff;
        for (int axis = 1; axis < NAXES; ++axis)
        {
            diff = SYSmax(p[axis]-vals[axis][0], vals[axis][1]-p[axis]);
            d2 += diff*diff;
        }
        return d2;
    }
};

/// Used by BVH::init to specify the heuristic to use for choosing between different box splits.
/// I tried putting this inside the BVH class, but I had difficulty getting it to compile.
enum class BVH_Heuristic {
    /// Tries to minimize the sum of axis lengths of the boxes.
    /// This is useful for applications where the probability of a box being applicable to a
    /// query is proportional to the "length", e.g. the probability of a random infinite plane
    /// intersecting the box.
    BOX_PERIMETER,

    /// Tries to minimize the "surface area" of the boxes.
    /// In 3D, uses the surface area; in 2D, uses the perimeter; in 1D, uses the axis length.
    /// This is what most applications, e.g. ray tracing, should use, particularly when the
    /// probability of a box being applicable to a query is proportional to the surface "area",
    /// e.g. the probability of a random ray hitting the box.
    ///
    /// NOTE: USE THIS ONE IF YOU ARE UNSURE!
    BOX_AREA,

    /// Tries to minimize the "volume" of the boxes.
    /// Uses the product of all axis lengths as a heuristic, (volume in 3D, area in 2D, length in 1D).
    /// This is useful for applications where the probability of a box being applicable to a
    /// query is proportional to the "volume", e.g. the probability of a random point being inside the box.
    BOX_VOLUME,

    /// Tries to minimize the "radii" of the boxes (i.e. the distance from the centre to a corner).
    /// This is useful for applications where the probability of a box being applicable to a
    /// query is proportional to the distance to the box centre, e.g. the probability of a random
    /// infinite plane being within the "radius" of the centre.
    BOX_RADIUS,

    /// Tries to minimize the squared "radii" of the boxes (i.e. the squared distance from the centre to a corner).
    /// This is useful for applications where the probability of a box being applicable to a
    /// query is proportional to the squared distance to the box centre, e.g. the probability of a random
    /// ray passing within the "radius" of the centre.
    BOX_RADIUS2,

    /// Tries to minimize the cubed "radii" of the boxes (i.e. the cubed distance from the centre to a corner).
    /// This is useful for applications where the probability of a box being applicable to a
    /// query is proportional to the cubed distance to the box centre, e.g. the probability of a random
    /// point being within the "radius" of the centre.
    BOX_RADIUS3,

    /// Tries to minimize the depth of the tree by primarily splitting at the median of the max axis.
    /// It may fall back to minimizing the area, but the tree depth should be unaffected.
    ///
    /// FIXME: This is not fully implemented yet.
    MEDIAN_MAX_AXIS
};

template<uint N>
class BVH {
public:
    using INT_TYPE = uint;
    struct Node {
        INT_TYPE child[N];

        static constexpr INT_TYPE theN = N;
        static constexpr INT_TYPE EMPTY = INT_TYPE(-1);
        static constexpr INT_TYPE INTERNAL_BIT = (INT_TYPE(1)<<(sizeof(INT_TYPE)*8 - 1));
        SYS_FORCE_INLINE static INT_TYPE markInternal(INT_TYPE internal_node_num) noexcept {
            return internal_node_num | INTERNAL_BIT;
        }
        SYS_FORCE_INLINE static bool isInternal(INT_TYPE node_int) noexcept {
            return (node_int & INTERNAL_BIT) != 0;
        }
        SYS_FORCE_INLINE static INT_TYPE getInternalNum(INT_TYPE node_int) noexcept {
            return node_int & ~INTERNAL_BIT;
        }
    };
private:
    struct FreeDeleter {
        SYS_FORCE_INLINE void operator()(Node* p) const {
            if (p) {
                // The pointer was allocated with malloc by UT_Array,
                // so it must be freed with free.
                free(p);
            }
        }
    };

    std::unique_ptr<Node[],FreeDeleter> myRoot;
    INT_TYPE myNumNodes;
public:
    SYS_FORCE_INLINE BVH() noexcept : myRoot(nullptr), myNumNodes(0) {}

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE=INT_TYPE>
    inline void init(const BOX_TYPE* boxes, const INT_TYPE nboxes, SRC_INT_TYPE* indices=nullptr, bool reorder_indices=false, INT_TYPE max_items_per_leaf=1) noexcept;

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE=INT_TYPE>
    inline void init(Box<T,NAXES> axes_minmax, const BOX_TYPE* boxes, INT_TYPE nboxes, SRC_INT_TYPE* indices=nullptr, bool reorder_indices=false, INT_TYPE max_items_per_leaf=1) noexcept;

    SYS_FORCE_INLINE
    INT_TYPE getNumNodes() const noexcept
    {
        return myNumNodes;
    }
    SYS_FORCE_INLINE
    const Node *getNodes() const noexcept
    {
        return myRoot.get();
    }

    SYS_FORCE_INLINE
    void clear() noexcept {
        myRoot.reset();
        myNumNodes = 0;
    }

    /// For each node, this effectively does:
    /// LOCAL_DATA local_data[MAX_ORDER];
    /// bool descend = functors.pre(nodei, parent_data);
    /// if (!descend)
    ///     return;
    /// for each child {
    ///     if (isitem(child))
    ///         functors.item(getitemi(child), nodei, local_data[child]);
    ///     else if (isnode(child))
    ///         recurse(getnodei(child), local_data);
    /// }
    /// functors.post(nodei, parent_nodei, data_for_parent, num_children, local_data);
    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverse(
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    /// This acts like the traverse function, except if the number of nodes in two subtrees
    /// of a node contain at least parallel_threshold nodes, they may be executed in parallel.
    /// If parallel_threshold is 0, even item_functor may be executed on items in parallel.
    /// NOTE: Make sure that your functors don't depend on the order that they're executed in,
    ///       e.g. don't add values from sibling nodes together except in post functor,
    ///       else they might have nondeterministic roundoff or miss some values entirely.
    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverseParallel(
        INT_TYPE parallel_threshold,
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    /// For each node, this effectively does:
    /// LOCAL_DATA local_data[MAX_ORDER];
    /// uint descend = functors.pre(nodei, parent_data);
    /// if (!descend)
    ///     return;
    /// for each child {
    ///     if (!(descend & (1<<child)))
    ///         continue;
    ///     if (isitem(child))
    ///         functors.item(getitemi(child), nodei, local_data[child]);
    ///     else if (isnode(child))
    ///         recurse(getnodei(child), local_data);
    /// }
    /// functors.post(nodei, parent_nodei, data_for_parent, num_children, local_data);
    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverseVector(
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    /// Prints a text representation of the tree to stdout.
    inline void debugDump() const;

    template<typename SRC_INT_TYPE>
    static inline void createTrivialIndices(SRC_INT_TYPE* indices, const INT_TYPE n) noexcept;

private:
    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverseHelper(
        INT_TYPE nodei,
        INT_TYPE parent_nodei,
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverseParallelHelper(
        INT_TYPE nodei,
        INT_TYPE parent_nodei,
        INT_TYPE parallel_threshold,
        INT_TYPE next_node_id,
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    template<typename LOCAL_DATA,typename FUNCTORS>
    inline void traverseVectorHelper(
        INT_TYPE nodei,
        INT_TYPE parent_nodei,
        FUNCTORS &functors,
        LOCAL_DATA *data_for_parent=nullptr) const noexcept;

    template<typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void computeFullBoundingBox(Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, const INT_TYPE nboxes, SRC_INT_TYPE* indices) noexcept;

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void initNode(UT_Array<Node>& nodes, Node &node, const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, const INT_TYPE nboxes) noexcept;

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void initNodeReorder(UT_Array<Node>& nodes, Node &node, const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, const INT_TYPE nboxes, const INT_TYPE indices_offset, const INT_TYPE max_items_per_leaf) noexcept;

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void multiSplit(const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE nboxes, SRC_INT_TYPE* sub_indices[N+1], Box<T,NAXES> sub_boxes[N]) noexcept;

    template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void split(const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE nboxes, SRC_INT_TYPE*& split_indices, Box<T,NAXES>* split_boxes) noexcept;

    template<INT_TYPE PARALLEL_THRESHOLD, typename SRC_INT_TYPE>
    static inline void adjustParallelChildNodes(INT_TYPE nparallel, UT_Array<Node>& nodes, Node& node, UT_Array<Node>* parallel_nodes, SRC_INT_TYPE* sub_indices) noexcept;

    template<typename T,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void nthElement(const BOX_TYPE* boxes, SRC_INT_TYPE* indices, const SRC_INT_TYPE* indices_end, const uint axis, SRC_INT_TYPE*const nth) noexcept;

    template<typename T,typename BOX_TYPE,typename SRC_INT_TYPE>
    static inline void partitionByCentre(const BOX_TYPE* boxes, SRC_INT_TYPE*const indices, const SRC_INT_TYPE*const indices_end, const uint axis, const T pivotx2, SRC_INT_TYPE*& ppivot_start, SRC_INT_TYPE*& ppivot_end) noexcept;

    /// An overestimate of the number of nodes needed.
    /// At worst, we could have only 2 children in every leaf, and
    /// then above that, we have a geometric series with r=1/N and a=(sub_nboxes/2)/N
    /// The true worst case might be a little worst than this, but
    /// it's probably fairly unlikely.
    SYS_FORCE_INLINE static INT_TYPE nodeEstimate(const INT_TYPE nboxes) noexcept {
        return nboxes/2 + nboxes/(2*(N-1));
    }

    template<BVH_Heuristic H,typename T, uint NAXES>
    SYS_FORCE_INLINE static T unweightedHeuristic(const Box<T, NAXES>& box) noexcept {
        if (H == BVH_Heuristic::BOX_PERIMETER) {
            return box.axis_sum();
        }
        if (H == BVH_Heuristic::BOX_AREA) {
            return box.half_surface_area();
        }
        if (H == BVH_Heuristic::BOX_VOLUME) {
            return box.volume();
        }
        if (H == BVH_Heuristic::BOX_RADIUS) {
            T diameter2 = box.diameter2();
            return SYSsqrt(diameter2);
        }
        if (H == BVH_Heuristic::BOX_RADIUS2) {
            return box.diameter2();
        }
        if (H == BVH_Heuristic::BOX_RADIUS3) {
            T diameter2 = box.diameter2();
            return diameter2*SYSsqrt(diameter2);
        }
        UT_IGL_ASSERT_MSG(0, "BVH_Heuristic::MEDIAN_MAX_AXIS should be handled separately by caller!");
        return T(1);
    }

    /// 16 equal-length spans (15 evenly-spaced splits) should be enough for a decent heuristic
    static constexpr INT_TYPE NSPANS = 16;
    static constexpr INT_TYPE NSPLITS = NSPANS-1;

    /// At least 1/16 of all boxes must be on each side, else we could end up with a very deep tree
    static constexpr INT_TYPE MIN_FRACTION = 16;
};

} // UT namespace

template<uint N>
using UT_BVH = UT::BVH<N>;

} // End HDK_Sample namespace
}}
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Bounding Volume Hierarchy (BVH) implementation.
 *      The main file is UT_BVH.h; this file is separate so that
 *      files that don't actually need to call functions on the BVH
 *      won't have unnecessary headers and functions included.
 */

#pragma once

#ifndef __HDK_UT_BVHImpl_h__
#define __HDK_UT_BVHImpl_h__








#include "parallel_for.h"

#include <iostream>
#include <algorithm>

namespace igl { 
  /// @private
  namespace FastWindingNumber {
namespace HDK_Sample {

namespace UT {

template<typename T,uint NAXES>
SYS_FORCE_INLINE bool utBoxExclude(const UT::Box<T,NAXES>& box) noexcept {
    bool has_nan_or_inf = !SYSisFinite(box[0][0]);
    has_nan_or_inf |= !SYSisFinite(box[0][1]);
    for (uint axis = 1; axis < NAXES; ++axis)
    {
        has_nan_or_inf |= !SYSisFinite(box[axis][0]);
        has_nan_or_inf |= !SYSisFinite(box[axis][1]);
    }
    return has_nan_or_inf;
}
template<uint NAXES>
SYS_FORCE_INLINE bool utBoxExclude(const UT::Box<fpreal32,NAXES>& box) noexcept {
    const int32 *pboxints = reinterpret_cast<const int32*>(&box);
    // Fast check for NaN or infinity: check if exponent bits are 0xFF.
    bool has_nan_or_inf = ((pboxints[0] & 0x7F800000) == 0x7F800000);
    has_nan_or_inf |= ((pboxints[1] & 0x7F800000) == 0x7F800000);
    for (uint axis = 1; axis < NAXES; ++axis)
    {
        has_nan_or_inf |= ((pboxints[2*axis] & 0x7F800000) == 0x7F800000);
        has_nan_or_inf |= ((pboxints[2*axis + 1] & 0x7F800000) == 0x7F800000);
    }
    return has_nan_or_inf;
}
template<typename T,uint NAXES>
SYS_FORCE_INLINE T utBoxCenter(const UT::Box<T,NAXES>& box, uint axis) noexcept {
    const T* v = box.vals[axis];
    return v[0] + v[1];
}
template<typename T>
struct ut_BoxCentre {
    constexpr static uint scale = 2;
};
template<typename T,uint NAXES,bool INSTANTIATED>
SYS_FORCE_INLINE T utBoxExclude(const UT_FixedVector<T,NAXES,INSTANTIATED>& position) noexcept {
    bool has_nan_or_inf = !SYSisFinite(position[0]);
    for (uint axis = 1; axis < NAXES; ++axis)
        has_nan_or_inf |= !SYSisFinite(position[axis]);
    return has_nan_or_inf;
}
template<uint NAXES,bool INSTANTIATED>
SYS_FORCE_INLINE bool utBoxExclude(const UT_FixedVector<fpreal32,NAXES,INSTANTIATED>& position) noexcept {
    const int32 *ppositionints = reinterpret_cast<const int32*>(&position);
    // Fast check for NaN or infinity: check if exponent bits are 0xFF.
    bool has_nan_or_inf = ((ppositionints[0] & 0x7F800000) == 0x7F800000);
    for (uint axis = 1; axis < NAXES; ++axis)
        has_nan_or_inf |= ((ppositionints[axis] & 0x7F800000) == 0x7F800000);
    return has_nan_or_inf;
}
template<typename T,uint NAXES,bool INSTANTIATED>
SYS_FORCE_INLINE T utBoxCenter(const UT_FixedVector<T,NAXES,INSTANTIATED>& position, uint axis) noexcept {
    return position[axis];
}
template<typename T,uint NAXES,bool INSTANTIATED>
struct ut_BoxCentre<UT_FixedVector<T,NAXES,INSTANTIATED>> {
    constexpr static uint scale = 1;
};

template<typename BOX_TYPE,typename SRC_INT_TYPE,typename INT_TYPE>
inline INT_TYPE utExcludeNaNInfBoxIndices(const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE& nboxes) noexcept 
{
    //constexpr INT_TYPE PARALLEL_THRESHOLD = 65536;
    //INT_TYPE ntasks = 1;
    //if (nboxes >= PARALLEL_THRESHOLD) 
    //{
    //    INT_TYPE nprocessors = UT_Thread::getNumProcessors();
    //    ntasks = (nprocessors > 1) ? SYSmin(4*nprocessors, nboxes/(PARALLEL_THRESHOLD/2)) : 1;
    //}
    //if (ntasks == 1) 
    {
        // Serial: easy case; just loop through.

        const SRC_INT_TYPE* indices_end = indices + nboxes;

        // Loop through forward once
        SRC_INT_TYPE* psrc_index = indices;
        for (; psrc_index != indices_end; ++psrc_index) 
        {
            const bool exclude = utBoxExclude(boxes[*psrc_index]);
            if (exclude)
                break;
        }
        if (psrc_index == indices_end)
            return 0;

        // First NaN or infinite box
        SRC_INT_TYPE* nan_start = psrc_index;
        for (++psrc_index; psrc_index != indices_end; ++psrc_index) 
	{
            const bool exclude = utBoxExclude(boxes[*psrc_index]);
            if (!exclude) 
	    {
                *nan_start = *psrc_index;
                ++nan_start;
            }
        }
        nboxes = nan_start-indices;
        return indices_end - nan_start;
    }

}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::init(const BOX_TYPE* boxes, const INT_TYPE nboxes, SRC_INT_TYPE* indices, bool reorder_indices, INT_TYPE max_items_per_leaf) noexcept {
    Box<T,NAXES> axes_minmax;
    computeFullBoundingBox(axes_minmax, boxes, nboxes, indices);

    init<H>(axes_minmax, boxes, nboxes, indices, reorder_indices, max_items_per_leaf);
}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::init(Box<T,NAXES> axes_minmax, const BOX_TYPE* boxes, INT_TYPE nboxes, SRC_INT_TYPE* indices, bool reorder_indices, INT_TYPE max_items_per_leaf) noexcept {
    // Clear the tree in advance to save memory.
    myRoot.reset();

    if (nboxes == 0) {
        myNumNodes = 0;
        return;
    }

    UT_Array<INT_TYPE> local_indices;
    if (!indices) {
        local_indices.setSizeNoInit(nboxes);
        indices = local_indices.array();
        createTrivialIndices(indices, nboxes);
    }

    // Exclude any boxes with NaNs or infinities by shifting down indices
    // over the bad box indices and updating nboxes.
    INT_TYPE nexcluded = utExcludeNaNInfBoxIndices(boxes, indices, nboxes);
    if (nexcluded != 0) {
        if (nboxes == 0) {
            myNumNodes = 0;
            return;
        }
        computeFullBoundingBox(axes_minmax, boxes, nboxes, indices);
    }

    UT_Array<Node> nodes;
    // Preallocate an overestimate of the number of nodes needed.
    nodes.setCapacity(nodeEstimate(nboxes));
    nodes.setSize(1);
    if (reorder_indices)
        initNodeReorder<H>(nodes, nodes[0], axes_minmax, boxes, indices, nboxes, 0, max_items_per_leaf);
    else
        initNode<H>(nodes, nodes[0], axes_minmax, boxes, indices, nboxes);

    // If capacity is more than 12.5% over the size, rellocate.
    if (8*nodes.capacity() > 9*nodes.size()) {
        nodes.setCapacity(nodes.size());
    }
    // Steal ownership of the array from the UT_Array
    myRoot.reset(nodes.array());
    myNumNodes = nodes.size();
    nodes.unsafeClearData();
}

template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverse(
    FUNCTORS &functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    if (!myRoot)
        return;

    // NOTE: The root is always index 0.
    traverseHelper(0, INT_TYPE(-1), functors, data_for_parent);
}
template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverseHelper(
    INT_TYPE nodei,
    INT_TYPE parent_nodei,
    FUNCTORS &functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    const Node &node = myRoot[nodei];
    bool descend = functors.pre(nodei, data_for_parent);
    if (!descend)
        return;
    LOCAL_DATA local_data[N];
    INT_TYPE s;
    for (s = 0; s < N; ++s) {
        const INT_TYPE node_int = node.child[s];
        if (Node::isInternal(node_int)) {
            if (node_int == Node::EMPTY) {
                // NOTE: Anything after this will be empty too, so we can break.
                break;
            }
            traverseHelper(Node::getInternalNum(node_int), nodei, functors, &local_data[s]);
        }
        else {
            functors.item(node_int, nodei, local_data[s]);
        }
    }
    // NOTE: s is now the number of non-empty entries in this node.
    functors.post(nodei, parent_nodei, data_for_parent, s, local_data);
}

template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverseParallel(
    INT_TYPE parallel_threshold,
    FUNCTORS& functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    if (!myRoot)
        return;

    // NOTE: The root is always index 0.
    traverseParallelHelper(0, INT_TYPE(-1), parallel_threshold, myNumNodes, functors, data_for_parent);
}
template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverseParallelHelper(
    INT_TYPE nodei,
    INT_TYPE parent_nodei,
    INT_TYPE parallel_threshold,
    INT_TYPE next_node_id,
    FUNCTORS& functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    const Node &node = myRoot[nodei];
    bool descend = functors.pre(nodei, data_for_parent);
    if (!descend)
        return;

    // To determine the number of nodes in a child's subtree, we take the next
    // node ID minus the current child's node ID.
    INT_TYPE next_nodes[N];
    INT_TYPE nnodes[N];
    INT_TYPE nchildren = N;
    INT_TYPE nparallel = 0;
    // s is currently unsigned, so we check s < N for bounds check.
    // The s >= 0 check is in case s ever becomes signed, and should be
    // automatically removed by the compiler for unsigned s.
    for (INT_TYPE s = N-1; (std::is_signed<INT_TYPE>::value ? (s >= 0) : (s < N)); --s) {
        const INT_TYPE node_int = node.child[s];
        if (node_int == Node::EMPTY) {
            --nchildren;
            continue;
        }
        next_nodes[s] = next_node_id;
        if (Node::isInternal(node_int)) {
            // NOTE: This depends on BVH<N>::initNode appending the child nodes
            //       in between their content, instead of all at once.
            INT_TYPE child_node_id = Node::getInternalNum(node_int);
            nnodes[s] = next_node_id - child_node_id;
            next_node_id = child_node_id;
        }
        else {
            nnodes[s] = 0;
        }
        nparallel += (nnodes[s] >= parallel_threshold);
    }

    LOCAL_DATA local_data[N];
    if (nparallel >= 2) {
        // Do any non-parallel ones first
        if (nparallel < nchildren) {
            for (INT_TYPE s = 0; s < N; ++s) {
                if (nnodes[s] >= parallel_threshold) {
                    continue;
                }
                const INT_TYPE node_int = node.child[s];
                if (Node::isInternal(node_int)) {
                    if (node_int == Node::EMPTY) {
                        // NOTE: Anything after this will be empty too, so we can break.
                        break;
                    }
                    traverseHelper(Node::getInternalNum(node_int), nodei, functors, &local_data[s]);
                }
                else {
                    functors.item(node_int, nodei, local_data[s]);
                }
            }
        }
        // Now do the parallel ones
        igl::parallel_for(
          nparallel,
          [this,nodei,&node,&nnodes,&next_nodes,&parallel_threshold,&functors,&local_data](int taski)
          {
            INT_TYPE parallel_count = 0;
            // NOTE: The check for s < N is just so that the compiler can
            //       (hopefully) figure out that it can fully unroll the loop.
            INT_TYPE s;
            for (s = 0; s < N; ++s) {
                if (nnodes[s] < parallel_threshold) {
                    continue;
                }
                if (parallel_count == taski) {
                    break;
                }
                ++parallel_count;
            }
            const INT_TYPE node_int = node.child[s];
            if (Node::isInternal(node_int)) {
                UT_IGL_ASSERT_MSG_P(node_int != Node::EMPTY, "Empty entries should have been excluded above.");
                traverseParallelHelper(Node::getInternalNum(node_int), nodei, parallel_threshold, next_nodes[s], functors, &local_data[s]);
            }
            else {
                functors.item(node_int, nodei, local_data[s]);
            }
          });
    }
    else {
        // All in serial
        for (INT_TYPE s = 0; s < N; ++s) {
            const INT_TYPE node_int = node.child[s];
            if (Node::isInternal(node_int)) {
                if (node_int == Node::EMPTY) {
                    // NOTE: Anything after this will be empty too, so we can break.
                    break;
                }
                traverseHelper(Node::getInternalNum(node_int), nodei, functors, &local_data[s]);
            }
            else {
                functors.item(node_int, nodei, local_data[s]);
            }
        }
    }
    functors.post(nodei, parent_nodei, data_for_parent, nchildren, local_data);
}

template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverseVector(
    FUNCTORS &functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    if (!myRoot)
        return;

    // NOTE: The root is always index 0.
    traverseVectorHelper(0, INT_TYPE(-1), functors, data_for_parent);
}
template<uint N>
template<typename LOCAL_DATA,typename FUNCTORS>
inline void BVH<N>::traverseVectorHelper(
    INT_TYPE nodei,
    INT_TYPE parent_nodei,
    FUNCTORS &functors,
    LOCAL_DATA* data_for_parent) const noexcept
{
    const Node &node = myRoot[nodei];
    INT_TYPE descend = functors.pre(nodei, data_for_parent);
    if (!descend)
        return;
    LOCAL_DATA local_data[N];
    INT_TYPE s;
    for (s = 0; s < N; ++s) {
        if ((descend>>s) & 1) {
            const INT_TYPE node_int = node.child[s];
            if (Node::isInternal(node_int)) {
                if (node_int == Node::EMPTY) {
                    // NOTE: Anything after this will be empty too, so we can break.
                    descend &= (INT_TYPE(1)<<s)-1;
                    break;
                }
                traverseVectorHelper(Node::getInternalNum(node_int), nodei, functors, &local_data[s]);
            }
            else {
                functors.item(node_int, nodei, local_data[s]);
            }
        }
    }
    // NOTE: s is now the number of non-empty entries in this node.
    functors.post(nodei, parent_nodei, data_for_parent, s, local_data, descend);
}


template<uint N>
template<typename SRC_INT_TYPE>
inline void BVH<N>::createTrivialIndices(SRC_INT_TYPE* indices, const INT_TYPE n) noexcept {
    igl::parallel_for(n, [indices](INT_TYPE i) { indices[i] = i; }, 65536);
}

template<uint N>
template<typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::computeFullBoundingBox(Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, const INT_TYPE nboxes, SRC_INT_TYPE* indices) noexcept {
    if (!nboxes) {
        axes_minmax.initBounds();
        return;
    }
    INT_TYPE ntasks = 1;
    if (nboxes >= 2*4096) {
        INT_TYPE nprocessors = UT_Thread::getNumProcessors();
        ntasks = (nprocessors > 1) ? SYSmin(4*nprocessors, nboxes/4096) : 1;
    }
    if (ntasks == 1) {
        Box<T,NAXES> box;
        if (indices) {
            box.initBounds(boxes[indices[0]]);
            for (INT_TYPE i = 1; i < nboxes; ++i) {
                box.combine(boxes[indices[i]]);
            }
        }
        else {
            box.initBounds(boxes[0]);
            for (INT_TYPE i = 1; i < nboxes; ++i) {
                box.combine(boxes[i]);
            }
        }
        axes_minmax = box;
    }
    else {
        UT_SmallArray<Box<T,NAXES>> parallel_boxes;
        Box<T,NAXES> box;
        igl::parallel_for(
          nboxes,
          [&parallel_boxes](int n){parallel_boxes.setSize(n);},
          [&parallel_boxes,indices,&boxes](int i, int t)
          {
            if(indices)
            {
              parallel_boxes[t].combine(boxes[indices[i]]);
            }else
            {
              parallel_boxes[t].combine(boxes[i]);
            }
          },
          [&parallel_boxes,&box](int t)
          {
            if(t == 0)
            {
              box = parallel_boxes[0];
            }else
            {
              box.combine(parallel_boxes[t]);
            }
          });

        axes_minmax = box;
    }
}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::initNode(UT_Array<Node>& nodes, Node &node, const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, const INT_TYPE nboxes) noexcept {
    if (nboxes <= N) {
        // Fits in one node
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            node.child[i] = indices[i];
        }
        for (INT_TYPE i = nboxes; i < N; ++i) {
            node.child[i] = Node::EMPTY;
        }
        return;
    }

    SRC_INT_TYPE* sub_indices[N+1];
    Box<T,NAXES> sub_boxes[N];

    if (N == 2) {
        sub_indices[0] = indices;
        sub_indices[2] = indices+nboxes;
        split<H>(axes_minmax, boxes, indices, nboxes, sub_indices[1], &sub_boxes[0]);
    }
    else {
        multiSplit<H>(axes_minmax, boxes, indices, nboxes, sub_indices, sub_boxes);
    }

    // Count the number of nodes to run in parallel and fill in single items in this node
    INT_TYPE nparallel = 0;
    static constexpr INT_TYPE PARALLEL_THRESHOLD = 1024;
    for (INT_TYPE i = 0; i < N; ++i) {
        INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
        if (sub_nboxes == 1) {
            node.child[i] = sub_indices[i][0];
        }
        else if (sub_nboxes >= PARALLEL_THRESHOLD) {
            ++nparallel;
        }
    }

    // NOTE: Child nodes of this node need to be placed just before the nodes in
    //       their corresponding subtree, in between the subtrees, because
    //       traverseParallel uses the difference between the child node IDs
    //       to determine the number of nodes in the subtree.

    // Recurse
    if (nparallel >= 2) {
        UT_SmallArray<UT_Array<Node>> parallel_nodes;
        UT_SmallArray<Node> parallel_parent_nodes;
        parallel_nodes.setSize(nparallel);
        parallel_parent_nodes.setSize(nparallel);
        igl::parallel_for(
          nparallel,
          [&parallel_nodes,&parallel_parent_nodes,&sub_indices,boxes,&sub_boxes](int taski)
          {
            // First, find which child this is
            INT_TYPE counted_parallel = 0;
            INT_TYPE sub_nboxes;
            INT_TYPE childi;
            for (childi = 0; childi < N; ++childi) {
                sub_nboxes = sub_indices[childi+1]-sub_indices[childi];
                if (sub_nboxes >= PARALLEL_THRESHOLD) {
                    if (counted_parallel == taski) {
                        break;
                    }
                    ++counted_parallel;
                }
            }
            UT_IGL_ASSERT_P(counted_parallel == taski);

            UT_Array<Node>& local_nodes = parallel_nodes[taski];
            // Preallocate an overestimate of the number of nodes needed.
            // At worst, we could have only 2 children in every leaf, and
            // then above that, we have a geometric series with r=1/N and a=(sub_nboxes/2)/N
            // The true worst case might be a little worst than this, but
            // it's probably fairly unlikely.
            local_nodes.setCapacity(nodeEstimate(sub_nboxes));
            Node& parent_node = parallel_parent_nodes[taski];

            // We'll have to fix the internal node numbers in parent_node and local_nodes later
            initNode<H>(local_nodes, parent_node, sub_boxes[childi], boxes, sub_indices[childi], sub_nboxes);
          });

        INT_TYPE counted_parallel = 0;
        for (INT_TYPE i = 0; i < N; ++i) {
            INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
            if (sub_nboxes != 1) {
                INT_TYPE local_nodes_start = nodes.size();
                node.child[i] = Node::markInternal(local_nodes_start);
                if (sub_nboxes >= PARALLEL_THRESHOLD) {
                    // First, adjust the root child node
                    Node child_node = parallel_parent_nodes[counted_parallel];
                    ++local_nodes_start;
                    for (INT_TYPE childi = 0; childi < N; ++childi) {
                        INT_TYPE child_child = child_node.child[childi];
                        if (Node::isInternal(child_child) && child_child != Node::EMPTY) {
                            child_child += local_nodes_start;
                            child_node.child[childi] = child_child;
                        }
                    }

                    // Make space in the array for the sub-child nodes
                    const UT_Array<Node>& local_nodes = parallel_nodes[counted_parallel];
                    ++counted_parallel;
                    INT_TYPE n = local_nodes.size();
                    nodes.bumpCapacity(local_nodes_start + n);
                    nodes.setSizeNoInit(local_nodes_start + n);
                    nodes[local_nodes_start-1] = child_node;
                }
                else {
                    nodes.bumpCapacity(local_nodes_start + 1);
                    nodes.setSizeNoInit(local_nodes_start + 1);
                    initNode<H>(nodes, nodes[local_nodes_start], sub_boxes[i], boxes, sub_indices[i], sub_nboxes);
                }
            }
        }

        // Now, adjust and copy all sub-child nodes that were made in parallel
        adjustParallelChildNodes<PARALLEL_THRESHOLD>(nparallel, nodes, node, parallel_nodes.array(), sub_indices);
    }
    else {
        for (INT_TYPE i = 0; i < N; ++i) {
            INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
            if (sub_nboxes != 1) {
                INT_TYPE local_nodes_start = nodes.size();
                node.child[i] = Node::markInternal(local_nodes_start);
                nodes.bumpCapacity(local_nodes_start + 1);
                nodes.setSizeNoInit(local_nodes_start + 1);
                initNode<H>(nodes, nodes[local_nodes_start], sub_boxes[i], boxes, sub_indices[i], sub_nboxes);
            }
        }
    }
}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::initNodeReorder(UT_Array<Node>& nodes, Node &node, const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE nboxes, const INT_TYPE indices_offset, const INT_TYPE max_items_per_leaf) noexcept {
    if (nboxes <= N) {
        // Fits in one node
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            node.child[i] = indices_offset+i;
        }
        for (INT_TYPE i = nboxes; i < N; ++i) {
            node.child[i] = Node::EMPTY;
        }
        return;
    }

    SRC_INT_TYPE* sub_indices[N+1];
    Box<T,NAXES> sub_boxes[N];

    if (N == 2) {
        sub_indices[0] = indices;
        sub_indices[2] = indices+nboxes;
        split<H>(axes_minmax, boxes, indices, nboxes, sub_indices[1], &sub_boxes[0]);
    }
    else {
        multiSplit<H>(axes_minmax, boxes, indices, nboxes, sub_indices, sub_boxes);
    }

    // Move any children with max_items_per_leaf or fewer indices before any children with more,
    // for better cache coherence when we're accessing data in a corresponding array.
    INT_TYPE nleaves = 0;
    UT_SmallArray<SRC_INT_TYPE> leaf_indices;
    SRC_INT_TYPE leaf_sizes[N];
    INT_TYPE sub_nboxes0 = sub_indices[1]-sub_indices[0];
    if (sub_nboxes0 <= max_items_per_leaf) {
        leaf_sizes[0] = sub_nboxes0;
        for (int j = 0; j < sub_nboxes0; ++j)
            leaf_indices.append(sub_indices[0][j]);
        ++nleaves;
    }
    INT_TYPE sub_nboxes1 = sub_indices[2]-sub_indices[1];
    if (sub_nboxes1 <= max_items_per_leaf) {
        leaf_sizes[nleaves] = sub_nboxes1;
        for (int j = 0; j < sub_nboxes1; ++j)
            leaf_indices.append(sub_indices[1][j]);
        ++nleaves;
    }
    for (INT_TYPE i = 2; i < N; ++i) {
        INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
        if (sub_nboxes <= max_items_per_leaf) {
            leaf_sizes[nleaves] = sub_nboxes;
            for (int j = 0; j < sub_nboxes; ++j)
                leaf_indices.append(sub_indices[i][j]);
            ++nleaves;
        }
    }
    if (nleaves > 0) {
        // NOTE: i < N condition is because INT_TYPE is unsigned.
        //       i >= 0 condition is in case INT_TYPE is changed to signed.
        INT_TYPE move_distance = 0;
        INT_TYPE index_move_distance = 0;
        for (INT_TYPE i = N-1; (std::is_signed<INT_TYPE>::value ? (i >= 0) : (i < N)); --i) {
            INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
            if (sub_nboxes <= max_items_per_leaf) {
                ++move_distance;
                index_move_distance += sub_nboxes;
            }
            else if (move_distance > 0) {
                SRC_INT_TYPE *start_src_index = sub_indices[i];
                for (SRC_INT_TYPE *src_index = sub_indices[i+1]-1; src_index >= start_src_index; --src_index) {
                    src_index[index_move_distance] = src_index[0];
                }
                sub_indices[i+move_distance] = sub_indices[i]+index_move_distance;
            }
        }
        index_move_distance = 0;
        for (INT_TYPE i = 0; i < nleaves; ++i) {
            INT_TYPE sub_nboxes = leaf_sizes[i];
            sub_indices[i] = indices+index_move_distance;
            for (int j = 0; j < sub_nboxes; ++j)
                indices[index_move_distance+j] = leaf_indices[index_move_distance+j];
            index_move_distance += sub_nboxes;
        }
    }

    // Count the number of nodes to run in parallel and fill in single items in this node
    INT_TYPE nparallel = 0;
    static constexpr INT_TYPE PARALLEL_THRESHOLD = 1024;
    for (INT_TYPE i = 0; i < N; ++i) {
        INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
        if (sub_nboxes <= max_items_per_leaf) {
            node.child[i] = indices_offset+(sub_indices[i]-sub_indices[0]);
        }
        else if (sub_nboxes >= PARALLEL_THRESHOLD) {
            ++nparallel;
        }
    }

    // NOTE: Child nodes of this node need to be placed just before the nodes in
    //       their corresponding subtree, in between the subtrees, because
    //       traverseParallel uses the difference between the child node IDs
    //       to determine the number of nodes in the subtree.

    // Recurse
    if (nparallel >= 2 && false) {
      assert(false && "Not implemented; should never get here");
      exit(1);
      //  // Do the parallel ones first, so that they can be inserted in the right place.
      //  // Although the choice may seem somewhat arbitrary, we need the results to be
      //  // identical whether we choose to parallelize or not, and in case we change the
      //  // threshold later.
      //  UT_SmallArray<UT_Array<Node>,4*sizeof(UT_Array<Node>)> parallel_nodes;
      //  parallel_nodes.setSize(nparallel);
      //  UT_SmallArray<Node,4*sizeof(Node)> parallel_parent_nodes;
      //  parallel_parent_nodes.setSize(nparallel);
      //  UTparallelFor(UT_BlockedRange<INT_TYPE>(0,nparallel), [&parallel_nodes,&parallel_parent_nodes,&sub_indices,boxes,&sub_boxes,indices_offset,max_items_per_leaf](const UT_BlockedRange<INT_TYPE>& r) {
      //      for (INT_TYPE taski = r.begin(), end = r.end(); taski < end; ++taski) {
      //          // First, find which child this is
      //          INT_TYPE counted_parallel = 0;
      //          INT_TYPE sub_nboxes;
      //          INT_TYPE childi;
      //          for (childi = 0; childi < N; ++childi) {
      //              sub_nboxes = sub_indices[childi+1]-sub_indices[childi];
      //              if (sub_nboxes >= PARALLEL_THRESHOLD) {
      //                  if (counted_parallel == taski) {
      //                      break;
      //                  }
      //                  ++counted_parallel;
      //              }
      //          }
      //          UT_IGL_ASSERT_P(counted_parallel == taski);

      //          UT_Array<Node>& local_nodes = parallel_nodes[taski];
      //          // Preallocate an overestimate of the number of nodes needed.
      //          // At worst, we could have only 2 children in every leaf, and
      //          // then above that, we have a geometric series with r=1/N and a=(sub_nboxes/2)/N
      //          // The true worst case might be a little worst than this, but
      //          // it's probably fairly unlikely.
      //          local_nodes.setCapacity(nodeEstimate(sub_nboxes));
      //          Node& parent_node = parallel_parent_nodes[taski];

      //          // We'll have to fix the internal node numbers in parent_node and local_nodes later
      //          initNodeReorder<H>(local_nodes, parent_node, sub_boxes[childi], boxes, sub_indices[childi], sub_nboxes,
      //              indices_offset+(sub_indices[childi]-sub_indices[0]), max_items_per_leaf);
      //      }
      //  }, 0, 1);

      //  INT_TYPE counted_parallel = 0;
      //  for (INT_TYPE i = 0; i < N; ++i) {
      //      INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
      //      if (sub_nboxes > max_items_per_leaf) {
      //          INT_TYPE local_nodes_start = nodes.size();
      //          node.child[i] = Node::markInternal(local_nodes_start);
      //          if (sub_nboxes >= PARALLEL_THRESHOLD) {
      //              // First, adjust the root child node
      //              Node child_node = parallel_parent_nodes[counted_parallel];
      //              ++local_nodes_start;
      //              for (INT_TYPE childi = 0; childi < N; ++childi) {
      //                  INT_TYPE child_child = child_node.child[childi];
      //                  if (Node::isInternal(child_child) && child_child != Node::EMPTY) {
      //                      child_child += local_nodes_start;
      //                      child_node.child[childi] = child_child;
      //                  }
      //              }

      //              // Make space in the array for the sub-child nodes
      //              const UT_Array<Node>& local_nodes = parallel_nodes[counted_parallel];
      //              ++counted_parallel;
      //              INT_TYPE n = local_nodes.size();
      //              nodes.bumpCapacity(local_nodes_start + n);
      //              nodes.setSizeNoInit(local_nodes_start + n);
      //              nodes[local_nodes_start-1] = child_node;
      //          }
      //          else {
      //              nodes.bumpCapacity(local_nodes_start + 1);
      //              nodes.setSizeNoInit(local_nodes_start + 1);
      //              initNodeReorder<H>(nodes, nodes[local_nodes_start], sub_boxes[i], boxes, sub_indices[i], sub_nboxes,
      //                  indices_offset+(sub_indices[i]-sub_indices[0]), max_items_per_leaf);
      //          }
      //      }
      //  }

      //  // Now, adjust and copy all sub-child nodes that were made in parallel
      //  adjustParallelChildNodes<PARALLEL_THRESHOLD>(nparallel, nodes, node, parallel_nodes.array(), sub_indices);
    }
    else {
        for (INT_TYPE i = 0; i < N; ++i) {
            INT_TYPE sub_nboxes = sub_indices[i+1]-sub_indices[i];
            if (sub_nboxes > max_items_per_leaf) {
                INT_TYPE local_nodes_start = nodes.size();
                node.child[i] = Node::markInternal(local_nodes_start);
                nodes.bumpCapacity(local_nodes_start + 1);
                nodes.setSizeNoInit(local_nodes_start + 1);
                initNodeReorder<H>(nodes, nodes[local_nodes_start], sub_boxes[i], boxes, sub_indices[i], sub_nboxes,
                    indices_offset+(sub_indices[i]-sub_indices[0]), max_items_per_leaf);
            }
        }
    }
}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::multiSplit(const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE nboxes, SRC_INT_TYPE* sub_indices[N+1], Box<T,NAXES> sub_boxes[N]) noexcept {
    sub_indices[0] = indices;
    sub_indices[2] = indices+nboxes;
    split<H>(axes_minmax, boxes, indices, nboxes, sub_indices[1], &sub_boxes[0]);

    if (N == 2) {
        return;
    }

    if (H == BVH_Heuristic::MEDIAN_MAX_AXIS) {
        SRC_INT_TYPE* sub_indices_startend[2*N];
        Box<T,NAXES> sub_boxes_unsorted[N];
        sub_boxes_unsorted[0] = sub_boxes[0];
        sub_boxes_unsorted[1] = sub_boxes[1];
        sub_indices_startend[0] = sub_indices[0];
        sub_indices_startend[1] = sub_indices[1];
        sub_indices_startend[2] = sub_indices[1];
        sub_indices_startend[3] = sub_indices[2];
        for (INT_TYPE nsub = 2; nsub < N; ++nsub) {
            SRC_INT_TYPE* selected_start = sub_indices_startend[0];
            SRC_INT_TYPE* selected_end = sub_indices_startend[1];
            Box<T,NAXES> sub_box = sub_boxes_unsorted[0];

            // Shift results back.
            for (INT_TYPE i = 0; i < nsub-1; ++i) {
                sub_indices_startend[2*i  ] = sub_indices_startend[2*i+2];
                sub_indices_startend[2*i+1] = sub_indices_startend[2*i+3];
            }
            for (INT_TYPE i = 0; i < nsub-1; ++i) {
                sub_boxes_unsorted[i] = sub_boxes_unsorted[i-1];
            }

            // Do the split
            split<H>(sub_box, boxes, selected_start, selected_end-selected_start, sub_indices_startend[2*nsub-1], &sub_boxes_unsorted[nsub]);
            sub_indices_startend[2*nsub-2] = selected_start;
            sub_indices_startend[2*nsub] = sub_indices_startend[2*nsub-1];
            sub_indices_startend[2*nsub+1] = selected_end;

            // Sort pointers so that they're in the correct order
            sub_indices[N] = indices+nboxes;
            for (INT_TYPE i = 0; i < N; ++i) {
                SRC_INT_TYPE* prev_pointer = (i != 0) ? sub_indices[i-1] : nullptr;
                SRC_INT_TYPE* min_pointer = nullptr;
                Box<T,NAXES> box;
                for (INT_TYPE j = 0; j < N; ++j) {
                    SRC_INT_TYPE* cur_pointer = sub_indices_startend[2*j];
                    if ((cur_pointer > prev_pointer) && (!min_pointer || (cur_pointer < min_pointer))) {
                        min_pointer = cur_pointer;
                        box = sub_boxes_unsorted[j];
                    }
                }
                UT_IGL_ASSERT_P(min_pointer);
                sub_indices[i] = min_pointer;
                sub_boxes[i] = box;
            }
        }
    }
    else {
        T sub_box_areas[N];
        sub_box_areas[0] = unweightedHeuristic<H>(sub_boxes[0]);
        sub_box_areas[1] = unweightedHeuristic<H>(sub_boxes[1]);
        for (INT_TYPE nsub = 2; nsub < N; ++nsub) {
            // Choose which one to split
            INT_TYPE split_choice = INT_TYPE(-1);
            T max_heuristic;
            for (INT_TYPE i = 0; i < nsub; ++i) {
                const INT_TYPE index_count = (sub_indices[i+1]-sub_indices[i]);
                if (index_count > 1) {
                    const T heuristic = sub_box_areas[i]*index_count;
                    if (split_choice == INT_TYPE(-1) || heuristic > max_heuristic) {
                        split_choice = i;
                        max_heuristic = heuristic;
                    }
                }
            }
            UT_IGL_ASSERT_MSG_P(split_choice != INT_TYPE(-1), "There should always be at least one that can be split!");

            SRC_INT_TYPE* selected_start = sub_indices[split_choice];
            SRC_INT_TYPE* selected_end = sub_indices[split_choice+1];

            // Shift results over; we can skip the one we selected.
            for (INT_TYPE i = nsub; i > split_choice; --i) {
                sub_indices[i+1] = sub_indices[i];
            }
            for (INT_TYPE i = nsub-1; i > split_choice; --i) {
                sub_boxes[i+1] = sub_boxes[i];
            }
            for (INT_TYPE i = nsub-1; i > split_choice; --i) {
                sub_box_areas[i+1] = sub_box_areas[i];
            }

            // Do the split
            split<H>(sub_boxes[split_choice], boxes, selected_start, selected_end-selected_start, sub_indices[split_choice+1], &sub_boxes[split_choice]);
            sub_box_areas[split_choice] = unweightedHeuristic<H>(sub_boxes[split_choice]);
            sub_box_areas[split_choice+1] = unweightedHeuristic<H>(sub_boxes[split_choice+1]);
        }
    }
}

template<uint N>
template<BVH_Heuristic H,typename T,uint NAXES,typename BOX_TYPE,typename SRC_INT_TYPE>
inline void BVH<N>::split(const Box<T,NAXES>& axes_minmax, const BOX_TYPE* boxes, SRC_INT_TYPE* indices, INT_TYPE nboxes, SRC_INT_TYPE*& split_indices, Box<T,NAXES>* split_boxes) noexcept {
    if (nboxes == 2) {
        split_boxes[0].initBounds(boxes[indices[0]]);
        split_boxes[1].initBounds(boxes[indices[1]]);
        split_indices = indices+1;
        return;
    }
    UT_IGL_ASSERT_MSG_P(nboxes > 2, "Cases with less than 3 boxes should have already been handled!");

    if (H == BVH_Heuristic::MEDIAN_MAX_AXIS) {
        UT_IGL_ASSERT_MSG(0, "FIXME: Implement this!!!");
    }

    constexpr INT_TYPE SMALL_LIMIT = 6;
    if (nboxes <= SMALL_LIMIT) {
        // Special case for a small number of boxes: check all (2^(n-1))-1 partitions.
        // Without loss of generality, we assume that box 0 is in partition 0,
        // and that not all boxes are in partition 0.
        Box<T,NAXES> local_boxes[SMALL_LIMIT];
        for (INT_TYPE box = 0; box < nboxes; ++box) {
            local_boxes[box].initBounds(boxes[indices[box]]);
            //printf("Box %u: (%f-%f)x(%f-%f)x(%f-%f)\n", uint(box), local_boxes[box].vals[0][0], local_boxes[box].vals[0][1], local_boxes[box].vals[1][0], local_boxes[box].vals[1][1], local_boxes[box].vals[2][0], local_boxes[box].vals[2][1]);
        }
        const INT_TYPE partition_limit = (INT_TYPE(1)<<(nboxes-1));
        INT_TYPE best_partition = INT_TYPE(-1);
        T best_heuristic;
        for (INT_TYPE partition_bits = 1; partition_bits < partition_limit; ++partition_bits) {
            Box<T,NAXES> sub_boxes[2];
            sub_boxes[0] = local_boxes[0];
            sub_boxes[1].initBounds();
            INT_TYPE sub_counts[2] = {1,0};
            for (INT_TYPE bit = 0; bit < nboxes-1; ++bit) {
                INT_TYPE dest = (partition_bits>>bit)&1;
                sub_boxes[dest].combine(local_boxes[bit+1]);
                ++sub_counts[dest];
            }
            //printf("Partition bits %u: sub_box[0]: (%f-%f)x(%f-%f)x(%f-%f)\n", uint(partition_bits), sub_boxes[0].vals[0][0], sub_boxes[0].vals[0][1], sub_boxes[0].vals[1][0], sub_boxes[0].vals[1][1], sub_boxes[0].vals[2][0], sub_boxes[0].vals[2][1]);
            //printf("Partition bits %u: sub_box[1]: (%f-%f)x(%f-%f)x(%f-%f)\n", uint(partition_bits), sub_boxes[1].vals[0][0], sub_boxes[1].vals[0][1], sub_boxes[1].vals[1][0], sub_boxes[1].vals[1][1], sub_boxes[1].vals[2][0], sub_boxes[1].vals[2][1]);
            const T heuristic =
                unweightedHeuristic<H>(sub_boxes[0])*sub_counts[0] +
                unweightedHeuristic<H>(sub_boxes[1])*sub_counts[1];
            //printf("Partition bits %u: heuristic = %f (= %f*%u + %f*%u)\n",uint(partition_bits),heuristic, unweightedHeuristic<H>(sub_boxes[0]), uint(sub_counts[0]), unweightedHeuristic<H>(sub_boxes[1]), uint(sub_counts[1]));
            if (best_partition == INT_TYPE(-1) || heuristic < best_heuristic) {
                //printf("    New best\n");
                best_partition = partition_bits;
                best_heuristic = heuristic;
                split_boxes[0] = sub_boxes[0];
                split_boxes[1] = sub_boxes[1];
            }
        }

#if 0 // This isn't actually necessary with the current design, because I changed how the number of subtree nodes is determined.
        // If best_partition is partition_limit-1, there's only 1 box
        // in partition 0.  We should instead put this in partition 1,
        // so that we can help always have the internal node indices first
        // in each node.  That gets used to (fairly) quickly determine
        // the number of nodes in a sub-tree.
        if (best_partition == partition_limit - 1) {
            // Put the first index last.
            SRC_INT_TYPE last_index = indices[0];
            SRC_INT_TYPE* dest_indices = indices;
            SRC_INT_TYPE* local_split_indices = indices + nboxes-1;
            for (; dest_indices != local_split_indices; ++dest_indices) {
                dest_indices[0] = dest_indices[1];
            }
            *local_split_indices = last_index;
            split_indices = local_split_indices;

            // Swap the boxes
            const Box<T,NAXES> temp_box = sub_boxes[0];
            sub_boxes[0] = sub_boxes[1];
            sub_boxes[1] = temp_box;
            return;
        }
#endif

        // Reorder the indices.
        // NOTE: Index 0 is always in partition 0, so can stay put.
        SRC_INT_TYPE local_indices[SMALL_LIMIT-1];
        for (INT_TYPE box = 0; box < nboxes-1; ++box) {
            local_indices[box] = indices[box+1];
        }
        SRC_INT_TYPE* dest_indices = indices+1;
        SRC_INT_TYPE* src_indices = local_indices;
        // Copy partition 0
        for (INT_TYPE bit = 0; bit < nboxes-1; ++bit, ++src_indices) {
            if (!((best_partition>>bit)&1)) {
                //printf("Copying %u into partition 0\n",uint(*src_indices));
                *dest_indices = *src_indices;
                ++dest_indices;
            }
        }
        split_indices = dest_indices;
        // Copy partition 1
        src_indices = local_indices;
        for (INT_TYPE bit = 0; bit < nboxes-1; ++bit, ++src_indices) {
            if ((best_partition>>bit)&1) {
                //printf("Copying %u into partition 1\n",uint(*src_indices));
                *dest_indices = *src_indices;
                ++dest_indices;
            }
        }
        return;
    }

    uint max_axis = 0;
    T max_axis_length = axes_minmax.vals[0][1] - axes_minmax.vals[0][0];
    for (uint axis = 1; axis < NAXES; ++axis) {
        const T axis_length = axes_minmax.vals[axis][1] - axes_minmax.vals[axis][0];
        if (axis_length > max_axis_length) {
            max_axis = axis;
            max_axis_length = axis_length;
        }
    }

    if (!(max_axis_length > T(0))) {
        // All boxes are a single point or NaN.
        // Pick an arbitrary split point.
        split_indices = indices + nboxes/2;
        split_boxes[0] = axes_minmax;
        split_boxes[1] = axes_minmax;
        return;
    }

    const INT_TYPE axis = max_axis;

    constexpr INT_TYPE MID_LIMIT = 2*NSPANS;
    if (nboxes <= MID_LIMIT) {
        // Sort along axis, and try all possible splits.

#if 1
        // First, compute midpoints
        T midpointsx2[MID_LIMIT];
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            midpointsx2[i] = utBoxCenter(boxes[indices[i]], axis);
        }
        SRC_INT_TYPE local_indices[MID_LIMIT];
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            local_indices[i] = i;
        }

        const INT_TYPE chunk_starts[5] = {0, nboxes/4, nboxes/2, INT_TYPE((3*uint64(nboxes))/4), nboxes};

        // For sorting, insertion sort 4 chunks and merge them
        for (INT_TYPE chunk = 0; chunk < 4; ++chunk) {
            const INT_TYPE start = chunk_starts[chunk];
            const INT_TYPE end = chunk_starts[chunk+1];
            for (INT_TYPE i = start+1; i < end; ++i) {
                SRC_INT_TYPE indexi = local_indices[i];
                T vi = midpointsx2[indexi];
                for (INT_TYPE j = start; j < i; ++j) {
                    SRC_INT_TYPE indexj = local_indices[j];
                    T vj = midpointsx2[indexj];
                    if (vi < vj) {
                        do {
                            local_indices[j] = indexi;
                            indexi = indexj;
                            ++j;
                            if (j == i) {
                                local_indices[j] = indexi;
                                break;
                            }
                            indexj = local_indices[j];
                        } while (true);
                        break;
                    }
                }
            }
        }
        // Merge chunks into another buffer
        SRC_INT_TYPE local_indices_temp[MID_LIMIT];
        std::merge(local_indices, local_indices+chunk_starts[1],
            local_indices+chunk_starts[1], local_indices+chunk_starts[2],
            local_indices_temp, [&midpointsx2](const SRC_INT_TYPE a, const SRC_INT_TYPE b)->bool {
            return midpointsx2[a] < midpointsx2[b];
        });
        std::merge(local_indices+chunk_starts[2], local_indices+chunk_starts[3],
            local_indices+chunk_starts[3], local_indices+chunk_starts[4],
            local_indices_temp+chunk_starts[2], [&midpointsx2](const SRC_INT_TYPE a, const SRC_INT_TYPE b)->bool {
            return midpointsx2[a] < midpointsx2[b];
        });
        std::merge(local_indices_temp, local_indices_temp+chunk_starts[2],
            local_indices_temp+chunk_starts[2], local_indices_temp+chunk_starts[4],
            local_indices, [&midpointsx2](const SRC_INT_TYPE a, const SRC_INT_TYPE b)->bool {
            return midpointsx2[a] < midpointsx2[b];
        });

        // Translate local_indices into indices
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            local_indices[i] = indices[local_indices[i]];
        }
        // Copy back
        for (INT_TYPE i = 0; i < nboxes; ++i) {
            indices[i] = local_indices[i];
        }
#else
        std::stable_sort(indices, indices+nboxes, [boxes,max_axis](SRC_INT_TYPE a, SRC_INT_TYPE b)->bool {
            return utBoxCenter(boxes[a], max_axis) < utBoxCenter(boxes[b], max_axis);
        });
#endif

        // Accumulate boxes
        Box<T,NAXES> left_boxes[MID_LIMIT-1];
        Box<T,NAXES> right_boxes[MID_LIMIT-1];
        const INT_TYPE nsplits = nboxes-1;
        Box<T,NAXES> box_accumulator(boxes[local_indices[0]]);
        left_boxes[0] = box_accumulator;
        for (INT_TYPE i = 1; i < nsplits; ++i) {
            box_accumulator.combine(boxes[local_indices[i]]);
            left_boxes[i] = box_accumulator;
        }
        box_accumulator.initBounds(boxes[local_indices[nsplits-1]]);
        right_boxes[nsplits-1] = box_accumulator;
        for (INT_TYPE i = nsplits-1; i > 0; --i) {
            box_accumulator.combine(boxes[local_indices[i]]);
            right_boxes[i-1] = box_accumulator;
        }

        INT_TYPE best_split = 0;
        T best_local_heuristic =
            unweightedHeuristic<H>(left_boxes[0]) +
            unweightedHeuristic<H>(right_boxes[0])*(nboxes-1);
        for (INT_TYPE split = 1; split < nsplits; ++split) {
            const T heuristic =
                unweightedHeuristic<H>(left_boxes[split])*(split+1) +
                unweightedHeuristic<H>(right_boxes[split])*(nboxes-(split+1));
            if (heuristic < best_local_heuristic) {
                best_split = split;
                best_local_heuristic = heuristic;
            }
        }
        split_indices = indices+best_split+1;
        split_boxes[0] = left_boxes[best_split];
        split_boxes[1] = right_boxes[best_split];
        return;
    }

    const T axis_min = axes_minmax.vals[max_axis][0];
    const T axis_length = max_axis_length;
    Box<T,NAXES> span_boxes[NSPANS];
    for (INT_TYPE i = 0; i < NSPANS; ++i) {
        span_boxes[i].initBounds();
    }
    INT_TYPE span_counts[NSPANS];
    for (INT_TYPE i = 0; i < NSPANS; ++i) {
        span_counts[i] = 0;
    }

    const T axis_min_x2 = ut_BoxCentre<BOX_TYPE>::scale*axis_min;
    // NOTE: Factor of 0.5 is factored out of the average when using the average value to determine the span that a box lies in.
    const T axis_index_scale = (T(1.0/ut_BoxCentre<BOX_TYPE>::scale)*NSPANS)/axis_length;
    constexpr INT_TYPE BOX_SPANS_PARALLEL_THRESHOLD = 2048;
    INT_TYPE ntasks = 1;
    if (nboxes >= BOX_SPANS_PARALLEL_THRESHOLD) {
        INT_TYPE nprocessors = UT_Thread::getNumProcessors();
        ntasks = (nprocessors > 1) ? SYSmin(4*nprocessors, nboxes/(BOX_SPANS_PARALLEL_THRESHOLD/2)) : 1;
    }
    if (ntasks == 1) {
        for (INT_TYPE indexi = 0; indexi < nboxes; ++indexi) {
            const auto& box = boxes[indices[indexi]];
            const T sum = utBoxCenter(box, axis);
            const uint span_index = SYSclamp(int((sum-axis_min_x2)*axis_index_scale), int(0), int(NSPANS-1));
            ++span_counts[span_index];
            Box<T,NAXES>& span_box = span_boxes[span_index];
            span_box.combine(box);
        }
    }
    else {
        UT_SmallArray<Box<T,NAXES>> parallel_boxes;
        UT_SmallArray<INT_TYPE> parallel_counts;
        igl::parallel_for(
          nboxes,
          [&parallel_boxes,&parallel_counts](int n)
          {
            parallel_boxes.setSize( NSPANS*n);
            parallel_counts.setSize(NSPANS*n);
            for(int t = 0;t<n;t++)
            {
              for (INT_TYPE i = 0; i < NSPANS; ++i) 
              {
                parallel_boxes[t*NSPANS+i].initBounds();
                parallel_counts[t*NSPANS+i] = 0;
              }
            }
          },
          [&parallel_boxes,&parallel_counts,&boxes,indices,axis,axis_min_x2,axis_index_scale](int j, int t)
          {
            const auto& box = boxes[indices[j]];
            const T sum = utBoxCenter(box, axis);
            const uint span_index = SYSclamp(int((sum-axis_min_x2)*axis_index_scale), int(0), int(NSPANS-1));
            ++parallel_counts[t*NSPANS+span_index];
            Box<T,NAXES>& span_box = parallel_boxes[t*NSPANS+span_index];
            span_box.combine(box);
          },
          [&parallel_boxes,&parallel_counts,&span_boxes,&span_counts](int t)
          {
            for(int i = 0;i<NSPANS;i++)
            {
              span_counts[i] += parallel_counts[t*NSPANS + i];
              span_boxes[i].combine(parallel_boxes[t*NSPANS + i]);
            }
          });

    }

    // Spans 0 to NSPANS-2
    Box<T,NAXES> left_boxes[NSPLITS];
    // Spans 1 to NSPANS-1
    Box<T,NAXES> right_boxes[NSPLITS];

    // Accumulate boxes
    Box<T,NAXES> box_accumulator = span_boxes[0];
    left_boxes[0] = box_accumulator;
    for (INT_TYPE i = 1; i < NSPLITS; ++i) {
        box_accumulator.combine(span_boxes[i]);
        left_boxes[i] = box_accumulator;
    }
    box_accumulator = span_boxes[NSPANS-1];
    right_boxes[NSPLITS-1] = box_accumulator;
    for (INT_TYPE i = NSPLITS-1; i > 0; --i) {
        box_accumulator.combine(span_boxes[i]);
        right_boxes[i-1] = box_accumulator;
    }

    INT_TYPE left_counts[NSPLITS];

    // Accumulate counts
    INT_TYPE count_accumulator = span_counts[0];
    left_counts[0] = count_accumulator;
    for (INT_TYPE spliti = 1; spliti < NSPLITS; ++spliti) {
        count_accumulator += span_counts[spliti];
        left_counts[spliti] = count_accumulator;
    }

    // Check which split is optimal, making sure that at least 1/MIN_FRACTION of all boxes are on each side.
    const INT_TYPE min_count = nboxes/MIN_FRACTION;
    UT_IGL_ASSERT_MSG_P(min_count > 0, "MID_LIMIT above should have been large enough that nboxes would be > MIN_FRACTION");
    const INT_TYPE max_count = ((MIN_FRACTION-1)*uint64(nboxes))/MIN_FRACTION;
    UT_IGL_ASSERT_MSG_P(max_count < nboxes, "I'm not sure how this could happen mathematically, but it needs to be checked.");
    T smallest_heuristic = std::numeric_limits<T>::infinity();
    INT_TYPE split_index = -1;
    for (INT_TYPE spliti = 0; spliti < NSPLITS; ++spliti) {
        const INT_TYPE left_count = left_counts[spliti];
        if (left_count < min_count || left_count > max_count) {
            continue;
        }
        const INT_TYPE right_count = nboxes-left_count;
        const T heuristic =
            left_count*unweightedHeuristic<H>(left_boxes[spliti]) +
            right_count*unweightedHeuristic<H>(right_boxes[spliti]);
        if (heuristic < smallest_heuristic) {
            smallest_heuristic = heuristic;
            split_index = spliti;
        }
    }

    SRC_INT_TYPE*const indices_end = indices+nboxes;

    if (split_index == -1) {
        // No split was anywhere close to balanced, so we fall back to searching for one.

        // First, find the span containing the "balance" point, namely where left_counts goes from
        // being less than min_count to more than max_count.
        // If that's span 0, use max_count as the ordered index to select,
        // if it's span NSPANS-1, use min_count as the ordered index to select,
        // else use nboxes/2 as the ordered index to select.
        //T min_pivotx2 = -std::numeric_limits<T>::infinity();
        //T max_pivotx2 = std::numeric_limits<T>::infinity();
        SRC_INT_TYPE* nth_index;
        if (left_counts[0] > max_count) {
            // Search for max_count ordered index
            nth_index = indices+max_count;
            //max_pivotx2 = max_axis_min_x2 + max_axis_length/(NSPANS/ut_BoxCentre<BOX_TYPE>::scale);
        }
        else if (left_counts[NSPLITS-1] < min_count) {
            // Search for min_count ordered index
            nth_index = indices+min_count;
            //min_pivotx2 = max_axis_min_x2 + max_axis_length - max_axis_length/(NSPANS/ut_BoxCentre<BOX_TYPE>::scale);
        }
        else {
            // Search for nboxes/2 ordered index
            nth_index = indices+nboxes/2;
            //for (INT_TYPE spliti = 1; spliti < NSPLITS; ++spliti) {
            //    // The second condition should be redundant, but is just in case.
            //    if (left_counts[spliti] > max_count || spliti == NSPLITS-1) {
            //        min_pivotx2 = max_axis_min_x2 + spliti*max_axis_length/(NSPANS/ut_BoxCentre<BOX_TYPE>::scale);
            //        max_pivotx2 = max_axis_min_x2 + (spliti+1)*max_axis_length/(NSPANS/ut_BoxCentre<BOX_TYPE>::scale);
            //        break;
            //    }
            //}
        }
        nthElement<T>(boxes,indices,indices+nboxes,max_axis,nth_index);//,min_pivotx2,max_pivotx2);

        split_indices = nth_index;
        Box<T,NAXES> left_box(boxes[indices[0]]);
        for (SRC_INT_TYPE* left_indices = indices+1; left_indices < nth_index; ++left_indices) {
            left_box.combine(boxes[*left_indices]);
        }
        Box<T,NAXES> right_box(boxes[nth_index[0]]);
        for (SRC_INT_TYPE* right_indices = nth_index+1; right_indices < indices_end; ++right_indices) {
            right_box.combine(boxes[*right_indices]);
        }
        split_boxes[0] = left_box;
        split_boxes[1] = right_box;
    }
    else {
        const T pivotx2 = axis_min_x2 + (split_index+1)*axis_length/(NSPANS/ut_BoxCentre<BOX_TYPE>::scale);
        SRC_INT_TYPE* ppivot_start;
        SRC_INT_TYPE* ppivot_end;
        partitionByCentre(boxes,indices,indices+nboxes,max_axis,pivotx2,ppivot_start,ppivot_end);

        split_indices = indices + left_counts[split_index];

        // Ignoring roundoff error, we would have
        // split_indices >= ppivot_start && split_indices <= ppivot_end,
        // but it may not always be in practice.
        if (split_indices >= ppivot_start && split_indices <= ppivot_end) {
            split_boxes[0] = left_boxes[split_index];
            split_boxes[1] = right_boxes[split_index];
            return;
        }

        // Roundoff error changed the split, so we need to recompute the boxes.
        if (split_indices < ppivot_start) {
            split_indices = ppivot_start;
        }
        else {//(split_indices > ppivot_end)
            split_indices = ppivot_end;
        }

        // Emergency checks, just in case
        if (split_indices == indices) {
            ++split_indices;
        }
        else if (split_indices == indices_end) {
            --split_indices;
        }

        Box<T,NAXES> left_box(boxes[indices[0]]);
        for (SRC_INT_TYPE* left_indices = indices+1; left_indices < split_indices; ++left_indices) {
            left_box.combine(boxes[*left_indices]);
        }
        Box<T,NAXES> right_box(boxes[split_indices[0]]);
        for (SRC_INT_TYPE* right_indices = split_indices+1; right_indices < indices_end; ++right_indices) {
            right_box.combine(boxes[*right_indices]);
        }
        split_boxes[0] = left_box;
        split_boxes[1] = right_box;
    }
}

template<uint N>
template<uint PARALLEL_THRESHOLD, typename SRC_INT_TYPE>
inline void BVH<N>::adjustParallelChildNodes(INT_TYPE nparallel, UT_Array<Node>& nodes, Node& node, UT_Array<Node>* parallel_nodes, SRC_INT_TYPE* sub_indices) noexcept
{
  // Alec: No need to parallelize this...
    //UTparallelFor(UT_BlockedRange<INT_TYPE>(0,nparallel), [&node,&nodes,&parallel_nodes,&sub_indices](const UT_BlockedRange<INT_TYPE>& r) {
        INT_TYPE counted_parallel = 0;
        INT_TYPE childi = 0;
        for(int taski = 0;taski < nparallel; taski++)
        {
        //for (INT_TYPE taski = r.begin(), end = r.end(); taski < end; ++taski) {
            // First, find which child this is
            INT_TYPE sub_nboxes;
            for (; childi < N; ++childi) {
                sub_nboxes = sub_indices[childi+1]-sub_indices[childi];
                if (sub_nboxes >= PARALLEL_THRESHOLD) {
                    if (counted_parallel == taski) {
                        break;
                    }
                    ++counted_parallel;
                }
            }
            UT_IGL_ASSERT_P(counted_parallel == taski);

            const UT_Array<Node>& local_nodes = parallel_nodes[counted_parallel];
            INT_TYPE n = local_nodes.size();
            INT_TYPE local_nodes_start = Node::getInternalNum(node.child[childi])+1;
            ++counted_parallel;
            ++childi;

            for (INT_TYPE j = 0; j < n; ++j) {
                Node local_node = local_nodes[j];
                for (INT_TYPE childj = 0; childj < N; ++childj) {
                    INT_TYPE local_child = local_node.child[childj];
                    if (Node::isInternal(local_child) && local_child != Node::EMPTY) {
                        local_child += local_nodes_start;
                        local_node.child[childj] = local_child;
                    }
                }
                nodes[local_nodes_start+j] = local_node;
            }
        }
}

template<uint N>
template<typename T,typename BOX_TYPE,typename SRC_INT_TYPE>
void BVH<N>::nthElement(const BOX_TYPE* boxes, SRC_INT_TYPE* indices, const SRC_INT_TYPE* indices_end, const uint axis, SRC_INT_TYPE*const nth) noexcept {//, const T min_pivotx2, const T max_pivotx2) noexcept {
    while (true) {
        // Choose median of first, middle, and last as the pivot
        T pivots[3] = {
            utBoxCenter(boxes[indices[0]], axis),
            utBoxCenter(boxes[indices[(indices_end-indices)/2]], axis),
            utBoxCenter(boxes[*(indices_end-1)], axis)
        };
        if (pivots[0] < pivots[1]) {
            const T temp = pivots[0];
            pivots[0] = pivots[1];
            pivots[1] = temp;
        }
        if (pivots[0] < pivots[2]) {
            const T temp = pivots[0];
            pivots[0] = pivots[2];
            pivots[2] = temp;
        }
        if (pivots[1] < pivots[2]) {
            const T temp = pivots[1];
            pivots[1] = pivots[2];
            pivots[2] = temp;
        }
        T mid_pivotx2 = pivots[1];
#if 0
        // We limit the pivot, because we know that the true value is between min and max
        if (mid_pivotx2 < min_pivotx2) {
            mid_pivotx2 = min_pivotx2;
        }
        else if (mid_pivotx2 > max_pivotx2) {
            mid_pivotx2 = max_pivotx2;
        }
#endif
        SRC_INT_TYPE* pivot_start;
        SRC_INT_TYPE* pivot_end;
        partitionByCentre(boxes,indices,indices_end,axis,mid_pivotx2,pivot_start,pivot_end);
        if (nth < pivot_start) {
            indices_end = pivot_start;
        }
        else if (nth < pivot_end) {
            // nth is in the middle of the pivot range,
            // which is in the right place, so we're done.
            return;
        }
        else {
            indices = pivot_end;
        }
        if (indices_end <= indices+1) {
            return;
        }
    }
}

template<uint N>
template<typename T,typename BOX_TYPE,typename SRC_INT_TYPE>
void BVH<N>::partitionByCentre(const BOX_TYPE* boxes, SRC_INT_TYPE*const indices, const SRC_INT_TYPE*const indices_end, const uint axis, const T pivotx2, SRC_INT_TYPE*& ppivot_start, SRC_INT_TYPE*& ppivot_end) noexcept {
    // TODO: Consider parallelizing this!

    // First element >= pivot
    SRC_INT_TYPE* pivot_start = indices;
    // First element > pivot
    SRC_INT_TYPE* pivot_end = indices;

    // Loop through forward once
    for (SRC_INT_TYPE* psrc_index = indices; psrc_index != indices_end; ++psrc_index) {
        const T srcsum = utBoxCenter(boxes[*psrc_index], axis);
        if (srcsum < pivotx2) {
            if (psrc_index != pivot_start) {
                if (pivot_start == pivot_end) {
                    // Common case: nothing equal to the pivot
                    const SRC_INT_TYPE temp = *psrc_index;
                    *psrc_index = *pivot_start;
                    *pivot_start = temp;
                }
                else {
                    // Less common case: at least one thing equal to the pivot
                    const SRC_INT_TYPE temp = *psrc_index;
                    *psrc_index = *pivot_end;
                    *pivot_end = *pivot_start;
                    *pivot_start = temp;
                }
            }
            ++pivot_start;
            ++pivot_end;
        }
        else if (srcsum == pivotx2) {
            // Add to the pivot area
            if (psrc_index != pivot_end) {
                const SRC_INT_TYPE temp = *psrc_index;
                *psrc_index = *pivot_end;
                *pivot_end = temp;
            }
            ++pivot_end;
        }
    }
    ppivot_start = pivot_start;
    ppivot_end = pivot_end;
}

#if 0
template<uint N>
void BVH<N>::debugDump() const {
    printf("\nNode 0: {\n");
    UT_WorkBuffer indent;
    indent.append(80, ' ');
    UT_Array<INT_TYPE> stack;
    stack.append(0);
    stack.append(0);
    while (!stack.isEmpty()) {
        int depth = stack.size()/2;
        if (indent.length() < 4*depth) {
            indent.append(4, ' ');
        }
        INT_TYPE cur_nodei = stack[stack.size()-2];
        INT_TYPE cur_i = stack[stack.size()-1];
        if (cur_i == N) {
            printf(indent.buffer()+indent.length()-(4*(depth-1)));
            printf("}\n");
            stack.removeLast();
            stack.removeLast();
            continue;
        }
        ++stack[stack.size()-1];
        Node& cur_node = myRoot[cur_nodei];
        INT_TYPE child_nodei = cur_node.child[cur_i];
        if (Node::isInternal(child_nodei)) {
            if (child_nodei == Node::EMPTY) {
                printf(indent.buffer()+indent.length()-(4*(depth-1)));
                printf("}\n");
                stack.removeLast();
                stack.removeLast();
                continue;
            }
            INT_TYPE internal_node = Node::getInternalNum(child_nodei);
            printf(indent.buffer()+indent.length()-(4*depth));
            printf("Node %u: {\n", uint(internal_node));
            stack.append(internal_node);
            stack.append(0);
            continue;
        }
        else {
            printf(indent.buffer()+indent.length()-(4*depth));
            printf("Tri %u\n", uint(child_nodei));
        }
    }
}
#endif

} // UT namespace
} // End HDK_Sample namespace
}}
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Functions and structures for computing solid angles.
 */

#pragma once

#ifndef __HDK_UT_SolidAngle_h__
#define __HDK_UT_SolidAngle_h__





#include <memory>

namespace igl { 
  /// @private
  namespace FastWindingNumber {
namespace HDK_Sample {

template<typename T>
using UT_Vector2T = UT_FixedVector<T,2>;
template<typename T>
using UT_Vector3T = UT_FixedVector<T,3>;

template <typename T>
SYS_FORCE_INLINE T cross(const UT_Vector2T<T> &v1, const UT_Vector2T<T> &v2)
{
    return v1[0]*v2[1] - v1[1]*v2[0];
}

template <typename T>
SYS_FORCE_INLINE
UT_Vector3T<T> cross(const UT_Vector3T<T> &v1, const UT_Vector3T<T> &v2)
{
    UT_Vector3T<T> result;
    // compute the cross product:
    result[0] = v1[1]*v2[2] - v1[2]*v2[1];
    result[1] = v1[2]*v2[0] - v1[0]*v2[2];
    result[2] = v1[0]*v2[1] - v1[1]*v2[0];
    return result;
}

/// Returns the signed solid angle subtended by triangle abc
/// from query point.
///
/// WARNING: This uses the right-handed normal convention, whereas most of
///          Houdini uses the left-handed normal convention, so either
///          negate the output, or swap b and c if you want it to be
///          positive inside and negative outside.
template<typename T>
inline T UTsignedSolidAngleTri(
    const UT_Vector3T<T> &a,
    const UT_Vector3T<T> &b,
    const UT_Vector3T<T> &c,
    const UT_Vector3T<T> &query)
{
    // Make a, b, and c relative to query
    UT_Vector3T<T> qa = a-query;
    UT_Vector3T<T> qb = b-query;
    UT_Vector3T<T> qc = c-query;

    const T alength = qa.length();
    const T blength = qb.length();
    const T clength = qc.length();

    // If any triangle vertices are coincident with query,
    // query is on the surface, which we treat as no solid angle.
    if (alength == 0 || blength == 0 || clength == 0)
        return T(0);

    // Normalize the vectors
    qa /= alength;
    qb /= blength;
    qc /= clength;

    // The formula on Wikipedia has roughly dot(qa,cross(qb,qc)),
    // but that's unstable when qa, qb, and qc are very close,
    // (e.g. if the input triangle was very far away).
    // This should be equivalent, but more stable.
    const T numerator = dot(qa, cross(qb-qa, qc-qa));

    // If numerator is 0, regardless of denominator, query is on the
    // surface, which we treat as no solid angle.
    if (numerator == 0)
        return T(0);

    const T denominator = T(1) + dot(qa,qb) + dot(qa,qc) + dot(qb,qc);

    return T(2)*SYSatan2(numerator, denominator);
}

template<typename T>
inline T UTsignedSolidAngleQuad(
    const UT_Vector3T<T> &a,
    const UT_Vector3T<T> &b,
    const UT_Vector3T<T> &c,
    const UT_Vector3T<T> &d,
    const UT_Vector3T<T> &query)
{
    // Make a, b, c, and d relative to query
    UT_Vector3T<T> v[4] = {
        a-query,
        b-query,
        c-query,
        d-query
    };

    const T lengths[4] = {
        v[0].length(),
        v[1].length(),
        v[2].length(),
        v[3].length()
    };

    // If any quad vertices are coincident with query,
    // query is on the surface, which we treat as no solid angle.
    // We could add the contribution from the non-planar part,
    // but in the context of a mesh, we'd still miss some, like
    // we do in the triangle case.
    if (lengths[0] == T(0) || lengths[1] == T(0) || lengths[2] == T(0) || lengths[3] == T(0))
        return T(0);

    // Normalize the vectors
    v[0] /= lengths[0];
    v[1] /= lengths[1];
    v[2] /= lengths[2];
    v[3] /= lengths[3];

    // Compute (unnormalized, but consistently-scaled) barycentric coordinates
    // for the query point inside the tetrahedron of points.
    // If 0 or 4 of the coordinates are positive, (or slightly negative), the
    // query is (approximately) inside, so the choice of triangulation matters.
    // Otherwise, the triangulation doesn't matter.

    const UT_Vector3T<T> diag02 = v[2]-v[0];
    const UT_Vector3T<T> diag13 = v[3]-v[1];
    const UT_Vector3T<T> v01 = v[1]-v[0];
    const UT_Vector3T<T> v23 = v[3]-v[2];

    T bary[4];
    bary[0] = dot(v[3],cross(v23,diag13));
    bary[1] = -dot(v[2],cross(v23,diag02));
    bary[2] = -dot(v[1],cross(v01,diag13));
    bary[3] = dot(v[0],cross(v01,diag02));

    const T dot01 = dot(v[0],v[1]);
    const T dot12 = dot(v[1],v[2]);
    const T dot23 = dot(v[2],v[3]);
    const T dot30 = dot(v[3],v[0]);

    T omega = T(0);

    // Equation of a bilinear patch in barycentric coordinates of its
    // tetrahedron is x0*x2 = x1*x3.  Less is one side; greater is other.
    if (bary[0]*bary[2] < bary[1]*bary[3])
    {
        // Split 0-2: triangles 0,1,2 and 0,2,3
        const T numerator012 = bary[3];
        const T numerator023 = bary[1];
        const T dot02 = dot(v[0],v[2]);

        // If numerator is 0, regardless of denominator, query is on the
        // surface, which we treat as no solid angle.
        if (numerator012 != T(0))
        {
            const T denominator012 = T(1) + dot01 + dot12 + dot02;
            omega = SYSatan2(numerator012, denominator012);
        }
        if (numerator023 != T(0))
        {
            const T denominator023 = T(1) + dot02 + dot23 + dot30;
            omega += SYSatan2(numerator023, denominator023);
        }
    }
    else
    {
        // Split 1-3: triangles 0,1,3 and 1,2,3
        const T numerator013 = -bary[2];
        const T numerator123 = -bary[0];
        const T dot13 = dot(v[1],v[3]);

        // If numerator is 0, regardless of denominator, query is on the
        // surface, which we treat as no solid angle.
        if (numerator013 != T(0))
        {
            const T denominator013 = T(1) + dot01 + dot13 + dot30;
            omega = SYSatan2(numerator013, denominator013);
        }
        if (numerator123 != T(0))
        {
            const T denominator123 = T(1) + dot12 + dot23 + dot13;
            omega += SYSatan2(numerator123, denominator123);
        }
    }
    return T(2)*omega;
}

/// Class for quickly approximating signed solid angle of a large mesh
/// from many query points.  This is useful for computing the
/// generalized winding number at many points.
///
/// NOTE: This is currently only instantiated for <float,float>.
template<typename T,typename S>
class UT_SolidAngle
{
public:
    /// This is outlined so that we don't need to include UT_BVHImpl.h
    inline UT_SolidAngle();
    /// This is outlined so that we don't need to include UT_BVHImpl.h
    inline ~UT_SolidAngle();

    /// NOTE: This does not take ownership over triangle_points or positions,
    ///       but does keep pointers to them, so the caller must keep them in
    ///       scope for the lifetime of this structure.
    UT_SolidAngle(
        const int ntriangles,
        const int *const triangle_points,
        const int npoints,
        const UT_Vector3T<S> *const positions,
        const int order = 2)
        : UT_SolidAngle()
    { init(ntriangles, triangle_points, npoints, positions, order); }

    /// Initialize the tree and data.
    /// NOTE: It is safe to call init on a UT_SolidAngle that has had init
    ///       called on it before, to re-initialize it.
    inline void init(
        const int ntriangles,
        const int *const triangle_points,
        const int npoints,
        const UT_Vector3T<S> *const positions,
        const int order = 2);

    /// Frees myTree and myData, and clears the rest.
    inline void clear();

    /// Returns true if this is clear
    bool isClear() const
    { return myNTriangles == 0; }

    /// Returns an approximation of the signed solid angle of the mesh from the specified query_point
    /// accuracy_scale is the value of (maxP/q) beyond which the approximation of the box will be used.
    inline T computeSolidAngle(const UT_Vector3T<T> &query_point, const T accuracy_scale = T(2.0)) const;

private:
    struct BoxData;

    static constexpr uint BVH_N = 4;
    UT_BVH<BVH_N> myTree;
    int myNBoxes;
    int myOrder;
    std::unique_ptr<BoxData[]> myData;
    int myNTriangles;
    const int *myTrianglePoints;
    int myNPoints;
    const UT_Vector3T<S> *myPositions;
};

template<typename T>
inline T UTsignedAngleSegment(
    const UT_Vector2T<T> &a,
    const UT_Vector2T<T> &b,
    const UT_Vector2T<T> &query)
{
    // Make a and b relative to query
    UT_Vector2T<T> qa = a-query;
    UT_Vector2T<T> qb = b-query;

    // If any segment vertices are coincident with query,
    // query is on the segment, which we treat as no angle.
    if (qa.isZero() || qb.isZero())
        return T(0);

    // numerator = |qa||qb|sin(theta)
    const T numerator = cross(qa, qb);

    // If numerator is 0, regardless of denominator, query is on the
    // surface, which we treat as no solid angle.
    if (numerator == 0)
        return T(0);

    // denominator = |qa||qb|cos(theta)
    const T denominator = dot(qa,qb);

    // numerator/denominator = tan(theta)
    return SYSatan2(numerator, denominator);
}

/// Class for quickly approximating signed subtended angle of a large curve
/// from many query points.  This is useful for computing the
/// generalized winding number at many points.
///
/// NOTE: This is currently only instantiated for <float,float>.
template<typename T,typename S>
class UT_SubtendedAngle
{
public:
    /// This is outlined so that we don't need to include UT_BVHImpl.h
    inline UT_SubtendedAngle();
    /// This is outlined so that we don't need to include UT_BVHImpl.h
    inline ~UT_SubtendedAngle();

    /// NOTE: This does not take ownership over segment_points or positions,
    ///       but does keep pointers to them, so the caller must keep them in
    ///       scope for the lifetime of this structure.
    UT_SubtendedAngle(
        const int nsegments,
        const int *const segment_points,
        const int npoints,
        const UT_Vector2T<S> *const positions,
        const int order = 2)
        : UT_SubtendedAngle()
    { init(nsegments, segment_points, npoints, positions, order); }

    /// Initialize the tree and data.
    /// NOTE: It is safe to call init on a UT_SolidAngle that has had init
    ///       called on it before, to re-initialize it.
    inline void init(
        const int nsegments,
        const int *const segment_points,
        const int npoints,
        const UT_Vector2T<S> *const positions,
        const int order = 2);

    /// Frees myTree and myData, and clears the rest.
    inline void clear();

    /// Returns true if this is clear
    bool isClear() const
    { return myNSegments == 0; }

    /// Returns an approximation of the signed solid angle of the mesh from the specified query_point
    /// accuracy_scale is the value of (maxP/q) beyond which the approximation of the box will be used.
    inline T computeAngle(const UT_Vector2T<T> &query_point, const T accuracy_scale = T(2.0)) const;

private:
    struct BoxData;

    static constexpr uint BVH_N = 4;
    UT_BVH<BVH_N> myTree;
    int myNBoxes;
    int myOrder;
    std::unique_ptr<BoxData[]> myData;
    int myNSegments;
    const int *mySegmentPoints;
    int myNPoints;
    const UT_Vector2T<S> *myPositions;
};

} // End HDK_Sample namespace
}}
#endif
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      A wrapper function for the "free" function, used by UT_(Small)Array
 */



#include <stdlib.h>

namespace igl { 
  /// @private
  namespace FastWindingNumber {

// This needs to be here or else the warning suppression doesn't work because
// the templated calling code won't otherwise be compiled until after we've
// already popped the warning.state. So we just always disable this at file
// scope here.
#if defined(__GNUC__) && !defined(__clang__)
    _Pragma("GCC diagnostic push")
    _Pragma("GCC diagnostic ignored \"-Wfree-nonheap-object\"")
#endif
inline void ut_ArrayImplFree(void *p)
{
    free(p);
}
#if defined(__GNUC__) && !defined(__clang__)
    _Pragma("GCC diagnostic pop")
#endif
} }
/*
 * Copyright (c) 2018 Side Effects Software Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * COMMENTS:
 *      Functions and structures for computing solid angles.
 */








#include "parallel_for.h"
#include <type_traits>
#include <utility>

#define SOLID_ANGLE_TIME_PRECOMPUTE 0

#if SOLID_ANGLE_TIME_PRECOMPUTE
#include <UT/UT_StopWatch.h>
#endif

#define SOLID_ANGLE_DEBUG 0
#if SOLID_ANGLE_DEBUG
#include <UT/UT_Debug.h>
#endif

#define TAYLOR_SERIES_ORDER 2

namespace igl { 
  /// @private
  namespace FastWindingNumber {

namespace HDK_Sample {

template<typename T,typename S>
struct UT_SolidAngle<T,S>::BoxData
{
    void clear()
    {
        // Set everything to zero
        memset(this,0,sizeof(*this));
    }

    using Type  = typename std::conditional<BVH_N==4 && std::is_same<T,float>::value, v4uf, UT_FixedVector<T,BVH_N>>::type;
    using SType = typename std::conditional<BVH_N==4 && std::is_same<S,float>::value, v4uf, UT_FixedVector<S,BVH_N>>::type;

    /// An upper bound on the squared distance from myAverageP to the farthest point in the box.
    SType myMaxPDist2;

    /// Centre of mass of the mesh surface in this box
    UT_FixedVector<Type,3> myAverageP;

    /// Unnormalized, area-weighted normal of the mesh in this box
    UT_FixedVector<Type,3> myN;

#if TAYLOR_SERIES_ORDER >= 1
    /// Values for Omega_1
    /// @{
    UT_FixedVector<Type,3> myNijDiag;  // Nxx, Nyy, Nzz
    Type myNxy_Nyx;               // Nxy+Nyx
    Type myNyz_Nzy;               // Nyz+Nzy
    Type myNzx_Nxz;               // Nzx+Nxz
    /// @}
#endif

#if TAYLOR_SERIES_ORDER >= 2
    /// Values for Omega_2
    /// @{
    UT_FixedVector<Type,3> myNijkDiag; // Nxxx, Nyyy, Nzzz
    Type mySumPermuteNxyz;        // (Nxyz+Nxzy+Nyzx+Nyxz+Nzxy+Nzyx) = 2*(Nxyz+Nyzx+Nzxy)
    Type my2Nxxy_Nyxx; // Nxxy+Nxyx+Nyxx = 2Nxxy+Nyxx
    Type my2Nxxz_Nzxx; // Nxxz+Nxzx+Nzxx = 2Nxxz+Nzxx
    Type my2Nyyz_Nzyy; // Nyyz+Nyzy+Nzyy = 2Nyyz+Nzyy
    Type my2Nyyx_Nxyy; // Nyyx+Nyxy+Nxyy = 2Nyyx+Nxyy
    Type my2Nzzx_Nxzz; // Nzzx+Nzxz+Nxzz = 2Nzzx+Nxzz
    Type my2Nzzy_Nyzz; // Nzzy+Nzyz+Nyzz = 2Nzzy+Nyzz
    /// @}
#endif
};

template<typename T,typename S>
inline UT_SolidAngle<T,S>::UT_SolidAngle()
    : myTree()
    , myNBoxes(0)
    , myOrder(2)
    , myData(nullptr)
    , myNTriangles(0)
    , myTrianglePoints(nullptr)
    , myNPoints(0)
    , myPositions(nullptr)
{}

template<typename T,typename S>
inline UT_SolidAngle<T,S>::~UT_SolidAngle()
{
    // Default destruction works, but this needs to be outlined
    // to avoid having to include UT_BVHImpl.h in the header,
    // (for the UT_UniquePtr destructor.)
}

template<typename T,typename S>
inline void UT_SolidAngle<T,S>::init(
    const int ntriangles,
    const int *const triangle_points,
    const int npoints,
    const UT_Vector3T<S> *const positions,
    const int order)
{
#if SOLID_ANGLE_DEBUG
    UTdebugFormat("");
    UTdebugFormat("");
    UTdebugFormat("Building BVH for {} ntriangles on {} points:", ntriangles, npoints);
#endif
    myOrder = order;
    myNTriangles = ntriangles;
    myTrianglePoints = triangle_points;
    myNPoints = npoints;
    myPositions = positions;

#if SOLID_ANGLE_TIME_PRECOMPUTE
    UT_StopWatch timer;
    timer.start();
#endif
    UT_SmallArray<UT::Box<S,3>> triangle_boxes;
    triangle_boxes.setSizeNoInit(ntriangles);
    if (ntriangles < 16*1024)
    {
        const int *cur_triangle_points = triangle_points;
        for (int i = 0; i < ntriangles; ++i, cur_triangle_points += 3)
        {
            UT::Box<S,3> &box = triangle_boxes[i];
            box.initBounds(positions[cur_triangle_points[0]]);
            box.enlargeBounds(positions[cur_triangle_points[1]]);
            box.enlargeBounds(positions[cur_triangle_points[2]]);
        }
    }
    else
    {
      igl::parallel_for(ntriangles,
        [triangle_points,&triangle_boxes,positions](int i)
        {
          const int *cur_triangle_points = triangle_points + i*3;
          UT::Box<S,3> &box = triangle_boxes[i];
          box.initBounds(positions[cur_triangle_points[0]]);
          box.enlargeBounds(positions[cur_triangle_points[1]]);
          box.enlargeBounds(positions[cur_triangle_points[2]]);
        });
    }
#if SOLID_ANGLE_TIME_PRECOMPUTE
    double time = timer.stop();
    UTdebugFormat("{} s to create bounding boxes.", time);
    timer.start();
#endif
    myTree.template init<UT::BVH_Heuristic::BOX_AREA,S,3>(triangle_boxes.array(), ntriangles);
#if SOLID_ANGLE_TIME_PRECOMPUTE
    time = timer.stop();
    UTdebugFormat("{} s to initialize UT_BVH structure.  {} nodes", time, myTree.getNumNodes());
#endif

    //myTree.debugDump();

    const int nnodes = myTree.getNumNodes();

    myNBoxes = nnodes;
    BoxData *box_data = new BoxData[nnodes];
    myData.reset(box_data);

    // Some data are only needed during initialization.
    struct LocalData
    {
        // Bounding box
        UT::Box<S,3> myBox;

        // P and N are needed from each child for computing Nij.
        UT_Vector3T<T> myAverageP;
        UT_Vector3T<T> myAreaP;
        UT_Vector3T<T> myN;

        // Unsigned area is needed for computing the average position.
        T myArea;

#if TAYLOR_SERIES_ORDER >= 1
        // These are needed for computing Nijk.
        UT_Vector3T<T> myNijDiag;
        T myNxy; T myNyx;
        T myNyz; T myNzy;
        T myNzx; T myNxz;
#endif

#if TAYLOR_SERIES_ORDER >= 2
        UT_Vector3T<T> myNijkDiag; // Nxxx, Nyyy, Nzzz
        T mySumPermuteNxyz; // (Nxyz+Nxzy+Nyzx+Nyxz+Nzxy+Nzyx) = 2*(Nxyz+Nyzx+Nzxy)
        T my2Nxxy_Nyxx;     // Nxxy+Nxyx+Nyxx = 2Nxxy+Nyxx
        T my2Nxxz_Nzxx;     // Nxxz+Nxzx+Nzxx = 2Nxxz+Nzxx
        T my2Nyyz_Nzyy;     // Nyyz+Nyzy+Nzyy = 2Nyyz+Nzyy
        T my2Nyyx_Nxyy;     // Nyyx+Nyxy+Nxyy = 2Nyyx+Nxyy
        T my2Nzzx_Nxzz;     // Nzzx+Nzxz+Nxzz = 2Nzzx+Nxzz
        T my2Nzzy_Nyzz;     // Nzzy+Nzyz+Nyzz = 2Nzzy+Nyzz
#endif
    };

    struct PrecomputeFunctors
    {
        BoxData *const myBoxData;
        const UT::Box<S,3> *const myTriangleBoxes;
        const int *const myTrianglePoints;
        const UT_Vector3T<S> *const myPositions;
        const int myOrder;

        PrecomputeFunctors(
            BoxData *box_data,
            const UT::Box<S,3> *triangle_boxes,
            const int *triangle_points,
            const UT_Vector3T<S> *positions,
            const int order)
            : myBoxData(box_data)
            , myTriangleBoxes(triangle_boxes)
            , myTrianglePoints(triangle_points)
            , myPositions(positions)
            , myOrder(order)
        {}
        constexpr SYS_FORCE_INLINE bool pre(const int /*nodei*/, LocalData * /*data_for_parent*/) const
        {
            return true;
        }
        void item(const int itemi, const int /*parent_nodei*/, LocalData &data_for_parent) const
        {
            const UT_Vector3T<S> *const positions = myPositions;
            const int *const cur_triangle_points = myTrianglePoints + 3*itemi;
            const UT_Vector3T<T> a = positions[cur_triangle_points[0]];
            const UT_Vector3T<T> b = positions[cur_triangle_points[1]];
            const UT_Vector3T<T> c = positions[cur_triangle_points[2]];
            const UT_Vector3T<T> ab = b-a;
            const UT_Vector3T<T> ac = c-a;

            const UT::Box<S,3> &triangle_box = myTriangleBoxes[itemi];
            data_for_parent.myBox.initBounds(triangle_box.getMin(), triangle_box.getMax());

            // Area-weighted normal (unnormalized)
            const UT_Vector3T<T> N = T(0.5)*cross(ab,ac);
            const T area2 = N.length2();
            const T area = SYSsqrt(area2);
            const UT_Vector3T<T> P = (a+b+c)/3;
            data_for_parent.myAverageP = P;
            data_for_parent.myAreaP = P*area;
            data_for_parent.myN = N;
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("");
            UTdebugFormat("Triangle {}: P = {}; N = {}; area = {}", itemi, P, N, area);
            UTdebugFormat("             box = {}", data_for_parent.myBox);
#endif

            data_for_parent.myArea = area;
#if TAYLOR_SERIES_ORDER >= 1
            const int order = myOrder;
            if (order < 1)
                return;

            // NOTE: Due to P being at the centroid, triangles have Nij = 0
            //       contributions to Nij.
            data_for_parent.myNijDiag = T(0);
            data_for_parent.myNxy = 0; data_for_parent.myNyx = 0;
            data_for_parent.myNyz = 0; data_for_parent.myNzy = 0;
            data_for_parent.myNzx = 0; data_for_parent.myNxz = 0;
#endif

#if TAYLOR_SERIES_ORDER >= 2
            if (order < 2)
                return;

            // If it's zero-length, the results are zero, so we can skip.
            if (area == 0)
            {
                data_for_parent.myNijkDiag = T(0);
                data_for_parent.mySumPermuteNxyz = 0;
                data_for_parent.my2Nxxy_Nyxx = 0;
                data_for_parent.my2Nxxz_Nzxx = 0;
                data_for_parent.my2Nyyz_Nzyy = 0;
                data_for_parent.my2Nyyx_Nxyy = 0;
                data_for_parent.my2Nzzx_Nxzz = 0;
                data_for_parent.my2Nzzy_Nyzz = 0;
                return;
            }

            // We need to use the NORMALIZED normal to multiply the integrals by.
            UT_Vector3T<T> n = N/area;

            // Figure out the order of a, b, and c in x, y, and z
            // for use in computing the integrals for Nijk.
            UT_Vector3T<T> values[3] = {a, b, c};

            int order_x[3] = {0,1,2};
            if (a[0] > b[0])
                std::swap(order_x[0],order_x[1]);
            if (values[order_x[0]][0] > c[0])
                std::swap(order_x[0],order_x[2]);
            if (values[order_x[1]][0] > values[order_x[2]][0])
                std::swap(order_x[1],order_x[2]);
            T dx = values[order_x[2]][0] - values[order_x[0]][0];

            int order_y[3] = {0,1,2};
            if (a[1] > b[1])
                std::swap(order_y[0],order_y[1]);
            if (values[order_y[0]][1] > c[1])
                std::swap(order_y[0],order_y[2]);
            if (values[order_y[1]][1] > values[order_y[2]][1])
                std::swap(order_y[1],order_y[2]);
            T dy = values[order_y[2]][1] - values[order_y[0]][1];

            int order_z[3] = {0,1,2};
            if (a[2] > b[2])
                std::swap(order_z[0],order_z[1]);
            if (values[order_z[0]][2] > c[2])
                std::swap(order_z[0],order_z[2]);
            if (values[order_z[1]][2] > values[order_z[2]][2])
                std::swap(order_z[1],order_z[2]);
            T dz = values[order_z[2]][2] - values[order_z[0]][2];

            auto &&compute_integrals = [](
                const UT_Vector3T<T> &a,
                const UT_Vector3T<T> &b,
                const UT_Vector3T<T> &c,
                const UT_Vector3T<T> &P,
                T *integral_ii,
                T *integral_ij,
                T *integral_ik,
                const int i)
            {
#if SOLID_ANGLE_DEBUG
                UTdebugFormat("             Splitting on {}; a = {}; b = {}; c = {}", char('x'+i), a, b, c);
#endif
                // NOTE: a, b, and c must be in order of the i axis.
                // We're splitting the triangle at the middle i coordinate.
                const UT_Vector3T<T> oab = b - a;
                const UT_Vector3T<T> oac = c - a;
                const UT_Vector3T<T> ocb = b - c;
                UT_IGL_ASSERT_MSG_P(oac[i] > 0, "This should have been checked by the caller.");
                const T t = oab[i]/oac[i];
                UT_IGL_ASSERT_MSG_P(t >= 0 && t <= 1, "Either sorting must have gone wrong, or there are input NaNs.");

                const int j = (i==2) ? 0 : (i+1);
                const int k = (j==2) ? 0 : (j+1);
                const T jdiff = t*oac[j] - oab[j];
                const T kdiff = t*oac[k] - oab[k];
                UT_Vector3T<T> cross_a;
                cross_a[0] = (jdiff*oab[k] - kdiff*oab[j]);
                cross_a[1] = kdiff*oab[i];
                cross_a[2] = jdiff*oab[i];
                UT_Vector3T<T> cross_c;
                cross_c[0] = (jdiff*ocb[k] - kdiff*ocb[j]);
                cross_c[1] = kdiff*ocb[i];
                cross_c[2] = jdiff*ocb[i];
                const T area_scale_a = cross_a.length();
                const T area_scale_c = cross_c.length();
                const T Pai = a[i] - P[i];
                const T Pci = c[i] - P[i];

                // Integral over the area of the triangle of (pi^2)dA,
                // by splitting the triangle into two at b, the a side
                // and the c side.
                const T int_ii_a = area_scale_a*(T(0.5)*Pai*Pai + T(2.0/3.0)*Pai*oab[i] + T(0.25)*oab[i]*oab[i]);
                const T int_ii_c = area_scale_c*(T(0.5)*Pci*Pci + T(2.0/3.0)*Pci*ocb[i] + T(0.25)*ocb[i]*ocb[i]);
                *integral_ii = int_ii_a + int_ii_c;
#if SOLID_ANGLE_DEBUG
                UTdebugFormat("             integral_{}{}_a = {}; integral_{}{}_c = {}", char('x'+i), char('x'+i), int_ii_a, char('x'+i), char('x'+i), int_ii_c);
#endif

                int jk = j;
                T *integral = integral_ij;
                T diff = jdiff;
                while (true) // This only does 2 iterations, one for j and one for k
                {
                    if (integral)
                    {
                        T obmidj = b[jk] + T(0.5)*diff;
                        T oabmidj = obmidj - a[jk];
                        T ocbmidj = obmidj - c[jk];
                        T Paj = a[jk] - P[jk];
                        T Pcj = c[jk] - P[jk];
                        // Integral over the area of the triangle of (pi*pj)dA
                        const T int_ij_a = area_scale_a*(T(0.5)*Pai*Paj + T(1.0/3.0)*Pai*oabmidj + T(1.0/3.0)*Paj*oab[i] + T(0.25)*oab[i]*oabmidj);
                        const T int_ij_c = area_scale_c*(T(0.5)*Pci*Pcj + T(1.0/3.0)*Pci*ocbmidj + T(1.0/3.0)*Pcj*ocb[i] + T(0.25)*ocb[i]*ocbmidj);
                        *integral = int_ij_a + int_ij_c;
#if SOLID_ANGLE_DEBUG
                        UTdebugFormat("             integral_{}{}_a = {}; integral_{}{}_c = {}", char('x'+i), char('x'+jk), int_ij_a, char('x'+i), char('x'+jk), int_ij_c);
#endif
                    }
                    if (jk == k)
                        break;
                    jk = k;
                    integral = integral_ik;
                    diff = kdiff;
                }
            };

            T integral_xx = 0;
            T integral_xy = 0;
            T integral_yy = 0;
            T integral_yz = 0;
            T integral_zz = 0;
            T integral_zx = 0;
            // Note that if the span of any axis is zero, the integral must be zero,
            // since there's a factor of (p_i-P_i), i.e. value minus average,
            // and every value must be equal to the average, giving zero.
            if (dx > 0)
            {
                compute_integrals(
                    values[order_x[0]], values[order_x[1]], values[order_x[2]], P,
                    &integral_xx, ((dx >= dy && dy > 0) ? &integral_xy : nullptr), ((dx >= dz && dz > 0) ? &integral_zx : nullptr), 0);
            }
            if (dy > 0)
            {
                compute_integrals(
                    values[order_y[0]], values[order_y[1]], values[order_y[2]], P,
                    &integral_yy, ((dy >= dz && dz > 0) ? &integral_yz : nullptr), ((dx < dy && dx > 0) ? &integral_xy : nullptr), 1);
            }
            if (dz > 0)
            {
                compute_integrals(
                    values[order_z[0]], values[order_z[1]], values[order_z[2]], P,
                    &integral_zz, ((dx < dz && dx > 0) ? &integral_zx : nullptr), ((dy < dz && dy > 0) ? &integral_yz : nullptr), 2);
            }

            UT_Vector3T<T> Niii;
            Niii[0] = integral_xx;
            Niii[1] = integral_yy;
            Niii[2] = integral_zz;
            Niii *= n;
            data_for_parent.myNijkDiag = Niii;
            data_for_parent.mySumPermuteNxyz = 2*(n[0]*integral_yz + n[1]*integral_zx + n[2]*integral_xy);
            T Nxxy = n[0]*integral_xy;
            T Nxxz = n[0]*integral_zx;
            T Nyyz = n[1]*integral_yz;
            T Nyyx = n[1]*integral_xy;
            T Nzzx = n[2]*integral_zx;
            T Nzzy = n[2]*integral_yz;
            data_for_parent.my2Nxxy_Nyxx = 2*Nxxy + n[1]*integral_xx;
            data_for_parent.my2Nxxz_Nzxx = 2*Nxxz + n[2]*integral_xx;
            data_for_parent.my2Nyyz_Nzyy = 2*Nyyz + n[2]*integral_yy;
            data_for_parent.my2Nyyx_Nxyy = 2*Nyyx + n[0]*integral_yy;
            data_for_parent.my2Nzzx_Nxzz = 2*Nzzx + n[0]*integral_zz;
            data_for_parent.my2Nzzy_Nyzz = 2*Nzzy + n[1]*integral_zz;
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("             integral_xx = {}; yy = {}; zz = {}", integral_xx, integral_yy, integral_zz);
            UTdebugFormat("             integral_xy = {}; yz = {}; zx = {}", integral_xy, integral_yz, integral_zx);
#endif
#endif
        }

        void post(const int nodei, const int /*parent_nodei*/, LocalData *data_for_parent, const int nchildren, const LocalData *child_data_array) const
        {
            // NOTE: Although in the general case, data_for_parent may be null for the root call,
            //       this functor assumes that it's non-null, so the call below must pass a non-null pointer.

            BoxData &current_box_data = myBoxData[nodei];

            UT_Vector3T<T> N = child_data_array[0].myN;
            ((T*)&current_box_data.myN[0])[0] = N[0];
            ((T*)&current_box_data.myN[1])[0] = N[1];
            ((T*)&current_box_data.myN[2])[0] = N[2];
            UT_Vector3T<T> areaP = child_data_array[0].myAreaP;
            T area = child_data_array[0].myArea;
            UT_Vector3T<T> local_P = child_data_array[0].myAverageP;
            ((T*)&current_box_data.myAverageP[0])[0] = local_P[0];
            ((T*)&current_box_data.myAverageP[1])[0] = local_P[1];
            ((T*)&current_box_data.myAverageP[2])[0] = local_P[2];
            for (int i = 1; i < nchildren; ++i)
            {
                const UT_Vector3T<T> local_N = child_data_array[i].myN;
                N += local_N;
                ((T*)&current_box_data.myN[0])[i] = local_N[0];
                ((T*)&current_box_data.myN[1])[i] = local_N[1];
                ((T*)&current_box_data.myN[2])[i] = local_N[2];
                areaP += child_data_array[i].myAreaP;
                area += child_data_array[i].myArea;
                const UT_Vector3T<T> local_P = child_data_array[i].myAverageP;
                ((T*)&current_box_data.myAverageP[0])[i] = local_P[0];
                ((T*)&current_box_data.myAverageP[1])[i] = local_P[1];
                ((T*)&current_box_data.myAverageP[2])[i] = local_P[2];
            }
            for (int i = nchildren; i < BVH_N; ++i)
            {
                // Set to zero, just to avoid false positives for uses of uninitialized memory.
                ((T*)&current_box_data.myN[0])[i] = 0;
                ((T*)&current_box_data.myN[1])[i] = 0;
                ((T*)&current_box_data.myN[2])[i] = 0;
                ((T*)&current_box_data.myAverageP[0])[i] = 0;
                ((T*)&current_box_data.myAverageP[1])[i] = 0;
                ((T*)&current_box_data.myAverageP[2])[i] = 0;
            }
            data_for_parent->myN = N;
            data_for_parent->myAreaP = areaP;
            data_for_parent->myArea = area;

            UT::Box<S,3> box(child_data_array[0].myBox);
            for (int i = 1; i < nchildren; ++i)
                box.enlargeBounds(child_data_array[i].myBox);

            // Normalize P
            UT_Vector3T<T> averageP;
            if (area > 0)
                averageP = areaP/area;
            else
                averageP = T(0.5)*(box.getMin() + box.getMax());
            data_for_parent->myAverageP = averageP;

            data_for_parent->myBox = box;

            for (int i = 0; i < nchildren; ++i)
            {
                const UT::Box<S,3> &local_box(child_data_array[i].myBox);
                const UT_Vector3T<T> &local_P = child_data_array[i].myAverageP;
                const UT_Vector3T<T> maxPDiff = SYSmax(local_P-UT_Vector3T<T>(local_box.getMin()), UT_Vector3T<T>(local_box.getMax())-local_P);
                ((T*)&current_box_data.myMaxPDist2)[i] = maxPDiff.length2();
            }
            for (int i = nchildren; i < BVH_N; ++i)
            {
                // This child is non-existent.  If we set myMaxPDist2 to infinity, it will never
                // use the approximation, and the traverseVector function can check for EMPTY.
                ((T*)&current_box_data.myMaxPDist2)[i] = std::numeric_limits<T>::infinity();
            }

#if TAYLOR_SERIES_ORDER >= 1
            const int order = myOrder;
            if (order >= 1)
            {
                // We now have the current box's P, so we can adjust Nij and Nijk
                data_for_parent->myNijDiag = child_data_array[0].myNijDiag;
                data_for_parent->myNxy = 0;
                data_for_parent->myNyx = 0;
                data_for_parent->myNyz = 0;
                data_for_parent->myNzy = 0;
                data_for_parent->myNzx = 0;
                data_for_parent->myNxz = 0;
#if TAYLOR_SERIES_ORDER >= 2
                data_for_parent->myNijkDiag = child_data_array[0].myNijkDiag;
                data_for_parent->mySumPermuteNxyz = child_data_array[0].mySumPermuteNxyz;
                data_for_parent->my2Nxxy_Nyxx = child_data_array[0].my2Nxxy_Nyxx;
                data_for_parent->my2Nxxz_Nzxx = child_data_array[0].my2Nxxz_Nzxx;
                data_for_parent->my2Nyyz_Nzyy = child_data_array[0].my2Nyyz_Nzyy;
                data_for_parent->my2Nyyx_Nxyy = child_data_array[0].my2Nyyx_Nxyy;
                data_for_parent->my2Nzzx_Nxzz = child_data_array[0].my2Nzzx_Nxzz;
                data_for_parent->my2Nzzy_Nyzz = child_data_array[0].my2Nzzy_Nyzz;
#endif

                for (int i = 1; i < nchildren; ++i)
                {
                    data_for_parent->myNijDiag += child_data_array[i].myNijDiag;
#if TAYLOR_SERIES_ORDER >= 2
                    data_for_parent->myNijkDiag += child_data_array[i].myNijkDiag;
                    data_for_parent->mySumPermuteNxyz += child_data_array[i].mySumPermuteNxyz;
                    data_for_parent->my2Nxxy_Nyxx += child_data_array[i].my2Nxxy_Nyxx;
                    data_for_parent->my2Nxxz_Nzxx += child_data_array[i].my2Nxxz_Nzxx;
                    data_for_parent->my2Nyyz_Nzyy += child_data_array[i].my2Nyyz_Nzyy;
                    data_for_parent->my2Nyyx_Nxyy += child_data_array[i].my2Nyyx_Nxyy;
                    data_for_parent->my2Nzzx_Nxzz += child_data_array[i].my2Nzzx_Nxzz;
                    data_for_parent->my2Nzzy_Nyzz += child_data_array[i].my2Nzzy_Nyzz;
#endif
                }
                for (int j = 0; j < 3; ++j)
                    ((T*)&current_box_data.myNijDiag[j])[0] = child_data_array[0].myNijDiag[j];
                ((T*)&current_box_data.myNxy_Nyx)[0] = child_data_array[0].myNxy + child_data_array[0].myNyx;
                ((T*)&current_box_data.myNyz_Nzy)[0] = child_data_array[0].myNyz + child_data_array[0].myNzy;
                ((T*)&current_box_data.myNzx_Nxz)[0] = child_data_array[0].myNzx + child_data_array[0].myNxz;
                for (int j = 0; j < 3; ++j)
                    ((T*)&current_box_data.myNijkDiag[j])[0] = child_data_array[0].myNijkDiag[j];
                ((T*)&current_box_data.mySumPermuteNxyz)[0] = child_data_array[0].mySumPermuteNxyz;
                ((T*)&current_box_data.my2Nxxy_Nyxx)[0] = child_data_array[0].my2Nxxy_Nyxx;
                ((T*)&current_box_data.my2Nxxz_Nzxx)[0] = child_data_array[0].my2Nxxz_Nzxx;
                ((T*)&current_box_data.my2Nyyz_Nzyy)[0] = child_data_array[0].my2Nyyz_Nzyy;
                ((T*)&current_box_data.my2Nyyx_Nxyy)[0] = child_data_array[0].my2Nyyx_Nxyy;
                ((T*)&current_box_data.my2Nzzx_Nxzz)[0] = child_data_array[0].my2Nzzx_Nxzz;
                ((T*)&current_box_data.my2Nzzy_Nyzz)[0] = child_data_array[0].my2Nzzy_Nyzz;
                for (int i = 1; i < nchildren; ++i)
                {
                    for (int j = 0; j < 3; ++j)
                        ((T*)&current_box_data.myNijDiag[j])[i] = child_data_array[i].myNijDiag[j];
                    ((T*)&current_box_data.myNxy_Nyx)[i] = child_data_array[i].myNxy + child_data_array[i].myNyx;
                    ((T*)&current_box_data.myNyz_Nzy)[i] = child_data_array[i].myNyz + child_data_array[i].myNzy;
                    ((T*)&current_box_data.myNzx_Nxz)[i] = child_data_array[i].myNzx + child_data_array[i].myNxz;
                    for (int j = 0; j < 3; ++j)
                        ((T*)&current_box_data.myNijkDiag[j])[i] = child_data_array[i].myNijkDiag[j];
                    ((T*)&current_box_data.mySumPermuteNxyz)[i] = child_data_array[i].mySumPermuteNxyz;
                    ((T*)&current_box_data.my2Nxxy_Nyxx)[i] = child_data_array[i].my2Nxxy_Nyxx;
                    ((T*)&current_box_data.my2Nxxz_Nzxx)[i] = child_data_array[i].my2Nxxz_Nzxx;
                    ((T*)&current_box_data.my2Nyyz_Nzyy)[i] = child_data_array[i].my2Nyyz_Nzyy;
                    ((T*)&current_box_data.my2Nyyx_Nxyy)[i] = child_data_array[i].my2Nyyx_Nxyy;
                    ((T*)&current_box_data.my2Nzzx_Nxzz)[i] = child_data_array[i].my2Nzzx_Nxzz;
                    ((T*)&current_box_data.my2Nzzy_Nyzz)[i] = child_data_array[i].my2Nzzy_Nyzz;
                }
                for (int i = nchildren; i < BVH_N; ++i)
                {
                    // Set to zero, just to avoid false positives for uses of uninitialized memory.
                    for (int j = 0; j < 3; ++j)
                        ((T*)&current_box_data.myNijDiag[j])[i] = 0;
                    ((T*)&current_box_data.myNxy_Nyx)[i] = 0;
                    ((T*)&current_box_data.myNyz_Nzy)[i] = 0;
                    ((T*)&current_box_data.myNzx_Nxz)[i] = 0;
                    for (int j = 0; j < 3; ++j)
                        ((T*)&current_box_data.myNijkDiag[j])[i] = 0;
                    ((T*)&current_box_data.mySumPermuteNxyz)[i] = 0;
                    ((T*)&current_box_data.my2Nxxy_Nyxx)[i] = 0;
                    ((T*)&current_box_data.my2Nxxz_Nzxx)[i] = 0;
                    ((T*)&current_box_data.my2Nyyz_Nzyy)[i] = 0;
                    ((T*)&current_box_data.my2Nyyx_Nxyy)[i] = 0;
                    ((T*)&current_box_data.my2Nzzx_Nxzz)[i] = 0;
                    ((T*)&current_box_data.my2Nzzy_Nyzz)[i] = 0;
                }

                for (int i = 0; i < nchildren; ++i)
                {
                    const LocalData &child_data = child_data_array[i];
                    UT_Vector3T<T> displacement = child_data.myAverageP - UT_Vector3T<T>(data_for_parent->myAverageP);
                    UT_Vector3T<T> N = child_data.myN;

                    // Adjust Nij for the change in centre P
                    data_for_parent->myNijDiag += N*displacement;
                    T Nxy = child_data.myNxy + N[0]*displacement[1];
                    T Nyx = child_data.myNyx + N[1]*displacement[0];
                    T Nyz = child_data.myNyz + N[1]*displacement[2];
                    T Nzy = child_data.myNzy + N[2]*displacement[1];
                    T Nzx = child_data.myNzx + N[2]*displacement[0];
                    T Nxz = child_data.myNxz + N[0]*displacement[2];

                    data_for_parent->myNxy += Nxy;
                    data_for_parent->myNyx += Nyx;
                    data_for_parent->myNyz += Nyz;
                    data_for_parent->myNzy += Nzy;
                    data_for_parent->myNzx += Nzx;
                    data_for_parent->myNxz += Nxz;

#if TAYLOR_SERIES_ORDER >= 2
                    if (order >= 2)
                    {
                        // Adjust Nijk for the change in centre P
                        data_for_parent->myNijkDiag += T(2)*displacement*child_data.myNijDiag + displacement*displacement*child_data.myN;
                        data_for_parent->mySumPermuteNxyz += (displacement[0]*(Nyz+Nzy) + displacement[1]*(Nzx+Nxz) + displacement[2]*(Nxy+Nyx));
                        data_for_parent->my2Nxxy_Nyxx +=
                            2*(displacement[1]*child_data.myNijDiag[0] + displacement[0]*child_data.myNxy + N[0]*displacement[0]*displacement[1])
                            + 2*child_data.myNyx*displacement[0] + N[1]*displacement[0]*displacement[0];
                        data_for_parent->my2Nxxz_Nzxx +=
                            2*(displacement[2]*child_data.myNijDiag[0] + displacement[0]*child_data.myNxz + N[0]*displacement[0]*displacement[2])
                            + 2*child_data.myNzx*displacement[0] + N[2]*displacement[0]*displacement[0];
                        data_for_parent->my2Nyyz_Nzyy +=
                            2*(displacement[2]*child_data.myNijDiag[1] + displacement[1]*child_data.myNyz + N[1]*displacement[1]*displacement[2])
                            + 2*child_data.myNzy*displacement[1] + N[2]*displacement[1]*displacement[1];
                        data_for_parent->my2Nyyx_Nxyy +=
                            2*(displacement[0]*child_data.myNijDiag[1] + displacement[1]*child_data.myNyx + N[1]*displacement[1]*displacement[0])
                            + 2*child_data.myNxy*displacement[1] + N[0]*displacement[1]*displacement[1];
                        data_for_parent->my2Nzzx_Nxzz +=
                            2*(displacement[0]*child_data.myNijDiag[2] + displacement[2]*child_data.myNzx + N[2]*displacement[2]*displacement[0])
                            + 2*child_data.myNxz*displacement[2] + N[0]*displacement[2]*displacement[2];
                        data_for_parent->my2Nzzy_Nyzz +=
                            2*(displacement[1]*child_data.myNijDiag[2] + displacement[2]*child_data.myNzy + N[2]*displacement[2]*displacement[1])
                            + 2*child_data.myNyz*displacement[2] + N[1]*displacement[2]*displacement[2];
                    }
#endif
                }
            }
#endif
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("");
            UTdebugFormat("Node {}: nchildren = {}; maxP = {}", nodei, nchildren, SYSsqrt(current_box_data.myMaxPDist2));
            UTdebugFormat("         P = {}; N = {}", current_box_data.myAverageP, current_box_data.myN);
#if TAYLOR_SERIES_ORDER >= 1
            UTdebugFormat("         Nii = {}", current_box_data.myNijDiag);
            UTdebugFormat("         Nxy+Nyx = {}; Nyz+Nzy = {}; Nyz+Nzy = {}", current_box_data.myNxy_Nyx, current_box_data.myNyz_Nzy, current_box_data.myNzx_Nxz);
#if TAYLOR_SERIES_ORDER >= 2
            UTdebugFormat("         Niii = {}; 2(Nxyz+Nyzx+Nzxy) = {}", current_box_data.myNijkDiag, current_box_data.mySumPermuteNxyz);
            UTdebugFormat("         2Nxxy+Nyxx = {}; 2Nxxz+Nzxx = {}", current_box_data.my2Nxxy_Nyxx, current_box_data.my2Nxxz_Nzxx);
            UTdebugFormat("         2Nyyz+Nzyy = {}; 2Nyyx+Nxyy = {}", current_box_data.my2Nyyz_Nzyy, current_box_data.my2Nyyx_Nxyy);
            UTdebugFormat("         2Nzzx+Nxzz = {}; 2Nzzy+Nyzz = {}", current_box_data.my2Nzzx_Nxzz, current_box_data.my2Nzzy_Nyzz);
#endif
#endif
#endif
        }
    };

#if SOLID_ANGLE_TIME_PRECOMPUTE
    timer.start();
#endif
    const PrecomputeFunctors functors(box_data, triangle_boxes.array(), triangle_points, positions, order);
    // NOTE: post-functor relies on non-null data_for_parent, so we have to pass one.
    LocalData local_data;
    myTree.template traverseParallel<LocalData>(4096, functors, &local_data);
    //myTree.template traverse<LocalData>(functors);
#if SOLID_ANGLE_TIME_PRECOMPUTE
    time = timer.stop();
    UTdebugFormat("{} s to precompute coefficients.", time);
#endif
}

template<typename T,typename S>
inline void UT_SolidAngle<T, S>::clear()
{
    myTree.clear();
    myNBoxes = 0;
    myOrder = 2;
    myData.reset();
    myNTriangles = 0;
    myTrianglePoints = nullptr;
    myNPoints = 0;
    myPositions = nullptr;
}

template<typename T,typename S>
inline T UT_SolidAngle<T, S>::computeSolidAngle(const UT_Vector3T<T> &query_point, const T accuracy_scale) const
{
    const T accuracy_scale2 = accuracy_scale*accuracy_scale;

    struct SolidAngleFunctors
    {
        const BoxData *const myBoxData;
        const UT_Vector3T<T> myQueryPoint;
        const T myAccuracyScale2;
        const UT_Vector3T<S> *const myPositions;
        const int *const myTrianglePoints;
        const int myOrder;

        SolidAngleFunctors(
            const BoxData *const box_data,
            const UT_Vector3T<T> &query_point,
            const T accuracy_scale2,
            const int order,
            const UT_Vector3T<S> *const positions,
            const int *const triangle_points)
            : myBoxData(box_data)
            , myQueryPoint(query_point)
            , myAccuracyScale2(accuracy_scale2)
            , myPositions(positions)
            , myTrianglePoints(triangle_points)
            , myOrder(order)
        {}
        uint pre(const int nodei, T *data_for_parent) const
        {
            const BoxData &data = myBoxData[nodei];
            const typename BoxData::Type maxP2 = data.myMaxPDist2;
            UT_FixedVector<typename BoxData::Type,3> q;
            q[0] = typename BoxData::Type(myQueryPoint[0]);
            q[1] = typename BoxData::Type(myQueryPoint[1]);
            q[2] = typename BoxData::Type(myQueryPoint[2]);
            q -= data.myAverageP;
            const typename BoxData::Type qlength2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2];

            // If the query point is within a factor of accuracy_scale of the box radius,
            // it's assumed to be not a good enough approximation, so it needs to descend.
            // TODO: Is there a way to estimate the error?
            static_assert((std::is_same<typename BoxData::Type,v4uf>::value), "FIXME: Implement support for other tuple types!");
            v4uu descend_mask = (qlength2 <= maxP2*myAccuracyScale2);
            uint descend_bitmask = _mm_movemask_ps(V4SF(descend_mask.vector));
            constexpr uint allchildbits = ((uint(1)<<BVH_N)-1);
            if (descend_bitmask == allchildbits)
            {
                *data_for_parent = 0;
                return allchildbits;
            }

            // qlength2 must be non-zero, since it's strictly greater than something.
            // We still need to be careful for NaNs, though, because the 4th power might cause problems.
            const typename BoxData::Type qlength_m2 = typename BoxData::Type(1.0)/qlength2;
            const typename BoxData::Type qlength_m1 = sqrt(qlength_m2);

            // Normalize q to reduce issues with overflow/underflow, since we'd need the 7th power
            // if we didn't normalize, and (1e-6)^-7 = 1e42, which overflows single-precision.
            q *= qlength_m1;

            typename BoxData::Type Omega_approx = -qlength_m2*dot(q,data.myN);
#if TAYLOR_SERIES_ORDER >= 1
            const int order = myOrder;
            if (order >= 1)
            {
                const UT_FixedVector<typename BoxData::Type,3> q2 = q*q;
                const typename BoxData::Type qlength_m3 = qlength_m2*qlength_m1;
                const typename BoxData::Type Omega_1 =
                    qlength_m3*(data.myNijDiag[0] + data.myNijDiag[1] + data.myNijDiag[2]
                        -typename BoxData::Type(3.0)*(dot(q2,data.myNijDiag) +
                            q[0]*q[1]*data.myNxy_Nyx +
                            q[0]*q[2]*data.myNzx_Nxz +
                            q[1]*q[2]*data.myNyz_Nzy));
                Omega_approx += Omega_1;
#if TAYLOR_SERIES_ORDER >= 2
                if (order >= 2)
                {
                    const UT_FixedVector<typename BoxData::Type,3> q3 = q2*q;
                    const typename BoxData::Type qlength_m4 = qlength_m2*qlength_m2;
                    typename BoxData::Type temp0[3] = {
                        data.my2Nyyx_Nxyy+data.my2Nzzx_Nxzz,
                        data.my2Nzzy_Nyzz+data.my2Nxxy_Nyxx,
                        data.my2Nxxz_Nzxx+data.my2Nyyz_Nzyy
                    };
                    typename BoxData::Type temp1[3] = {
                        q[1]*data.my2Nxxy_Nyxx + q[2]*data.my2Nxxz_Nzxx,
                        q[2]*data.my2Nyyz_Nzyy + q[0]*data.my2Nyyx_Nxyy,
                        q[0]*data.my2Nzzx_Nxzz + q[1]*data.my2Nzzy_Nyzz
                    };
                    const typename BoxData::Type Omega_2 =
                        qlength_m4*(typename BoxData::Type(1.5)*dot(q, typename BoxData::Type(3)*data.myNijkDiag + UT_FixedVector<typename BoxData::Type,3>(temp0))
                            -typename BoxData::Type(7.5)*(dot(q3,data.myNijkDiag) + q[0]*q[1]*q[2]*data.mySumPermuteNxyz + dot(q2, UT_FixedVector<typename BoxData::Type,3>(temp1))));
                    Omega_approx += Omega_2;
                }
#endif
            }
#endif

            // If q is so small that we got NaNs and we just have a
            // small bounding box, it needs to descend.
            const v4uu mask = Omega_approx.isFinite() & ~descend_mask;
            Omega_approx = Omega_approx & mask;
            descend_bitmask = (~_mm_movemask_ps(V4SF(mask.vector))) & allchildbits;

            T sum = Omega_approx[0];
            for (int i = 1; i < BVH_N; ++i)
                sum += Omega_approx[i];
            *data_for_parent = sum;

            return descend_bitmask;
        }
        void item(const int itemi, const int /*parent_nodei*/, T &data_for_parent) const
        {
            const UT_Vector3T<S> *const positions = myPositions;
            const int *const cur_triangle_points = myTrianglePoints + 3*itemi;
            const UT_Vector3T<T> a = positions[cur_triangle_points[0]];
            const UT_Vector3T<T> b = positions[cur_triangle_points[1]];
            const UT_Vector3T<T> c = positions[cur_triangle_points[2]];

            data_for_parent = UTsignedSolidAngleTri(a, b, c, myQueryPoint);
        }
        SYS_FORCE_INLINE void post(const int /*nodei*/, const int /*parent_nodei*/, T *data_for_parent, const int nchildren, const T *child_data_array, const uint descend_bits) const
        {
            T sum = (descend_bits&1) ? child_data_array[0] : 0;
            for (int i = 1; i < nchildren; ++i)
                sum += ((descend_bits>>i)&1) ? child_data_array[i] : 0;

            *data_for_parent += sum;
        }
    };
    const SolidAngleFunctors functors(myData.get(), query_point, accuracy_scale2, myOrder, myPositions, myTrianglePoints);

    T sum;
    myTree.traverseVector(functors, &sum);
    return sum;
}

template<typename T,typename S>
struct UT_SubtendedAngle<T,S>::BoxData
{
    void clear()
    {
        // Set everything to zero
        memset(this,0,sizeof(*this));
    }

    using Type  = typename std::conditional<BVH_N==4 && std::is_same<T,float>::value, v4uf, UT_FixedVector<T,BVH_N>>::type;
    using SType = typename std::conditional<BVH_N==4 && std::is_same<S,float>::value, v4uf, UT_FixedVector<S,BVH_N>>::type;

    /// An upper bound on the squared distance from myAverageP to the farthest point in the box.
    SType myMaxPDist2;

    /// Centre of mass of the mesh surface in this box
    UT_FixedVector<Type,2> myAverageP;

    /// Unnormalized, area-weighted normal of the mesh in this box
    UT_FixedVector<Type,2> myN;

    /// Values for Omega_1
    /// @{
    UT_FixedVector<Type,2> myNijDiag;  // Nxx, Nyy
    Type myNxy_Nyx;               // Nxy+Nyx
    /// @}

    /// Values for Omega_2
    /// @{
    UT_FixedVector<Type,2> myNijkDiag; // Nxxx, Nyyy
    Type my2Nxxy_Nyxx; // Nxxy+Nxyx+Nyxx = 2Nxxy+Nyxx
    Type my2Nyyx_Nxyy; // Nyyx+Nyxy+Nxyy = 2Nyyx+Nxyy
    /// @}
};

template<typename T,typename S>
inline UT_SubtendedAngle<T,S>::UT_SubtendedAngle()
    : myTree()
    , myNBoxes(0)
    , myOrder(2)
    , myData(nullptr)
    , myNSegments(0)
    , mySegmentPoints(nullptr)
    , myNPoints(0)
    , myPositions(nullptr)
{}

template<typename T,typename S>
inline UT_SubtendedAngle<T,S>::~UT_SubtendedAngle()
{
    // Default destruction works, but this needs to be outlined
    // to avoid having to include UT_BVHImpl.h in the header,
    // (for the UT_UniquePtr destructor.)
}

template<typename T,typename S>
inline void UT_SubtendedAngle<T,S>::init(
    const int nsegments,
    const int *const segment_points,
    const int npoints,
    const UT_Vector2T<S> *const positions,
    const int order)
{
#if SOLID_ANGLE_DEBUG
    UTdebugFormat("");
    UTdebugFormat("");
    UTdebugFormat("Building BVH for {} segments on {} points:", nsegments, npoints);
#endif
    myOrder = order;
    myNSegments = nsegments;
    mySegmentPoints = segment_points;
    myNPoints = npoints;
    myPositions = positions;

#if SOLID_ANGLE_TIME_PRECOMPUTE
    UT_StopWatch timer;
    timer.start();
#endif
    UT_SmallArray<UT::Box<S,2>> segment_boxes;
    segment_boxes.setSizeNoInit(nsegments);
    if (nsegments < 16*1024)
    {
        const int *cur_segment_points = segment_points;
        for (int i = 0; i < nsegments; ++i, cur_segment_points += 2)
        {
            UT::Box<S,2> &box = segment_boxes[i];
            box.initBounds(positions[cur_segment_points[0]]);
            box.enlargeBounds(positions[cur_segment_points[1]]);
        }
    }
    else
    {
      igl::parallel_for(nsegments,
        [segment_points,&segment_boxes,positions](int i)
        {
          const int *cur_segment_points = segment_points + i*2;
          UT::Box<S,2> &box = segment_boxes[i];
          box.initBounds(positions[cur_segment_points[0]]);
          box.enlargeBounds(positions[cur_segment_points[1]]);
        });
    }
#if SOLID_ANGLE_TIME_PRECOMPUTE
    double time = timer.stop();
    UTdebugFormat("{} s to create bounding boxes.", time);
    timer.start();
#endif
    myTree.template init<UT::BVH_Heuristic::BOX_AREA,S,2>(segment_boxes.array(), nsegments);
#if SOLID_ANGLE_TIME_PRECOMPUTE
    time = timer.stop();
    UTdebugFormat("{} s to initialize UT_BVH structure.  {} nodes", time, myTree.getNumNodes());
#endif

    //myTree.debugDump();

    const int nnodes = myTree.getNumNodes();

    myNBoxes = nnodes;
    BoxData *box_data = new BoxData[nnodes];
    myData.reset(box_data);

    // Some data are only needed during initialization.
    struct LocalData
    {
        // Bounding box
        UT::Box<S,2> myBox;

        // P and N are needed from each child for computing Nij.
        UT_Vector2T<T> myAverageP;
        UT_Vector2T<T> myLengthP;
        UT_Vector2T<T> myN;

        // Unsigned length is needed for computing the average position.
        T myLength;

        // These are needed for computing Nijk.
        UT_Vector2T<T> myNijDiag;
        T myNxy; T myNyx;

        UT_Vector2T<T> myNijkDiag; // Nxxx, Nyyy
        T my2Nxxy_Nyxx;     // Nxxy+Nxyx+Nyxx = 2Nxxy+Nyxx
        T my2Nyyx_Nxyy;     // Nyyx+Nyxy+Nxyy = 2Nyyx+Nxyy
    };

    struct PrecomputeFunctors
    {
        BoxData *const myBoxData;
        const UT::Box<S,2> *const mySegmentBoxes;
        const int *const mySegmentPoints;
        const UT_Vector2T<S> *const myPositions;
        const int myOrder;

        PrecomputeFunctors(
            BoxData *box_data,
            const UT::Box<S,2> *segment_boxes,
            const int *segment_points,
            const UT_Vector2T<S> *positions,
            const int order)
            : myBoxData(box_data)
            , mySegmentBoxes(segment_boxes)
            , mySegmentPoints(segment_points)
            , myPositions(positions)
            , myOrder(order)
        {}
        constexpr SYS_FORCE_INLINE bool pre(const int /*nodei*/, LocalData * /*data_for_parent*/) const
        {
            return true;
        }
        void item(const int itemi, const int /*parent_nodei*/, LocalData &data_for_parent) const
        {
            const UT_Vector2T<S> *const positions = myPositions;
            const int *const cur_segment_points = mySegmentPoints + 2*itemi;
            const UT_Vector2T<T> a = positions[cur_segment_points[0]];
            const UT_Vector2T<T> b = positions[cur_segment_points[1]];
            const UT_Vector2T<T> ab = b-a;

            const UT::Box<S,2> &segment_box = mySegmentBoxes[itemi];
            data_for_parent.myBox = segment_box;

            // Length-weighted normal (unnormalized)
            UT_Vector2T<T> N;
            N[0] = ab[1];
            N[1] = -ab[0];
            const T length2 = ab.length2();
            const T length = SYSsqrt(length2);
            const UT_Vector2T<T> P = T(0.5)*(a+b);
            data_for_parent.myAverageP = P;
            data_for_parent.myLengthP = P*length;
            data_for_parent.myN = N;
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("");
            UTdebugFormat("Triangle {}: P = {}; N = {}; length = {}", itemi, P, N, length);
            UTdebugFormat("             box = {}", data_for_parent.myBox);
#endif

            data_for_parent.myLength = length;
            const int order = myOrder;
            if (order < 1)
                return;

            // NOTE: Due to P being at the centroid, segments have Nij = 0
            //       contributions to Nij.
            data_for_parent.myNijDiag = T(0);
            data_for_parent.myNxy = 0; data_for_parent.myNyx = 0;

            if (order < 2)
                return;

            // If it's zero-length, the results are zero, so we can skip.
            if (length == 0)
            {
                data_for_parent.myNijkDiag = T(0);
                data_for_parent.my2Nxxy_Nyxx = 0;
                data_for_parent.my2Nyyx_Nxyy = 0;
                return;
            }

            T integral_xx = ab[0]*ab[0]/T(12);
            T integral_xy = ab[0]*ab[1]/T(12);
            T integral_yy = ab[1]*ab[1]/T(12);
            data_for_parent.myNijkDiag[0] = integral_xx*N[0];
            data_for_parent.myNijkDiag[1] = integral_yy*N[1];
            T Nxxy = N[0]*integral_xy;
            T Nyxx = N[1]*integral_xx;
            T Nyyx = N[1]*integral_xy;
            T Nxyy = N[0]*integral_yy;
            data_for_parent.my2Nxxy_Nyxx = 2*Nxxy + Nyxx;
            data_for_parent.my2Nyyx_Nxyy = 2*Nyyx + Nxyy;
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("             integral_xx = {}; yy = {}", integral_xx, integral_yy);
            UTdebugFormat("             integral_xy = {}", integral_xy);
#endif
        }

        void post(const int nodei, const int /*parent_nodei*/, LocalData *data_for_parent, const int nchildren, const LocalData *child_data_array) const
        {
            // NOTE: Although in the general case, data_for_parent may be null for the root call,
            //       this functor assumes that it's non-null, so the call below must pass a non-null pointer.

            BoxData &current_box_data = myBoxData[nodei];

            UT_Vector2T<T> N = child_data_array[0].myN;
            ((T*)&current_box_data.myN[0])[0] = N[0];
            ((T*)&current_box_data.myN[1])[0] = N[1];
            UT_Vector2T<T> lengthP = child_data_array[0].myLengthP;
            T length = child_data_array[0].myLength;
            const UT_Vector2T<T> local_P = child_data_array[0].myAverageP;
            ((T*)&current_box_data.myAverageP[0])[0] = local_P[0];
            ((T*)&current_box_data.myAverageP[1])[0] = local_P[1];
            for (int i = 1; i < nchildren; ++i)
            {
                const UT_Vector2T<T> local_N = child_data_array[i].myN;
                N += local_N;
                ((T*)&current_box_data.myN[0])[i] = local_N[0];
                ((T*)&current_box_data.myN[1])[i] = local_N[1];
                lengthP += child_data_array[i].myLengthP;
                length += child_data_array[i].myLength;
                const UT_Vector2T<T> local_P = child_data_array[i].myAverageP;
                ((T*)&current_box_data.myAverageP[0])[i] = local_P[0];
                ((T*)&current_box_data.myAverageP[1])[i] = local_P[1];
            }
            for (int i = nchildren; i < BVH_N; ++i)
            {
                // Set to zero, just to avoid false positives for uses of uninitialized memory.
                ((T*)&current_box_data.myN[0])[i] = 0;
                ((T*)&current_box_data.myN[1])[i] = 0;
                ((T*)&current_box_data.myAverageP[0])[i] = 0;
                ((T*)&current_box_data.myAverageP[1])[i] = 0;
            }
            data_for_parent->myN = N;
            data_for_parent->myLengthP = lengthP;
            data_for_parent->myLength = length;

            UT::Box<S,2> box(child_data_array[0].myBox);
            for (int i = 1; i < nchildren; ++i)
                box.combine(child_data_array[i].myBox);

            // Normalize P
            UT_Vector2T<T> averageP;
            if (length > 0)
                averageP = lengthP/length;
            else
                averageP = T(0.5)*(box.getMin() + box.getMax());
            data_for_parent->myAverageP = averageP;

            data_for_parent->myBox = box;

            for (int i = 0; i < nchildren; ++i)
            {
                const UT::Box<S,2> &local_box(child_data_array[i].myBox);
                const UT_Vector2T<T> &local_P = child_data_array[i].myAverageP;
                const UT_Vector2T<T> maxPDiff = SYSmax(local_P-UT_Vector2T<T>(local_box.getMin()), UT_Vector2T<T>(local_box.getMax())-local_P);
                ((T*)&current_box_data.myMaxPDist2)[i] = maxPDiff.length2();
            }
            for (int i = nchildren; i < BVH_N; ++i)
            {
                // This child is non-existent.  If we set myMaxPDist2 to infinity, it will never
                // use the approximation, and the traverseVector function can check for EMPTY.
                ((T*)&current_box_data.myMaxPDist2)[i] = std::numeric_limits<T>::infinity();
            }

            const int order = myOrder;
            if (order >= 1)
            {
                // We now have the current box's P, so we can adjust Nij and Nijk
                data_for_parent->myNijDiag = child_data_array[0].myNijDiag;
                data_for_parent->myNxy = 0;
                data_for_parent->myNyx = 0;
                data_for_parent->myNijkDiag = child_data_array[0].myNijkDiag;
                data_for_parent->my2Nxxy_Nyxx = child_data_array[0].my2Nxxy_Nyxx;
                data_for_parent->my2Nyyx_Nxyy = child_data_array[0].my2Nyyx_Nxyy;

                for (int i = 1; i < nchildren; ++i)
                {
                    data_for_parent->myNijDiag += child_data_array[i].myNijDiag;
                    data_for_parent->myNijkDiag += child_data_array[i].myNijkDiag;
                    data_for_parent->my2Nxxy_Nyxx += child_data_array[i].my2Nxxy_Nyxx;
                    data_for_parent->my2Nyyx_Nxyy += child_data_array[i].my2Nyyx_Nxyy;
                }
                for (int j = 0; j < 2; ++j)
                    ((T*)&current_box_data.myNijDiag[j])[0] = child_data_array[0].myNijDiag[j];
                ((T*)&current_box_data.myNxy_Nyx)[0] = child_data_array[0].myNxy + child_data_array[0].myNyx;
                for (int j = 0; j < 2; ++j)
                    ((T*)&current_box_data.myNijkDiag[j])[0] = child_data_array[0].myNijkDiag[j];
                ((T*)&current_box_data.my2Nxxy_Nyxx)[0] = child_data_array[0].my2Nxxy_Nyxx;
                ((T*)&current_box_data.my2Nyyx_Nxyy)[0] = child_data_array[0].my2Nyyx_Nxyy;
                for (int i = 1; i < nchildren; ++i)
                {
                    for (int j = 0; j < 2; ++j)
                        ((T*)&current_box_data.myNijDiag[j])[i] = child_data_array[i].myNijDiag[j];
                    ((T*)&current_box_data.myNxy_Nyx)[i] = child_data_array[i].myNxy + child_data_array[i].myNyx;
                    for (int j = 0; j < 2; ++j)
                        ((T*)&current_box_data.myNijkDiag[j])[i] = child_data_array[i].myNijkDiag[j];
                    ((T*)&current_box_data.my2Nxxy_Nyxx)[i] = child_data_array[i].my2Nxxy_Nyxx;
                    ((T*)&current_box_data.my2Nyyx_Nxyy)[i] = child_data_array[i].my2Nyyx_Nxyy;
                }
                for (int i = nchildren; i < BVH_N; ++i)
                {
                    // Set to zero, just to avoid false positives for uses of uninitialized memory.
                    for (int j = 0; j < 2; ++j)
                        ((T*)&current_box_data.myNijDiag[j])[i] = 0;
                    ((T*)&current_box_data.myNxy_Nyx)[i] = 0;
                    for (int j = 0; j < 2; ++j)
                        ((T*)&current_box_data.myNijkDiag[j])[i] = 0;
                    ((T*)&current_box_data.my2Nxxy_Nyxx)[i] = 0;
                    ((T*)&current_box_data.my2Nyyx_Nxyy)[i] = 0;
                }

                for (int i = 0; i < nchildren; ++i)
                {
                    const LocalData &child_data = child_data_array[i];
                    UT_Vector2T<T> displacement = child_data.myAverageP - UT_Vector2T<T>(data_for_parent->myAverageP);
                    UT_Vector2T<T> N = child_data.myN;

                    // Adjust Nij for the change in centre P
                    data_for_parent->myNijDiag += N*displacement;
                    T Nxy = child_data.myNxy + N[0]*displacement[1];
                    T Nyx = child_data.myNyx + N[1]*displacement[0];

                    data_for_parent->myNxy += Nxy;
                    data_for_parent->myNyx += Nyx;

                    if (order >= 2)
                    {
                        // Adjust Nijk for the change in centre P
                        data_for_parent->myNijkDiag += T(2)*displacement*child_data.myNijDiag + displacement*displacement*child_data.myN;
                        data_for_parent->my2Nxxy_Nyxx +=
                            2*(displacement[1]*child_data.myNijDiag[0] + displacement[0]*child_data.myNxy + N[0]*displacement[0]*displacement[1])
                            + 2*child_data.myNyx*displacement[0] + N[1]*displacement[0]*displacement[0];
                        data_for_parent->my2Nyyx_Nxyy +=
                            2*(displacement[0]*child_data.myNijDiag[1] + displacement[1]*child_data.myNyx + N[1]*displacement[1]*displacement[0])
                            + 2*child_data.myNxy*displacement[1] + N[0]*displacement[1]*displacement[1];
                    }
                }
            }
#if SOLID_ANGLE_DEBUG
            UTdebugFormat("");
            UTdebugFormat("Node {}: nchildren = {}; maxP = {}", nodei, nchildren, SYSsqrt(current_box_data.myMaxPDist2));
            UTdebugFormat("         P = {}; N = {}", current_box_data.myAverageP, current_box_data.myN);
            UTdebugFormat("         Nii = {}", current_box_data.myNijDiag);
            UTdebugFormat("         Nxy+Nyx = {}", current_box_data.myNxy_Nyx);
            UTdebugFormat("         Niii = {}", current_box_data.myNijkDiag);
            UTdebugFormat("         2Nxxy+Nyxx = {}; 2Nyyx+Nxyy = {}", current_box_data.my2Nxxy_Nyxx, current_box_data.my2Nyyx_Nxyy);
#endif
        }
    };

#if SOLID_ANGLE_TIME_PRECOMPUTE
    timer.start();
#endif
    const PrecomputeFunctors functors(box_data, segment_boxes.array(), segment_points, positions, order);
    // NOTE: post-functor relies on non-null data_for_parent, so we have to pass one.
    LocalData local_data;
    myTree.template traverseParallel<LocalData>(4096, functors, &local_data);
    //myTree.template traverse<LocalData>(functors);
#if SOLID_ANGLE_TIME_PRECOMPUTE
    time = timer.stop();
    UTdebugFormat("{} s to precompute coefficients.", time);
#endif
}

template<typename T,typename S>
inline void UT_SubtendedAngle<T, S>::clear()
{
    myTree.clear();
    myNBoxes = 0;
    myOrder = 2;
    myData.reset();
    myNSegments = 0;
    mySegmentPoints = nullptr;
    myNPoints = 0;
    myPositions = nullptr;
}

template<typename T,typename S>
inline T UT_SubtendedAngle<T, S>::computeAngle(const UT_Vector2T<T> &query_point, const T accuracy_scale) const
{
    const T accuracy_scale2 = accuracy_scale*accuracy_scale;

    struct AngleFunctors
    {
        const BoxData *const myBoxData;
        const UT_Vector2T<T> myQueryPoint;
        const T myAccuracyScale2;
        const UT_Vector2T<S> *const myPositions;
        const int *const mySegmentPoints;
        const int myOrder;

        AngleFunctors(
            const BoxData *const box_data,
            const UT_Vector2T<T> &query_point,
            const T accuracy_scale2,
            const int order,
            const UT_Vector2T<S> *const positions,
            const int *const segment_points)
            : myBoxData(box_data)
            , myQueryPoint(query_point)
            , myAccuracyScale2(accuracy_scale2)
            , myOrder(order)
            , myPositions(positions)
            , mySegmentPoints(segment_points)
        {}
        uint pre(const int nodei, T *data_for_parent) const
        {
            const BoxData &data = myBoxData[nodei];
            const typename BoxData::Type maxP2 = data.myMaxPDist2;
            UT_FixedVector<typename BoxData::Type,2> q;
            q[0] = typename BoxData::Type(myQueryPoint[0]);
            q[1] = typename BoxData::Type(myQueryPoint[1]);
            q -= data.myAverageP;
            const typename BoxData::Type qlength2 = q[0]*q[0] + q[1]*q[1];

            // If the query point is within a factor of accuracy_scale of the box radius,
            // it's assumed to be not a good enough approximation, so it needs to descend.
            // TODO: Is there a way to estimate the error?
            static_assert((std::is_same<typename BoxData::Type,v4uf>::value), "FIXME: Implement support for other tuple types!");
            v4uu descend_mask = (qlength2 <= maxP2*myAccuracyScale2);
            uint descend_bitmask = _mm_movemask_ps(V4SF(descend_mask.vector));
            constexpr uint allchildbits = ((uint(1)<<BVH_N)-1);
            if (descend_bitmask == allchildbits)
            {
                *data_for_parent = 0;
                return allchildbits;
            }

            // qlength2 must be non-zero, since it's strictly greater than something.
            // We still need to be careful for NaNs, though, because the 4th power might cause problems.
            const typename BoxData::Type qlength_m2 = typename BoxData::Type(1.0)/qlength2;
            const typename BoxData::Type qlength_m1 = sqrt(qlength_m2);

            // Normalize q to reduce issues with overflow/underflow, since we'd need the 6th power
            // if we didn't normalize, and (1e-7)^-6 = 1e42, which overflows single-precision.
            q *= qlength_m1;

            typename BoxData::Type Omega_approx = -qlength_m1*dot(q,data.myN);
            const int order = myOrder;
            if (order >= 1)
            {
                const UT_FixedVector<typename BoxData::Type,2> q2 = q*q;
                const typename BoxData::Type Omega_1 =
                    qlength_m2*(data.myNijDiag[0] + data.myNijDiag[1]
                        -typename BoxData::Type(2.0)*(dot(q2,data.myNijDiag) +
                            q[0]*q[1]*data.myNxy_Nyx));
                Omega_approx += Omega_1;
                if (order >= 2)
                {
                    const UT_FixedVector<typename BoxData::Type,2> q3 = q2*q;
                    const typename BoxData::Type qlength_m3 = qlength_m2*qlength_m1;
                    typename BoxData::Type temp0[2] = {
                        data.my2Nyyx_Nxyy,
                        data.my2Nxxy_Nyxx
                    };
                    typename BoxData::Type temp1[2] = {
                        q[1]*data.my2Nxxy_Nyxx,
                        q[0]*data.my2Nyyx_Nxyy
                    };
                    const typename BoxData::Type Omega_2 =
                        qlength_m3*(dot(q, typename BoxData::Type(3)*data.myNijkDiag + UT_FixedVector<typename BoxData::Type,2>(temp0))
                            -typename BoxData::Type(4.0)*(dot(q3,data.myNijkDiag) + dot(q2, UT_FixedVector<typename BoxData::Type,2>(temp1))));
                    Omega_approx += Omega_2;
                }
            }

            // If q is so small that we got NaNs and we just have a
            // small bounding box, it needs to descend.
            const v4uu mask = Omega_approx.isFinite() & ~descend_mask;
            Omega_approx = Omega_approx & mask;
            descend_bitmask = (~_mm_movemask_ps(V4SF(mask.vector))) & allchildbits;

            T sum = Omega_approx[0];
            for (int i = 1; i < BVH_N; ++i)
                sum += Omega_approx[i];
            *data_for_parent = sum;

            return descend_bitmask;
        }
        void item(const int itemi, const int /*parent_nodei*/, T &data_for_parent) const
        {
            const UT_Vector2T<S> *const positions = myPositions;
            const int *const cur_segment_points = mySegmentPoints + 2*itemi;
            const UT_Vector2T<T> a = positions[cur_segment_points[0]];
            const UT_Vector2T<T> b = positions[cur_segment_points[1]];

            data_for_parent = UTsignedAngleSegment(a, b, myQueryPoint);
        }
        SYS_FORCE_INLINE void post(const int /*nodei*/, const int /*parent_nodei*/, T *data_for_parent, const int nchildren, const T *child_data_array, const uint descend_bits) const
        {
            T sum = (descend_bits&1) ? child_data_array[0] : 0;
            for (int i = 1; i < nchildren; ++i)
                sum += ((descend_bits>>i)&1) ? child_data_array[i] : 0;

            *data_for_parent += sum;
        }
    };
    const AngleFunctors functors(myData.get(), query_point, accuracy_scale2, myOrder, myPositions, mySegmentPoints);

    T sum;
    myTree.traverseVector(functors, &sum);
    return sum;
}

// Instantiate our templates.
//template class UT_SolidAngle<fpreal32,fpreal32>;
// FIXME: The SIMD parts will need to be handled differently in order to support fpreal64.
//template class UT_SolidAngle<fpreal64,fpreal32>;
//template class UT_SolidAngle<fpreal64,fpreal64>;
//template class UT_SubtendedAngle<fpreal32,fpreal32>;
//template class UT_SubtendedAngle<fpreal64,fpreal32>;
//template class UT_SubtendedAngle<fpreal64,fpreal64>;

} // End HDK_Sample namespace
}}
