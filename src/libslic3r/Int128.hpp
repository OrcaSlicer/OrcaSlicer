// This is an excerpt of from the Clipper library by Angus Johnson, see the license below,
// implementing a 64 x 64 -> 128bit multiply, and 128bit addition, subtraction and compare
// operations, to be used with exact geometric predicates.
// The code has been extended by Vojtech Bubnik to use 128 bit intrinsic types
// and/or 64x64->128 intrinsic functions where possible.

/*******************************************************************************
*                                                                              *
* Author    :  Angus Johnson                                                   *
* Version   :  6.2.9                                                           *
* Date      :  16 February 2015                                                *
* Website   :  http://www.angusj.com                                           *
* Copyright :  Angus Johnson 2010-2015                                         *
*                                                                              *
* License:                                                                     *
* Use, modification & distribution is subject to Boost Software License Ver 1. *
* http://www.boost.org/LICENSE_1_0.txt                                         *
*                                                                              *
* Attributions:                                                                *
* The code in this library is an extension of Bala Vatti's clipping algorithm: *
* "A generic solution to polygon clipping"                                     *
* Communications of the ACM, Vol 35, Issue 7 (July 1992) pp 56-63.             *
* http://portal.acm.org/citation.cfm?id=129906                                 *
*                                                                              *
* Computer graphics and geometric modeling: implementation and algorithms      *
* By Max K. Agoston                                                            *
* Springer; 1 edition (January 4, 2005)                                        *
* http://books.google.com/books?q=vatti+clipping+agoston                       *
*                                                                              *
* See also:                                                                    *
* "Polygon Offsetting by Computing Winding Numbers"                            *
* Paper no. DETC2005-85513 pp. 565-575                                         *
* ASME 2005 International Design Engineering Technical Conferences             *
* and Computers and Information in Engineering Conference (IDETC/CIE2005)      *
* September 24-28, 2005 , Long Beach, California, USA                          *
* http://www.me.berkeley.edu/~mcmains/pubs/DAC05OffsetPolygon.pdf              *
*                                                                              *
*******************************************************************************/

#ifndef SLIC3R_INT128_HPP
#define SLIC3R_INT128_HPP
// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #undef assert 
#endif

#include <cassert>
#include <cstdint>
#include <cmath>

#if ! defined(_MSC_VER) && defined(__SIZEOF_INT128__)
	#define HAS_INTRINSIC_128_TYPE
#endif

#if defined(_MSC_VER) && defined(_WIN64)
	#include <intrin.h>
	#pragma intrinsic(_mul128)
#endif

//------------------------------------------------------------------------------
// Int128 class (enables safe math on signed 64bit integers)
// eg Int128 val1((int64_t)9223372036854775807); //ie 2^63 -1
//    Int128 val2((int64_t)9223372036854775807);
//    Int128 val3 = val1 * val2;
//------------------------------------------------------------------------------

class Int128
{

#ifdef HAS_INTRINSIC_128_TYPE

/******************************************** Using the intrinsic 128bit x 128bit multiply ************************************************/

public:
	__int128 value;

	Int128(int64_t lo = 0) : value(lo) {}
	Int128(const Int128 &v) : value(v.value) {}

	Int128& operator=(const int64_t &rhs) { value = rhs; return *this; }

	uint64_t lo()   const { return uint64_t(value); }
	int64_t  hi()   const { return int64_t(value >> 64); }
	int      sign() const { return (value > 0) - (value < 0); }

	bool operator==(const Int128 &rhs) const { return value == rhs.value; }
	bool operator!=(const Int128 &rhs) const { return value != rhs.value; }
	bool operator> (const Int128 &rhs) const { return value >  rhs.value; }
	bool operator< (const Int128 &rhs) const { return value <  rhs.value; }
	bool operator>=(const Int128 &rhs) const { return value >= rhs.value; }
	bool operator<=(const Int128 &rhs) const { return value <= rhs.value; }

	Int128& operator+=(const Int128 &rhs) 		{ value += rhs.value; return *this; }
	Int128  operator+ (const Int128 &rhs) const { return Int128(value + rhs.value); }
	Int128& operator-=(const Int128 &rhs)		{ value -= rhs.value; return *this; }
	Int128  operator -(const Int128 &rhs) const { return Int128(value - rhs.value); }
	Int128  operator -()                  const { return Int128(- value); }

	operator double() 				      const { return double(value); }

	static inline Int128 multiply(int64_t lhs, int64_t rhs) { return Int128(__int128(lhs) * __int128(rhs)); }

	// Evaluate signum of a 2x2 determinant.
	static int sign_determinant_2x2(int64_t a11, int64_t a12, int64_t a21, int64_t a22)
	{
		__int128 det = __int128(a11) * __int128(a22) - __int128(a12) * __int128(a21);
		return (det > 0) - (det < 0);
	}

	// Compare two rational numbers.
	static int compare_rationals(int64_t p1, int64_t q1, int64_t p2, int64_t q2)
	{
		int invert = ((q1 < 0) == (q2 < 0)) ? 1 : -1;
		__int128 det = __int128(p1) * __int128(q2) - __int128(p2) * __int128(q1);
		return ((det > 0) - (det < 0)) * invert;
	}

#else /* HAS_INTRINSIC_128_TYPE */

/******************************************** Splitting the 128bit number into two 64bit words *********************************************/

	Int128(int64_t lo = 0) : m_lo((uint64_t)lo), m_hi((lo < 0) ? -1 : 0) {}
	Int128(const Int128 &val) : m_lo(val.m_lo), m_hi(val.m_hi) {}
	Int128(const int64_t& hi, const uint64_t& lo) : m_lo(lo), m_hi(hi) {}

	Int128& operator = (const int64_t &val)
	{
		m_lo = (uint64_t)val;
		m_hi = (val < 0) ? -1 : 0;
		return *this;
	}

	uint64_t lo()   const { return m_lo; }
	int64_t  hi()   const { return m_hi; }
	int      sign() const { return (m_hi == 0) ? (m_lo > 0) : (m_hi > 0) - (m_hi < 0); }

	bool operator == (const Int128 &val) const { return m_hi == val.m_hi && m_lo == val.m_lo; }
	bool operator != (const Int128 &val) const { return ! (*this == val); }
	bool operator >  (const Int128 &val) const { return (m_hi == val.m_hi) ? m_lo > val.m_lo : m_hi > val.m_hi; }
	bool operator <  (const Int128 &val) const { return (m_hi == val.m_hi) ? m_lo < val.m_lo : m_hi < val.m_hi; }
	bool operator >= (const Int128 &val) const { return ! (*this < val); }
	bool operator <= (const Int128 &val) const { return ! (*this > val); }

	Int128& operator += (const Int128 &rhs)
	{
		m_hi += rhs.m_hi;
		m_lo += rhs.m_lo;
		if (m_lo < rhs.m_lo) m_hi++;
		return *this;
	}

	Int128 operator + (const Int128 &rhs) const
	{
		Int128 result(*this);
		result+= rhs;
		return result;
	}

	Int128& operator -= (const Int128 &rhs)
	{
		*this += -rhs;
		return *this;
	}

	Int128 operator - (const Int128 &rhs) const
	{
		Int128 result(*this);
		result -= rhs;
		return result;
	}

	Int128 operator-() const { return (m_lo == 0) ? Int128(-m_hi, 0) : Int128(~m_hi, ~m_lo + 1); }

	operator double() const
	{
		const double shift64 = 18446744073709551616.0; //2^64
		return (m_hi < 0) ?
		((m_lo == 0) ? 
		(double)m_hi * shift64 :
		-(double)(~m_lo + ~m_hi * shift64)) :
		(double)(m_lo + m_hi * shift64);
	}

	static inline Int128 multiply(int64_t lhs, int64_t rhs)
	{
#if defined(_MSC_VER) && defined(_WIN64)
		// On Visual Studio 64bit, use the _mul128() intrinsic function.
		Int128 result;
	    result.m_lo = (uint64_t)_mul128(lhs, rhs, &result.m_hi);
	    return result;
#else
	    // This branch should only be executed in case there is neither __int16 type nor _mul128 intrinsic
	    // function available. This is mostly on 32bit operating systems.
	    // Use a pure C implementation of _mul128().

		int negate = (lhs < 0) != (rhs < 0);

		if (lhs < 0)
			lhs = -lhs;
		uint64_t int1Hi = uint64_t(lhs) >> 32;
		uint64_t int1Lo = uint64_t(lhs & 0xFFFFFFFF);

		if (rhs < 0)
			rhs = -rhs;
		uint64_t int2Hi = uint64_t(rhs) >> 32;
		uint64_t int2Lo = uint64_t(rhs & 0xFFFFFFFF);

		//because the high (sign) bits in both int1Hi & int2Hi have been zeroed,
		//there's no risk of 64 bit overflow in the following assignment
		//(ie: $7FFFFFFF*$FFFFFFFF + $7FFFFFFF*$FFFFFFFF < 64bits)
		uint64_t a = int1Hi * int2Hi;
		uint64_t b = int1Lo * int2Lo;
		//Result = A shl 64 + C shl 32 + B ...
		uint64_t c = int1Hi * int2Lo + int1Lo * int2Hi;

		Int128 tmp;
		tmp.m_hi = int64_t(a + (c >> 32));
		tmp.m_lo = int64_t(c << 32);
		tmp.m_lo += int64_t(b);
		if (tmp.m_lo < b) 
			++ tmp.m_hi;
		if (negate) 
			tmp = - tmp;
		return tmp;
#endif
	}

	// Evaluate signum of a 2x2 determinant.
	static int sign_determinant_2x2(int64_t a11, int64_t a12, int64_t a21, int64_t a22)
	{
		return (Int128::multiply(a11, a22) - Int128::multiply(a12, a21)).sign();
	}

	// Compare two rational numbers.
	static int compare_rationals(int64_t p1, int64_t q1, int64_t p2, int64_t q2)
	{
		int invert = ((q1 < 0) == (q2 < 0)) ? 1 : -1;
		Int128 det = Int128::multiply(p1, q2) - Int128::multiply(p2, q1);
		return det.sign() * invert;
	}

private:
	uint64_t m_lo;
	int64_t  m_hi;


#endif /* HAS_INTRINSIC_128_TYPE */


/******************************************** Common methods ************************************************/

public:

	// Evaluate signum of a 2x2 determinant, use a numeric filter to avoid 128 bit multiply if possible.
	static int sign_determinant_2x2_filtered(int64_t a11, int64_t a12, int64_t a21, int64_t a22)
	{
		// First try to calculate the determinant over the upper 31 bits.
		// Round p1, p2, q1, q2 to 31 bits.
		int64_t a11s = (a11 + (1 << 31)) >> 32;
		int64_t a12s = (a12 + (1 << 31)) >> 32;
		int64_t a21s = (a21 + (1 << 31)) >> 32;
		int64_t a22s = (a22 + (1 << 31)) >> 32;
		// Result fits 63 bits, it is an approximate of the determinant divided by 2^64.
		int64_t det  = a11s * a22s - a12s * a21s;
		// Maximum absolute of the remainder of the exact determinant, divided by 2^64.
		int64_t err  = ((std::abs(a11s) + std::abs(a12s) + std::abs(a21s) + std::abs(a22s)) << 1) + 1;
		assert(std::abs(det) <= err || ((det > 0) ? 1 : -1) == sign_determinant_2x2(a11, a12, a21, a22));
		return (std::abs(det) > err) ?
			((det > 0) ? 1 : -1) :
			sign_determinant_2x2(a11, a12, a21, a22);
	}

	// Compare two rational numbers, use a numeric filter to avoid 128 bit multiply if possible.
	static int compare_rationals_filtered(int64_t p1, int64_t q1, int64_t p2, int64_t q2)
	{
		// First try to calculate the determinant over the upper 31 bits.
		// Round p1, p2, q1, q2 to 31 bits.
		int     invert = ((q1 < 0) == (q2 < 0)) ? 1 : -1;
		int64_t q1s = (q1 + (1 << 31)) >> 32;
		int64_t q2s = (q2 + (1 << 31)) >> 32;
		if (q1s != 0 && q2s != 0) {
			int64_t p1s = (p1 + (1 << 31)) >> 32;
			int64_t p2s = (p2 + (1 << 31)) >> 32;
			// Result fits 63 bits, it is an approximate of the determinant divided by 2^64.
			int64_t det = p1s * q2s - p2s * q1s;
			// Maximum absolute of the remainder of the exact determinant, divided by 2^64.
			int64_t err = ((std::abs(p1s) + std::abs(q1s) + std::abs(p2s) + std::abs(q2s)) << 1) + 1;
			assert(std::abs(det) <= err || ((det > 0) ? 1 : -1) * invert == compare_rationals(p1, q1, p2, q2));
			if (std::abs(det) > err)
				return ((det > 0) ? 1 : -1) * invert;
		}
		return sign_determinant_2x2(p1, q1, p2, q2) * invert;
	}
};

#endif // SLIC3R_INT128_HPP
