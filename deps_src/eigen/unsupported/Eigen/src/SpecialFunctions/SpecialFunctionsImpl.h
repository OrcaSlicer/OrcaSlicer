// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2015 Eugene Brevdo <ebrevdo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_SPECIAL_FUNCTIONS_H
#define EIGEN_SPECIAL_FUNCTIONS_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {
namespace internal {

//  Parts of this code are based on the Cephes Math Library.
//
//  Cephes Math Library Release 2.8:  June, 2000
//  Copyright 1984, 1987, 1992, 2000 by Stephen L. Moshier
//
//  Permission has been kindly provided by the original author
//  to incorporate the Cephes software into the Eigen codebase:
//
//    From: Stephen Moshier
//    To: Eugene Brevdo
//    Subject: Re: Permission to wrap several cephes functions in Eigen
//
//    Hello Eugene,
//
//    Thank you for writing.
//
//    If your licensing is similar to BSD, the formal way that has been
//    handled is simply to add a statement to the effect that you are incorporating
//    the Cephes software by permission of the author.
//
//    Good luck with your project,
//    Steve

/****************************************************************************
 * Implementation of lgamma, requires C++11/C99                             *
 ****************************************************************************/

template <typename Scalar>
struct lgamma_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(const Scalar) { return Scalar(0); }
};

template <typename Scalar>
struct lgamma_retval {
  typedef Scalar type;
};

#if EIGEN_HAS_C99_MATH
// Since glibc 2.19
#if defined(__GLIBC__) && ((__GLIBC__ >= 2 && __GLIBC_MINOR__ >= 19) || __GLIBC__ > 2) && \
    (defined(_DEFAULT_SOURCE) || defined(_BSD_SOURCE) || defined(_SVID_SOURCE))
#define EIGEN_HAS_LGAMMA_R
#endif

// Glibc versions before 2.19
#if defined(__GLIBC__) && ((__GLIBC__ == 2 && __GLIBC_MINOR__ < 19) || __GLIBC__ < 2) && \
    (defined(_BSD_SOURCE) || defined(_SVID_SOURCE))
#define EIGEN_HAS_LGAMMA_R
#endif

template <>
struct lgamma_impl<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float run(float x) {
#if !defined(EIGEN_GPU_COMPILE_PHASE) && defined(EIGEN_HAS_LGAMMA_R) && !defined(__APPLE__)
    int dummy;
    return ::lgammaf_r(x, &dummy);
#elif defined(SYCL_DEVICE_ONLY)
    return cl::sycl::lgamma(x);
#else
    return ::lgammaf(x);
#endif
  }
};

template <>
struct lgamma_impl<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double run(double x) {
#if !defined(EIGEN_GPU_COMPILE_PHASE) && defined(EIGEN_HAS_LGAMMA_R) && !defined(__APPLE__)
    int dummy;
    return ::lgamma_r(x, &dummy);
#elif defined(SYCL_DEVICE_ONLY)
    return cl::sycl::lgamma(x);
#else
    return ::lgamma(x);
#endif
  }
};

#undef EIGEN_HAS_LGAMMA_R
#endif

/****************************************************************************
 * Implementation of digamma (psi), based on Cephes                         *
 ****************************************************************************/

template <typename Scalar>
struct digamma_retval {
  typedef Scalar type;
};

/*
 *
 * Polynomial evaluation helper for the Psi (digamma) function.
 *
 * digamma_impl_maybe_poly::run(s) evaluates the asymptotic Psi expansion for
 * input Scalar s, assuming s is above 10.0.
 *
 * If s is above a certain threshold for the given Scalar type, zero
 * is returned.  Otherwise the polynomial is evaluated with enough
 * coefficients for results matching Scalar machine precision.
 *
 *
 */
template <typename Scalar>
struct digamma_impl_maybe_poly {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(const Scalar) { return Scalar(0); }
};

template <>
struct digamma_impl_maybe_poly<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float run(const float s) {
    constexpr float A[] = {-4.16666666666666666667E-3f, 3.96825396825396825397E-3f, -8.33333333333333333333E-3f,
                           8.33333333333333333333E-2f};

    float z;
    if (s < 1.0e8f) {
      z = 1.0f / (s * s);
      return z * internal::ppolevl<float, 3>::run(z, A);
    } else
      return 0.0f;
  }
};

template <>
struct digamma_impl_maybe_poly<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double run(const double s) {
    constexpr double A[] = {8.33333333333333333333E-2,  -2.10927960927960927961E-2, 7.57575757575757575758E-3,
                            -4.16666666666666666667E-3, 3.96825396825396825397E-3,  -8.33333333333333333333E-3,
                            8.33333333333333333333E-2};

    double z;
    if (s < 1.0e17) {
      z = 1.0 / (s * s);
      return z * internal::ppolevl<double, 6>::run(z, A);
    } else
      return 0.0;
  }
};

template <typename Scalar>
struct digamma_impl {
  EIGEN_DEVICE_FUNC static Scalar run(Scalar x) {
    /*
     *
     *     Psi (digamma) function (modified for Eigen)
     *
     *
     * SYNOPSIS:
     *
     * double x, y, psi();
     *
     * y = psi( x );
     *
     *
     * DESCRIPTION:
     *
     *              d      -
     *   psi(x)  =  -- ln | (x)
     *              dx
     *
     * is the logarithmic derivative of the gamma function.
     * For integer x,
     *                   n-1
     *                    -
     * psi(n) = -EUL  +   >  1/k.
     *                    -
     *                   k=1
     *
     * If x is negative, it is transformed to a positive argument by the
     * reflection formula  psi(1-x) = psi(x) + pi cot(pi x).
     * For general positive x, the argument is made greater than 10
     * using the recurrence  psi(x+1) = psi(x) + 1/x.
     * Then the following asymptotic expansion is applied:
     *
     *                           inf.   B
     *                            -      2k
     * psi(x) = log(x) - 1/2x -   >   -------
     *                            -        2k
     *                           k=1   2k x
     *
     * where the B2k are Bernoulli numbers.
     *
     * ACCURACY (float):
     *    Relative error (except absolute when |psi| < 1):
     * arithmetic   domain     # trials      peak         rms
     *    IEEE      0,30        30000       1.3e-15     1.4e-16
     *    IEEE      -30,0       40000       1.5e-15     2.2e-16
     *
     * ACCURACY (double):
     *    Absolute error,  relative when |psi| > 1 :
     * arithmetic   domain     # trials      peak         rms
     *    IEEE      -33,0        30000      8.2e-7      1.2e-7
     *    IEEE      0,33        100000      7.3e-7      7.7e-8
     *
     * ERROR MESSAGES:
     *     message         condition      value returned
     * psi singularity    x integer <=0      INFINITY
     */

    Scalar p, q, nz, s, w, y;
    bool negative = false;

    const Scalar nan = NumTraits<Scalar>::quiet_NaN();
    const Scalar m_pi = Scalar(EIGEN_PI);

    const Scalar zero = Scalar(0);
    const Scalar one = Scalar(1);
    const Scalar half = Scalar(0.5);
    nz = zero;

    if (x <= zero) {
      negative = true;
      q = x;
      p = numext::floor(q);
      if (p == q) {
        return nan;
      }
      /* Remove the zeros of tan(m_pi x)
       * by subtracting the nearest integer from x
       */
      nz = q - p;
      if (nz != half) {
        if (nz > half) {
          p += one;
          nz = q - p;
        }
        nz = m_pi / numext::tan(m_pi * nz);
      } else {
        nz = zero;
      }
      x = one - x;
    }

    /* use the recurrence psi(x+1) = psi(x) + 1/x. */
    s = x;
    w = zero;
    while (s < Scalar(10)) {
      w += one / s;
      s += one;
    }

    y = digamma_impl_maybe_poly<Scalar>::run(s);

    y = numext::log(s) - (half / s) - y - w;

    return (negative) ? y - nz : y;
  }
};

/***************************************************************************
 * Implementation of erfc.
 ****************************************************************************/
template <typename Scalar>
struct generic_fast_erfc {
  template <typename T>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T run(const T& x_in);
};

template <>
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_fast_erfc<float>::run(const T& x_in) {
  constexpr float kClamp = 11.0f;
  const T x = pmin(pmax(x_in, pset1<T>(-kClamp)), pset1<T>(kClamp));

  // erfc(x) = 1 + x * S(x^2), |x| <= 1.
  //
  // Coefficients for S and T generated with Rminimax command:
  // ./ratapprox --function="erfc(x)-1" --dom='[-1,1]' --type=[11,0] --num="odd"
  //   --numF="[SG]" --denF="[SG]" --log --dispCoeff="dec"
  constexpr float alpha[] = {5.61802298761904239654541015625e-04, -4.91381669417023658752441406250e-03,
                             2.67075151205062866210937500000e-02, -1.12800106406211853027343750000e-01,
                             3.76122951507568359375000000000e-01, -1.12837910652160644531250000000e+00};
  const T x2 = pmul(x, x);
  const T one = pset1<T>(1.0f);
  const T erfc_small = pmadd(x, ppolevl<T, 5>::run(x2, alpha), one);

  // Return early if we don't need the more expensive approximation for any
  // entry in a.
  const T x_abs_gt_one_mask = pcmp_lt(one, x2);
  if (!predux_any(x_abs_gt_one_mask)) return erfc_small;

  // erfc(x) = exp(-x^2) * 1/x * P(1/x^2) / Q(1/x^2), 1 < x < 9.
  //
  // Coefficients for P and Q generated with Rminimax command:
  //   ./ratapprox --function="erfc(1/sqrt(x))*exp(1/x)/sqrt(x)"
  //     --dom='[0.01,1]' --type=[3,4] --numF="[SG]" --denF="[SG]" --log
  //     --dispCoeff="dec"
  constexpr float gamma[] = {1.0208116471767425537109375e-01f, 4.2920666933059692382812500e-01f,
                             3.2379078865051269531250000e-01f, 5.3971976041793823242187500e-02f};
  constexpr float delta[] = {1.7251677811145782470703125e-02f, 3.9137163758277893066406250e-01f,
                             1.0000000000000000000000000e+00f, 6.2173241376876831054687500e-01f,
                             9.5662862062454223632812500e-02f};
  const T x2_lo = twoprod_low(x, x, x2);
  // Here we use that
  //   exp(-x^2) = exp(-(x2+x2_lo)^2) ~= exp(-x2)*exp(-x2_lo) ~= exp(-x2)*(1-x2_lo)
  // since x2_lo < kClamp * eps << 1 in the region we care about. This trick reduces the max error
  // from 34 ulps to below 5 ulps.
  const T exp2_hi = pexp(pnegate(x2));
  const T z = pnmadd(exp2_hi, x2_lo, exp2_hi);
  const T q2 = preciprocal(x2);
  const T num = ppolevl<T, 3>::run(q2, gamma);
  const T denom = pmul(x, ppolevl<T, 4>::run(q2, delta));
  const T r = pdiv(num, denom);
  const T maybe_two = pselect(pcmp_lt(x, pset1<T>(0.0f)), pset1<T>(2.0f), pset1<T>(0.0f));
  const T erfc_large = pmadd(z, r, maybe_two);
  return pselect(x_abs_gt_one_mask, erfc_large, erfc_small);
}

// Computes erf(x)/x for |x| <= 1. Used by both erf and erfc implementations.
// Takes x2 = x^2 as input.
//
// PRECONDITION: x2 <= 1.
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T erf_over_x_double_small(const T& x2) {
  // erf(x)/x =  S(x^2) / T(x^2), x^2 <= 1.
  //
  // Coefficients for S and T generated with Rminimax command:
  //  ./ratapprox --function="erf(x)" --dom='[-1,1]' --type=[9,10]
  //  --num="odd" --numF="[D]" --den="even" --denF="[D]" --log --dispCoeff="dec"
  constexpr double alpha[] = {1.9493725660006057018823477644531294572516344487667083740234375e-04,
                              1.8272566210022942682217328425053892715368419885635375976562500e-03,
                              4.5303363351690106863856044583371840417385101318359375000000000e-02,
                              1.4215015503619179981775744181504705920815467834472656250000000e-01,
                              1.1283791670955125585606992899556644260883331298828125000000000e+00};
  constexpr double beta[] = {2.0294484101083099089526257108317963684385176748037338256835938e-05,
                             6.8117805899186819641732970609382391558028757572174072265625000e-04,
                             1.0582026056098614921752165685120417037978768348693847656250000e-02,
                             9.3252603143757495374188692949246615171432495117187500000000000e-02,
                             4.5931062818368939559832142549566924571990966796875000000000000e-01,
                             1.0};
  const T num_small = ppolevl<T, 4>::run(x2, alpha);
  const T denom_small = ppolevl<T, 5>::run(x2, beta);
  return pdiv(num_small, denom_small);
}

// erfc(x) = exp(-x^2) * 1/x * P(1/x^2) / Q(1/x^2), 1 < x < 28.
//
// Coefficients for P and Q generated with Rminimax command:
//  ./ratapprox --function="erfc(1/sqrt(x))*exp(1/x)/sqrt(x)"  --dom='[0.0013717,1]' --type=[9,9] --numF="[D]"
//  --denF="[D]" --log --dispCoeff="dec"
//
// PRECONDITION: 1 < x < 28.
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T erfc_double_large(const T& x, const T& x2) {
  constexpr double gamma[] = {1.5252844933226974316088642158462107545346952974796295166015625e-04,
                              1.0909912393738931124520519233556115068495273590087890625000000e-02,
                              1.0628604636755033252537572252549580298364162445068359375000000e-01,
                              3.3492472973137982217295416376146022230386734008789062500000000e-01,
                              4.5065776215933289750026347064704168587923049926757812500000000e-01,
                              2.9433039130294824659017649537418037652969360351562500000000000e-01,
                              9.8792676360600226170838311645638896152377128601074218750000000e-02,
                              1.7095935395503719655962981960328761488199234008789062500000000e-02,
                              1.4249109729504577659398023570247460156679153442382812500000000e-03,
                              4.4567378313647954771875570045835956989321857690811157226562500e-05};
  constexpr double delta[] = {2.041985103115789845773520028160419315099716186523437500000000e-03,
                              5.316030659946043707142493417450168635696172714233398437500000e-02,
                              3.426242193784684864077405563875799998641014099121093750000000e-01,
                              8.565637124308049799026321124983951449394226074218750000000000e-01,
                              1.000000000000000000000000000000000000000000000000000000000000e+00,
                              5.968805280570776972126623149961233139038085937500000000000000e-01,
                              1.890922854723317836356244470152887515723705291748046875000000e-01,
                              3.152505418656005586885981983868987299501895904541015625000000e-02,
                              2.565085751861882583380047861965067568235099315643310546875000e-03,
                              7.899362131678837697403017248376499992446042597293853759765625e-05};
  // Compute exp(-x^2).
  const T x2_lo = twoprod_low(x, x, x2);
  // Here we use that
  //   exp(-x^2) = exp(-(x2+x2_lo)^2) ~= exp(-x2)*exp(-x2_lo) ~= exp(-x2)*(1-x2_lo)
  // since x2_lo < kClamp *eps << 1 in the region we care about. This trick reduces the max error
  // from 258 ulps to below 7 ulps.
  const T exp2_hi = pexp(pnegate(x2));
  const T z = pnmadd(exp2_hi, x2_lo, exp2_hi);
  // Compute r = P / Q.
  const T q2 = preciprocal(x2);
  const T num_large = ppolevl<T, 9>::run(q2, gamma);
  const T denom_large = pmul(x, ppolevl<T, 9>::run(q2, delta));
  const T r = pdiv(num_large, denom_large);
  const T maybe_two = pselect(pcmp_lt(x, pset1<T>(0.0)), pset1<T>(2.0), pset1<T>(0.0));
  return pmadd(z, r, maybe_two);
}

template <>
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_fast_erfc<double>::run(const T& x_in) {
  // Clamp x to [-28:28] beyond which erfc(x) is either two or zero (below the underflow threshold).
  // This avoids having to deal with twoprod(x,x) producing NaN for sufficiently large x.
  constexpr double kClamp = 28.0;
  const T x = pmin(pmax(x_in, pset1<T>(-kClamp)), pset1<T>(kClamp));

  // For |x| < 1, we use erfc(x) = 1 - erf(x).
  const T x2 = pmul(x, x);
  const T one = pset1<T>(1.0);
  const T erfc_small = pnmadd(x, erf_over_x_double_small(x2), one);

  // Return early if we don't need the more expensive approximation for any
  // entry in a.
  const T x_abs_gt_one_mask = pcmp_lt(one, x2);
  if (!predux_any(x_abs_gt_one_mask)) return erfc_small;

  const T erfc_large = erfc_double_large(x, x2);
  return pselect(x_abs_gt_one_mask, erfc_large, erfc_small);
}

template <typename T>
struct erfc_impl {
  typedef typename unpacket_traits<T>::type Scalar;
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE T run(const T& x) { return generic_fast_erfc<Scalar>::run(x); }
};

template <typename Scalar>
struct erfc_retval {
  typedef Scalar type;
};

#if EIGEN_HAS_C99_MATH
template <>
struct erfc_impl<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float run(const float x) {
#if defined(SYCL_DEVICE_ONLY)
    return cl::sycl::erfc(x);
#else
    return generic_fast_erfc<float>::run(x);
#endif
  }
};

template <>
struct erfc_impl<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double run(const double x) {
#if defined(SYCL_DEVICE_ONLY)
    return cl::sycl::erfc(x);
#else
    return generic_fast_erfc<double>::run(x);
#endif
  }
};
#endif  // EIGEN_HAS_C99_MATH

/****************************************************************************
 * Implementation of erf.
 ****************************************************************************/

template <typename Scalar>
struct generic_fast_erf {
  template <typename T>
  static EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T run(const T& x_in);
};

/** \internal \returns the error function of \a a (coeff-wise)
    This uses a 11/10-degree rational interpolantand is accurate to 3 ulp for
    normalized floats.

    This implementation works on both scalars and SIMD "packets".
*/
template <>
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_fast_erf<float>::run(const T& x) {
  // The monomial coefficients of the numerator polynomial (odd).
  constexpr float alpha[] = {2.123732201653183437883853912353515625e-06f, 2.861979592125862836837768554687500000e-04f,
                             3.658048342913389205932617187500000000e-03f, 5.243302136659622192382812500000000000e-02f,
                             1.874160766601562500000000000000000000e-01f, 1.128379106521606445312500000000000000e+00f};

  // The monomial coefficients of the denominator polynomial (even).
  constexpr float beta[] = {3.89185734093189239501953125000e-05f, 1.14329601638019084930419921875e-03f,
                            1.47520881146192550659179687500e-02f, 1.12945675849914550781250000000e-01f,
                            4.99425798654556274414062500000e-01f, 1.0f};

  // Since the polynomials are odd/even, we need x^2.
  // Since erf(4) == 1 in float, we clamp x^2 to 16 to avoid
  // computing Inf/Inf below.
  const T x2 = pmin(pset1<T>(16.0f), pmul(x, x));

  // Evaluate the numerator polynomial p.
  T p = ppolevl<T, 5>::run(x2, alpha);
  p = pmul(x, p);

  // Evaluate the denominator polynomial p.
  T q = ppolevl<T, 5>::run(x2, beta);
  const T r = pdiv(p, q);

  // Clamp to [-1:1].
  return pmax(pmin(r, pset1<T>(1.0f)), pset1<T>(-1.0f));
}

template <>
template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_fast_erf<double>::run(const T& x) {
  T x2 = pmul(x, x);
  T erf_small = pmul(x, erf_over_x_double_small(x2));

  // Return early if we don't need the more expensive approximation for any
  // entry in a.
  const T one = pset1<T>(1.0);
  const T x_abs_gt_one_mask = pcmp_lt(one, x2);
  if (!predux_any(x_abs_gt_one_mask)) return erf_small;

  // For |x| > 1, use erf(x) = 1 - erfc(x).
  const T erf_large = psub(one, erfc_double_large(x, x2));
  return pselect(x_abs_gt_one_mask, erf_large, erf_small);
}

template <typename T>
struct erf_impl {
  typedef typename unpacket_traits<T>::type Scalar;
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE T run(const T& x) { return generic_fast_erf<Scalar>::run(x); }
};

template <typename Scalar>
struct erf_retval {
  typedef Scalar type;
};

#if EIGEN_HAS_C99_MATH
template <>
struct erf_impl<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float run(const float x) {
#if defined(SYCL_DEVICE_ONLY)
    return cl::sycl::erf(x);
#else
    return generic_fast_erf<float>::run(x);
#endif
  }
};

template <>
struct erf_impl<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double run(const double x) {
#if defined(SYCL_DEVICE_ONLY)
    return cl::sycl::erf(x);
#else
    return generic_fast_erf<double>::run(x);
#endif
  }
};
#endif  // EIGEN_HAS_C99_MATH

/***************************************************************************
 * Implementation of ndtri.                                                 *
 ****************************************************************************/

/* Inverse of Normal distribution function (modified for Eigen).
 *
 *
 * SYNOPSIS:
 *
 * double x, y, ndtri();
 *
 * x = ndtri( y );
 *
 *
 *
 * DESCRIPTION:
 *
 * Returns the argument, x, for which the area under the
 * Gaussian probability density function (integrated from
 * minus infinity to x) is equal to y.
 *
 *
 * For small arguments 0 < y < exp(-2), the program computes
 * z = sqrt( -2.0 * log(y) );  then the approximation is
 * x = z - log(z)/z  - (1/z) P(1/z) / Q(1/z).
 * There are two rational functions P/Q, one for 0 < y < exp(-32)
 * and the other for y up to exp(-2).  For larger arguments,
 * w = y - 0.5, and  x/sqrt(2pi) = w + w**3 R(w**2)/S(w**2)).
 *
 *
 * ACCURACY:
 *
 *                      Relative error:
 * arithmetic   domain        # trials      peak         rms
 *    DEC      0.125, 1         5500       9.5e-17     2.1e-17
 *    DEC      6e-39, 0.135     3500       5.7e-17     1.3e-17
 *    IEEE     0.125, 1        20000       7.2e-16     1.3e-16
 *    IEEE     3e-308, 0.135   50000       4.6e-16     9.8e-17
 *
 *
 * ERROR MESSAGES:
 *
 *   message         condition    value returned
 * ndtri domain       x == 0        -INF
 * ndtri domain       x == 1         INF
 * ndtri domain       x < 0, x > 1   NAN
 */
/*
  Cephes Math Library Release 2.2: June, 1992
  Copyright 1985, 1987, 1992 by Stephen L. Moshier
  Direct inquiries to 30 Frost Street, Cambridge, MA 02140
*/

// TODO: Add a cheaper approximation for float.

template <typename T>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T flipsign(const T& should_flipsign, const T& x) {
  typedef typename unpacket_traits<T>::type Scalar;
  const T sign_mask = pset1<T>(Scalar(-0.0));
  T sign_bit = pand<T>(should_flipsign, sign_mask);
  return pxor<T>(sign_bit, x);
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE double flipsign<double>(const double& should_flipsign, const double& x) {
  return should_flipsign == 0 ? x : -x;
}

template <>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE float flipsign<float>(const float& should_flipsign, const float& x) {
  return should_flipsign == 0 ? x : -x;
}

// We split this computation in to two so that in the scalar path
// only one branch is evaluated (due to our template specialization of pselect
// being an if statement.)

template <typename T, typename ScalarType>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_ndtri_gt_exp_neg_two(const T& b) {
  const ScalarType p0[] = {ScalarType(-5.99633501014107895267e1), ScalarType(9.80010754185999661536e1),
                           ScalarType(-5.66762857469070293439e1), ScalarType(1.39312609387279679503e1),
                           ScalarType(-1.23916583867381258016e0)};
  const ScalarType q0[] = {ScalarType(1.0),
                           ScalarType(1.95448858338141759834e0),
                           ScalarType(4.67627912898881538453e0),
                           ScalarType(8.63602421390890590575e1),
                           ScalarType(-2.25462687854119370527e2),
                           ScalarType(2.00260212380060660359e2),
                           ScalarType(-8.20372256168333339912e1),
                           ScalarType(1.59056225126211695515e1),
                           ScalarType(-1.18331621121330003142e0)};
  const T sqrt2pi = pset1<T>(ScalarType(2.50662827463100050242e0));
  const T half = pset1<T>(ScalarType(0.5));
  T c, c2, ndtri_gt_exp_neg_two;

  c = psub(b, half);
  c2 = pmul(c, c);
  ndtri_gt_exp_neg_two =
      pmadd(c, pmul(c2, pdiv(internal::ppolevl<T, 4>::run(c2, p0), internal::ppolevl<T, 8>::run(c2, q0))), c);
  return pmul(ndtri_gt_exp_neg_two, sqrt2pi);
}

template <typename T, typename ScalarType>
EIGEN_DEVICE_FUNC EIGEN_STRONG_INLINE T generic_ndtri_lt_exp_neg_two(const T& b, const T& should_flipsign) {
  /* Approximation for interval z = sqrt(-2 log a ) between 2 and 8
   * i.e., a between exp(-2) = .135 and exp(-32) = 1.27e-14.
   */
  const ScalarType p1[] = {ScalarType(4.05544892305962419923e0),   ScalarType(3.15251094599893866154e1),
                           ScalarType(5.71628192246421288162e1),   ScalarType(4.40805073893200834700e1),
                           ScalarType(1.46849561928858024014e1),   ScalarType(2.18663306850790267539e0),
                           ScalarType(-1.40256079171354495875e-1), ScalarType(-3.50424626827848203418e-2),
                           ScalarType(-8.57456785154685413611e-4)};
  const ScalarType q1[] = {ScalarType(1.0),
                           ScalarType(1.57799883256466749731e1),
                           ScalarType(4.53907635128879210584e1),
                           ScalarType(4.13172038254672030440e1),
                           ScalarType(1.50425385692907503408e1),
                           ScalarType(2.50464946208309415979e0),
                           ScalarType(-1.42182922854787788574e-1),
                           ScalarType(-3.80806407691578277194e-2),
                           ScalarType(-9.33259480895457427372e-4)};
  /* Approximation for interval z = sqrt(-2 log a ) between 8 and 64
   * i.e., a between exp(-32) = 1.27e-14 and exp(-2048) = 3.67e-890.
   */
  const ScalarType p2[] = {ScalarType(3.23774891776946035970e0),  ScalarType(6.91522889068984211695e0),
                           ScalarType(3.93881025292474443415e0),  ScalarType(1.33303460815807542389e0),
                           ScalarType(2.01485389549179081538e-1), ScalarType(1.23716634817820021358e-2),
                           ScalarType(3.01581553508235416007e-4), ScalarType(2.65806974686737550832e-6),
                           ScalarType(6.23974539184983293730e-9)};
  const ScalarType q2[] = {ScalarType(1.0),
                           ScalarType(6.02427039364742014255e0),
                           ScalarType(3.67983563856160859403e0),
                           ScalarType(1.37702099489081330271e0),
                           ScalarType(2.16236993594496635890e-1),
                           ScalarType(1.34204006088543189037e-2),
                           ScalarType(3.28014464682127739104e-4),
                           ScalarType(2.89247864745380683936e-6),
                           ScalarType(6.79019408009981274425e-9)};
  const T eight = pset1<T>(ScalarType(8.0));
  const T neg_two = pset1<T>(ScalarType(-2));
  T x, x0, x1, z;

  x = psqrt(pmul(neg_two, plog(b)));
  x0 = psub(x, pdiv(plog(x), x));
  z = preciprocal(x);
  x1 =
      pmul(z, pselect(pcmp_lt(x, eight), pdiv(internal::ppolevl<T, 8>::run(z, p1), internal::ppolevl<T, 8>::run(z, q1)),
                      pdiv(internal::ppolevl<T, 8>::run(z, p2), internal::ppolevl<T, 8>::run(z, q2))));
  return flipsign(should_flipsign, psub(x0, x1));
}

template <typename T, typename ScalarType>
EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE T generic_ndtri(const T& a) {
  const T maxnum = pset1<T>(NumTraits<ScalarType>::infinity());
  const T neg_maxnum = pset1<T>(-NumTraits<ScalarType>::infinity());

  const T zero = pset1<T>(ScalarType(0));
  const T one = pset1<T>(ScalarType(1));
  // exp(-2)
  const T exp_neg_two = pset1<T>(ScalarType(0.13533528323661269189));
  T b, ndtri, should_flipsign;

  should_flipsign = pcmp_le(a, psub(one, exp_neg_two));
  b = pselect(should_flipsign, a, psub(one, a));

  ndtri = pselect(pcmp_lt(exp_neg_two, b), generic_ndtri_gt_exp_neg_two<T, ScalarType>(b),
                  generic_ndtri_lt_exp_neg_two<T, ScalarType>(b, should_flipsign));

  return pselect(pcmp_eq(a, zero), neg_maxnum, pselect(pcmp_eq(one, a), maxnum, ndtri));
}

template <typename Scalar>
struct ndtri_retval {
  typedef Scalar type;
};

#if !EIGEN_HAS_C99_MATH

template <typename Scalar>
struct ndtri_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(const Scalar) { return Scalar(0); }
};

#else

template <typename Scalar>
struct ndtri_impl {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(const Scalar x) { return generic_ndtri<Scalar, Scalar>(x); }
};

#endif  // EIGEN_HAS_C99_MATH

/**************************************************************************************************************
 * Implementation of igammac (complemented incomplete gamma integral), based on Cephes but requires C++11/C99 *
 **************************************************************************************************************/

template <typename Scalar>
struct igammac_retval {
  typedef Scalar type;
};

// NOTE: cephes_helper is also used to implement zeta
template <typename Scalar>
struct cephes_helper {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar machep() {
    eigen_assert(false && "machep not supported for this type");
    return 0.0;
  }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar big() {
    eigen_assert(false && "big not supported for this type");
    return 0.0;
  }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar biginv() {
    eigen_assert(false && "biginv not supported for this type");
    return 0.0;
  }
};

template <>
struct cephes_helper<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float machep() {
    return NumTraits<float>::epsilon() / 2;  // 1.0 - machep == 1.0
  }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float big() {
    // use epsneg (1.0 - epsneg == 1.0)
    return 1.0f / (NumTraits<float>::epsilon() / 2);
  }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float biginv() {
    // epsneg
    return machep();
  }
};

template <>
struct cephes_helper<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double machep() {
    return NumTraits<double>::epsilon() / 2;  // 1.0 - machep == 1.0
  }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double big() { return 1.0 / NumTraits<double>::epsilon(); }
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double biginv() {
    // inverse of eps
    return NumTraits<double>::epsilon();
  }
};

enum IgammaComputationMode { VALUE, DERIVATIVE, SAMPLE_DERIVATIVE };

template <typename Scalar>
EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar main_igamma_term(Scalar a, Scalar x) {
  /* Compute  x**a * exp(-x) / gamma(a)  */
  Scalar logax = a * numext::log(x) - x - lgamma_impl<Scalar>::run(a);
  if (logax < -numext::log(NumTraits<Scalar>::highest()) ||
      // Assuming x and a aren't Nan.
      (numext::isnan)(logax)) {
    return Scalar(0);
  }
  return numext::exp(logax);
}

template <typename Scalar, IgammaComputationMode mode>
EIGEN_DEVICE_FUNC int igamma_num_iterations() {
  /* Returns the maximum number of internal iterations for igamma computation.
   */
  if (mode == VALUE) {
    return 2000;
  }

  if (internal::is_same<Scalar, float>::value) {
    return 200;
  } else if (internal::is_same<Scalar, double>::value) {
    return 500;
  } else {
    return 2000;
  }
}

template <typename Scalar, IgammaComputationMode mode>
struct igammac_cf_impl {
  /* Computes igamc(a, x) or derivative (depending on the mode)
   * using the continued fraction expansion of the complementary
   * incomplete Gamma function.
   *
   * Preconditions:
   *   a > 0
   *   x >= 1
   *   x >= a
   */
  EIGEN_DEVICE_FUNC static Scalar run(Scalar a, Scalar x) {
    const Scalar zero = 0;
    const Scalar one = 1;
    const Scalar two = 2;
    const Scalar machep = cephes_helper<Scalar>::machep();
    const Scalar big = cephes_helper<Scalar>::big();
    const Scalar biginv = cephes_helper<Scalar>::biginv();

    if ((numext::isinf)(x)) {
      return zero;
    }

    Scalar ax = main_igamma_term<Scalar>(a, x);
    // This is independent of mode. If this value is zero,
    // then the function value is zero. If the function value is zero,
    // then we are in a neighborhood where the function value evaluates to zero,
    // so the derivative is zero.
    if (ax == zero) {
      return zero;
    }

    // continued fraction
    Scalar y = one - a;
    Scalar z = x + y + one;
    Scalar c = zero;
    Scalar pkm2 = one;
    Scalar qkm2 = x;
    Scalar pkm1 = x + one;
    Scalar qkm1 = z * x;
    Scalar ans = pkm1 / qkm1;

    Scalar dpkm2_da = zero;
    Scalar dqkm2_da = zero;
    Scalar dpkm1_da = zero;
    Scalar dqkm1_da = -x;
    Scalar dans_da = (dpkm1_da - ans * dqkm1_da) / qkm1;

    for (int i = 0; i < igamma_num_iterations<Scalar, mode>(); i++) {
      c += one;
      y += one;
      z += two;

      Scalar yc = y * c;
      Scalar pk = pkm1 * z - pkm2 * yc;
      Scalar qk = qkm1 * z - qkm2 * yc;

      Scalar dpk_da = dpkm1_da * z - pkm1 - dpkm2_da * yc + pkm2 * c;
      Scalar dqk_da = dqkm1_da * z - qkm1 - dqkm2_da * yc + qkm2 * c;

      if (qk != zero) {
        Scalar ans_prev = ans;
        ans = pk / qk;

        Scalar dans_da_prev = dans_da;
        dans_da = (dpk_da - ans * dqk_da) / qk;

        if (mode == VALUE) {
          if (numext::abs(ans_prev - ans) <= machep * numext::abs(ans)) {
            break;
          }
        } else {
          if (numext::abs(dans_da - dans_da_prev) <= machep) {
            break;
          }
        }
      }

      pkm2 = pkm1;
      pkm1 = pk;
      qkm2 = qkm1;
      qkm1 = qk;

      dpkm2_da = dpkm1_da;
      dpkm1_da = dpk_da;
      dqkm2_da = dqkm1_da;
      dqkm1_da = dqk_da;

      if (numext::abs(pk) > big) {
        pkm2 *= biginv;
        pkm1 *= biginv;
        qkm2 *= biginv;
        qkm1 *= biginv;

        dpkm2_da *= biginv;
        dpkm1_da *= biginv;
        dqkm2_da *= biginv;
        dqkm1_da *= biginv;
      }
    }

    /* Compute  x**a * exp(-x) / gamma(a)  */
    Scalar dlogax_da = numext::log(x) - digamma_impl<Scalar>::run(a);
    Scalar dax_da = ax * dlogax_da;

    switch (mode) {
      case VALUE:
        return ans * ax;
      case DERIVATIVE:
        return ans * dax_da + dans_da * ax;
      case SAMPLE_DERIVATIVE:
      default:  // this is needed to suppress clang warning
        return -(dans_da + ans * dlogax_da) * x;
    }
  }
};

template <typename Scalar, IgammaComputationMode mode>
struct igamma_series_impl {
  /* Computes igam(a, x) or its derivative (depending on the mode)
   * using the series expansion of the incomplete Gamma function.
   *
   * Preconditions:
   *   x > 0
   *   a > 0
   *   !(x > 1 && x > a)
   */
  EIGEN_DEVICE_FUNC static Scalar run(Scalar a, Scalar x) {
    const Scalar zero = 0;
    const Scalar one = 1;
    const Scalar machep = cephes_helper<Scalar>::machep();

    Scalar ax = main_igamma_term<Scalar>(a, x);

    // This is independent of mode. If this value is zero,
    // then the function value is zero. If the function value is zero,
    // then we are in a neighborhood where the function value evaluates to zero,
    // so the derivative is zero.
    if (ax == zero) {
      return zero;
    }

    ax /= a;

    /* power series */
    Scalar r = a;
    Scalar c = one;
    Scalar ans = one;

    Scalar dc_da = zero;
    Scalar dans_da = zero;

    for (int i = 0; i < igamma_num_iterations<Scalar, mode>(); i++) {
      r += one;
      Scalar term = x / r;
      Scalar dterm_da = -x / (r * r);
      dc_da = term * dc_da + dterm_da * c;
      dans_da += dc_da;
      c *= term;
      ans += c;

      if (mode == VALUE) {
        if (c <= machep * ans) {
          break;
        }
      } else {
        if (numext::abs(dc_da) <= machep * numext::abs(dans_da)) {
          break;
        }
      }
    }

    Scalar dlogax_da = numext::log(x) - digamma_impl<Scalar>::run(a + one);
    Scalar dax_da = ax * dlogax_da;

    switch (mode) {
      case VALUE:
        return ans * ax;
      case DERIVATIVE:
        return ans * dax_da + dans_da * ax;
      case SAMPLE_DERIVATIVE:
      default:  // this is needed to suppress clang warning
        return -(dans_da + ans * dlogax_da) * x / a;
    }
  }
};

#if !EIGEN_HAS_C99_MATH

template <typename Scalar>
struct igammac_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static Scalar run(Scalar a, Scalar x) { return Scalar(0); }
};

#else

template <typename Scalar>
struct igammac_impl {
  EIGEN_DEVICE_FUNC static Scalar run(Scalar a, Scalar x) {
    /*  igamc()
     *
     *	Incomplete gamma integral (modified for Eigen)
     *
     *
     *
     * SYNOPSIS:
     *
     * double a, x, y, igamc();
     *
     * y = igamc( a, x );
     *
     * DESCRIPTION:
     *
     * The function is defined by
     *
     *
     *  igamc(a,x)   =   1 - igam(a,x)
     *
     *                            inf.
     *                              -
     *                     1       | |  -t  a-1
     *               =   -----     |   e   t   dt.
     *                    -      | |
     *                   | (a)    -
     *                             x
     *
     *
     * In this implementation both arguments must be positive.
     * The integral is evaluated by either a power series or
     * continued fraction expansion, depending on the relative
     * values of a and x.
     *
     * ACCURACY (float):
     *
     *                      Relative error:
     * arithmetic   domain     # trials      peak         rms
     *    IEEE      0,30        30000       7.8e-6      5.9e-7
     *
     *
     * ACCURACY (double):
     *
     * Tested at random a, x.
     *                a         x                      Relative error:
     * arithmetic   domain   domain     # trials      peak         rms
     *    IEEE     0.5,100   0,100      200000       1.9e-14     1.7e-15
     *    IEEE     0.01,0.5  0,100      200000       1.4e-13     1.6e-15
     *
     */
    /*
      Cephes Math Library Release 2.2: June, 1992
      Copyright 1985, 1987, 1992 by Stephen L. Moshier
      Direct inquiries to 30 Frost Street, Cambridge, MA 02140
    */
    const Scalar zero = 0;
    const Scalar one = 1;
    const Scalar nan = NumTraits<Scalar>::quiet_NaN();

    if ((x < zero) || (a <= zero)) {
      // domain error
      return nan;
    }

    if ((numext::isnan)(a) || (numext::isnan)(x)) {  // propagate nans
      return nan;
    }

    if ((x < one) || (x < a)) {
      return (one - igamma_series_impl<Scalar, VALUE>::run(a, x));
    }

    return igammac_cf_impl<Scalar, VALUE>::run(a, x);
  }
};

#endif  // EIGEN_HAS_C99_MATH

/************************************************************************************************
 * Implementation of igamma (incomplete gamma integral), based on Cephes but requires C++11/C99 *
 ************************************************************************************************/

#if !EIGEN_HAS_C99_MATH

template <typename Scalar, IgammaComputationMode mode>
struct igamma_generic_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(Scalar a, Scalar x) { return Scalar(0); }
};

#else

template <typename Scalar, IgammaComputationMode mode>
struct igamma_generic_impl {
  EIGEN_DEVICE_FUNC static Scalar run(Scalar a, Scalar x) {
    /* Depending on the mode, returns
     * - VALUE: incomplete Gamma function igamma(a, x)
     * - DERIVATIVE: derivative of incomplete Gamma function d/da igamma(a, x)
     * - SAMPLE_DERIVATIVE: implicit derivative of a Gamma random variable
     * x ~ Gamma(x | a, 1), dx/da = -1 / Gamma(x | a, 1) * d igamma(a, x) / dx
     *
     * Derivatives are implemented by forward-mode differentiation.
     */
    const Scalar zero = 0;
    const Scalar one = 1;
    const Scalar nan = NumTraits<Scalar>::quiet_NaN();

    if (x == zero) return zero;

    if ((x < zero) || (a <= zero)) {  // domain error
      return nan;
    }

    if ((numext::isnan)(a) || (numext::isnan)(x)) {  // propagate nans
      return nan;
    }

    if ((x > one) && (x > a)) {
      Scalar ret = igammac_cf_impl<Scalar, mode>::run(a, x);
      if (mode == VALUE) {
        return one - ret;
      } else {
        return -ret;
      }
    }

    return igamma_series_impl<Scalar, mode>::run(a, x);
  }
};

#endif  // EIGEN_HAS_C99_MATH

template <typename Scalar>
struct igamma_retval {
  typedef Scalar type;
};

template <typename Scalar>
struct igamma_impl : igamma_generic_impl<Scalar, VALUE> {
  /* igam()
   * Incomplete gamma integral.
   *
   * The CDF of Gamma(a, 1) random variable at the point x.
   *
   * Accuracy estimation. For each a in [10^-2, 10^-1...10^3] we sample
   * 50 Gamma random variables x ~ Gamma(x | a, 1), a total of 300 points.
   * The ground truth is computed by mpmath. Mean absolute error:
   * float: 1.26713e-05
   * double: 2.33606e-12
   *
   * Cephes documentation below.
   *
   * SYNOPSIS:
   *
   * double a, x, y, igam();
   *
   * y = igam( a, x );
   *
   * DESCRIPTION:
   *
   * The function is defined by
   *
   *                           x
   *                            -
   *                   1       | |  -t  a-1
   *  igam(a,x)  =   -----     |   e   t   dt.
   *                  -      | |
   *                 | (a)    -
   *                           0
   *
   *
   * In this implementation both arguments must be positive.
   * The integral is evaluated by either a power series or
   * continued fraction expansion, depending on the relative
   * values of a and x.
   *
   * ACCURACY (double):
   *
   *                      Relative error:
   * arithmetic   domain     # trials      peak         rms
   *    IEEE      0,30       200000       3.6e-14     2.9e-15
   *    IEEE      0,100      300000       9.9e-14     1.5e-14
   *
   *
   * ACCURACY (float):
   *
   *                      Relative error:
   * arithmetic   domain     # trials      peak         rms
   *    IEEE      0,30        20000       7.8e-6      5.9e-7
   *
   */
  /*
    Cephes Math Library Release 2.2: June, 1992
    Copyright 1985, 1987, 1992 by Stephen L. Moshier
    Direct inquiries to 30 Frost Street, Cambridge, MA 02140
  */

  /* left tail of incomplete gamma function:
   *
   *          inf.      k
   *   a  -x   -       x
   *  x  e     >   ----------
   *           -     -
   *          k=0   | (a+k+1)
   *
   */
};

template <typename Scalar>
struct igamma_der_a_retval : igamma_retval<Scalar> {};

template <typename Scalar>
struct igamma_der_a_impl : igamma_generic_impl<Scalar, DERIVATIVE> {
  /* Derivative of the incomplete Gamma function with respect to a.
   *
   * Computes d/da igamma(a, x) by forward differentiation of the igamma code.
   *
   * Accuracy estimation. For each a in [10^-2, 10^-1...10^3] we sample
   * 50 Gamma random variables x ~ Gamma(x | a, 1), a total of 300 points.
   * The ground truth is computed by mpmath. Mean absolute error:
   * float: 6.17992e-07
   * double: 4.60453e-12
   *
   * Reference:
   * R. Moore. "Algorithm AS 187: Derivatives of the incomplete gamma
   * integral". Journal of the Royal Statistical Society. 1982
   */
};

template <typename Scalar>
struct gamma_sample_der_alpha_retval : igamma_retval<Scalar> {};

template <typename Scalar>
struct gamma_sample_der_alpha_impl : igamma_generic_impl<Scalar, SAMPLE_DERIVATIVE> {
  /* Derivative of a Gamma random variable sample with respect to alpha.
   *
   * Consider a sample of a Gamma random variable with the concentration
   * parameter alpha: sample ~ Gamma(alpha, 1). The reparameterization
   * derivative that we want to compute is dsample / dalpha =
   * d igammainv(alpha, u) / dalpha, where u = igamma(alpha, sample).
   * However, this formula is numerically unstable and expensive, so instead
   * we use implicit differentiation:
   *
   * igamma(alpha, sample) = u, where u ~ Uniform(0, 1).
   * Apply d / dalpha to both sides:
   * d igamma(alpha, sample) / dalpha
   *     + d igamma(alpha, sample) / dsample * dsample/dalpha  = 0
   * d igamma(alpha, sample) / dalpha
   *     + Gamma(sample | alpha, 1) dsample / dalpha = 0
   * dsample/dalpha = - (d igamma(alpha, sample) / dalpha)
   *                   / Gamma(sample | alpha, 1)
   *
   * Here Gamma(sample | alpha, 1) is the PDF of the Gamma distribution
   * (note that the derivative of the CDF w.r.t. sample is the PDF).
   * See the reference below for more details.
   *
   * The derivative of igamma(alpha, sample) is computed by forward
   * differentiation of the igamma code. Division by the Gamma PDF is performed
   * in the same code, increasing the accuracy and speed due to cancellation
   * of some terms.
   *
   * Accuracy estimation. For each alpha in [10^-2, 10^-1...10^3] we sample
   * 50 Gamma random variables sample ~ Gamma(sample | alpha, 1), a total of 300
   * points. The ground truth is computed by mpmath. Mean absolute error:
   * float: 2.1686e-06
   * double: 1.4774e-12
   *
   * Reference:
   * M. Figurnov, S. Mohamed, A. Mnih "Implicit Reparameterization Gradients".
   * 2018
   */
};

/*****************************************************************************
 * Implementation of Riemann zeta function of two arguments, based on Cephes *
 *****************************************************************************/

template <typename Scalar>
struct zeta_retval {
  typedef Scalar type;
};

template <typename Scalar>
struct zeta_impl_series {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(const Scalar) { return Scalar(0); }
};

template <>
struct zeta_impl_series<float> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE bool run(float& a, float& b, float& s, const float x,
                                                        const float machep) {
    int i = 0;
    while (i < 9) {
      i += 1;
      a += 1.0f;
      b = numext::pow(a, -x);
      s += b;
      if (numext::abs(b / s) < machep) return true;
    }

    // Return whether we are done
    return false;
  }
};

template <>
struct zeta_impl_series<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE bool run(double& a, double& b, double& s, const double x,
                                                        const double machep) {
    int i = 0;
    while ((i < 9) || (a <= 9.0)) {
      i += 1;
      a += 1.0;
      b = numext::pow(a, -x);
      s += b;
      if (numext::abs(b / s) < machep) return true;
    }

    // Return whether we are done
    return false;
  }
};

template <typename Scalar>
struct zeta_impl {
  EIGEN_DEVICE_FUNC static Scalar run(Scalar x, Scalar q) {
    /*							zeta.c
     *
     *	Riemann zeta function of two arguments
     *
     *
     *
     * SYNOPSIS:
     *
     * double x, q, y, zeta();
     *
     * y = zeta( x, q );
     *
     *
     *
     * DESCRIPTION:
     *
     *
     *
     *                 inf.
     *                  -        -x
     *   zeta(x,q)  =   >   (k+q)
     *                  -
     *                 k=0
     *
     * where x > 1 and q is not a negative integer or zero.
     * The Euler-Maclaurin summation formula is used to obtain
     * the expansion
     *
     *                n
     *                -       -x
     * zeta(x,q)  =   >  (k+q)
     *                -
     *               k=1
     *
     *           1-x                 inf.  B   x(x+1)...(x+2j)
     *      (n+q)           1         -     2j
     *  +  ---------  -  -------  +   >    --------------------
     *        x-1              x      -                   x+2j+1
     *                   2(n+q)      j=1       (2j)! (n+q)
     *
     * where the B2j are Bernoulli numbers.  Note that (see zetac.c)
     * zeta(x,1) = zetac(x) + 1.
     *
     *
     *
     * ACCURACY:
     *
     * Relative error for single precision:
     * arithmetic   domain     # trials      peak         rms
     *    IEEE      0,25        10000       6.9e-7      1.0e-7
     *
     * Large arguments may produce underflow in powf(), in which
     * case the results are inaccurate.
     *
     * REFERENCE:
     *
     * Gradshteyn, I. S., and I. M. Ryzhik, Tables of Integrals,
     * Series, and Products, p. 1073; Academic Press, 1980.
     *
     */

    int i;
    Scalar p, r, a, b, k, s, t, w;

    const Scalar A[] = {
        Scalar(12.0),
        Scalar(-720.0),
        Scalar(30240.0),
        Scalar(-1209600.0),
        Scalar(47900160.0),
        Scalar(-1.8924375803183791606e9), /*1.307674368e12/691*/
        Scalar(7.47242496e10),
        Scalar(-2.950130727918164224e12),  /*1.067062284288e16/3617*/
        Scalar(1.1646782814350067249e14),  /*5.109094217170944e18/43867*/
        Scalar(-4.5979787224074726105e15), /*8.028576626982912e20/174611*/
        Scalar(1.8152105401943546773e17),  /*1.5511210043330985984e23/854513*/
        Scalar(-7.1661652561756670113e18)  /*1.6938241367317436694528e27/236364091*/
    };

    const Scalar maxnum = NumTraits<Scalar>::infinity();
    const Scalar zero = Scalar(0.0), half = Scalar(0.5), one = Scalar(1.0);
    const Scalar machep = cephes_helper<Scalar>::machep();
    const Scalar nan = NumTraits<Scalar>::quiet_NaN();

    if (x == one) return maxnum;

    if (x < one) {
      return nan;
    }

    if (q <= zero) {
      if (q == numext::floor(q)) {
        if (numext::rint(Scalar(0.5) * x) == Scalar(0.5) * x) {
          return maxnum;
        } else {
          return nan;
        }
      }
      p = x;
      r = numext::floor(p);
      if (p != r) return nan;
    }

    /* Permit negative q but continue sum until n+q > +9 .
     * This case should be handled by a reflection formula.
     * If q<0 and x is an integer, there is a relation to
     * the polygamma function.
     */
    s = numext::pow(q, -x);
    a = q;
    b = zero;
    // Run the summation in a helper function that is specific to the floating precision
    if (zeta_impl_series<Scalar>::run(a, b, s, x, machep)) {
      return s;
    }

    // If b is zero, then the tail sum will also end up being zero.
    // Exiting early here can prevent NaNs for some large inputs, where
    // the tail sum computed below has term `a` which can overflow to `inf`.
    if (numext::equal_strict(b, zero)) {
      return s;
    }

    w = a;
    s += b * w / (x - one);
    s -= half * b;
    a = one;
    k = zero;

    for (i = 0; i < 12; i++) {
      a *= x + k;
      b /= w;
      t = a * b / A[i];
      s = s + t;
      t = numext::abs(t / s);
      if (t < machep) {
        break;
      }
      k += one;
      a *= x + k;
      b /= w;
      k += one;
    }
    return s;
  }
};

/****************************************************************************
 * Implementation of polygamma function, requires C++11/C99                 *
 ****************************************************************************/

template <typename Scalar>
struct polygamma_retval {
  typedef Scalar type;
};

#if !EIGEN_HAS_C99_MATH

template <typename Scalar>
struct polygamma_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(Scalar n, Scalar x) { return Scalar(0); }
};

#else

template <typename Scalar>
struct polygamma_impl {
  EIGEN_DEVICE_FUNC static Scalar run(Scalar n, Scalar x) {
    Scalar zero = 0.0, one = 1.0;
    Scalar nplus = n + one;
    const Scalar nan = NumTraits<Scalar>::quiet_NaN();

    // Check that n is a non-negative integer
    if (numext::floor(n) != n || n < zero) {
      return nan;
    }
    // Just return the digamma function for n = 0
    else if (n == zero) {
      return digamma_impl<Scalar>::run(x);
    }
    // Use the same implementation as scipy
    else {
      Scalar factorial = numext::exp(lgamma_impl<Scalar>::run(nplus));
      return numext::pow(-one, nplus) * factorial * zeta_impl<Scalar>::run(nplus, x);
    }
  }
};

#endif  // EIGEN_HAS_C99_MATH

/************************************************************************************************
 * Implementation of betainc (incomplete beta integral), based on Cephes but requires C++11/C99 *
 ************************************************************************************************/

template <typename Scalar>
struct betainc_retval {
  typedef Scalar type;
};

#if !EIGEN_HAS_C99_MATH

template <typename Scalar>
struct betainc_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(Scalar a, Scalar b, Scalar x) { return Scalar(0); }
};

#else

template <typename Scalar>
struct betainc_impl {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, Scalar>::value == false), THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(Scalar, Scalar, Scalar) {
    /*	betaincf.c
     *
     *	Incomplete beta integral
     *
     *
     * SYNOPSIS:
     *
     * float a, b, x, y, betaincf();
     *
     * y = betaincf( a, b, x );
     *
     *
     * DESCRIPTION:
     *
     * Returns incomplete beta integral of the arguments, evaluated
     * from zero to x.  The function is defined as
     *
     *                  x
     *     -            -
     *    | (a+b)      | |  a-1     b-1
     *  -----------    |   t   (1-t)   dt.
     *   -     -     | |
     *  | (a) | (b)   -
     *                 0
     *
     * The domain of definition is 0 <= x <= 1.  In this
     * implementation a and b are restricted to positive values.
     * The integral from x to 1 may be obtained by the symmetry
     * relation
     *
     *    1 - betainc( a, b, x )  =  betainc( b, a, 1-x ).
     *
     * The integral is evaluated by a continued fraction expansion.
     * If a < 1, the function calls itself recursively after a
     * transformation to increase a to a+1.
     *
     * ACCURACY (float):
     *
     * Tested at random points (a,b,x) with a and b in the indicated
     * interval and x between 0 and 1.
     *
     * arithmetic   domain     # trials      peak         rms
     * Relative error:
     *    IEEE       0,30       10000       3.7e-5      5.1e-6
     *    IEEE       0,100      10000       1.7e-4      2.5e-5
     * The useful domain for relative error is limited by underflow
     * of the single precision exponential function.
     * Absolute error:
     *    IEEE       0,30      100000       2.2e-5      9.6e-7
     *    IEEE       0,100      10000       6.5e-5      3.7e-6
     *
     * Larger errors may occur for extreme ratios of a and b.
     *
     * ACCURACY (double):
     * arithmetic   domain     # trials      peak         rms
     *    IEEE      0,5         10000       6.9e-15     4.5e-16
     *    IEEE      0,85       250000       2.2e-13     1.7e-14
     *    IEEE      0,1000      30000       5.3e-12     6.3e-13
     *    IEEE      0,10000    250000       9.3e-11     7.1e-12
     *    IEEE      0,100000    10000       8.7e-10     4.8e-11
     * Outputs smaller than the IEEE gradual underflow threshold
     * were excluded from these statistics.
     *
     * ERROR MESSAGES:
     *   message         condition      value returned
     * incbet domain      x<0, x>1          nan
     * incbet underflow                     nan
     */
    return Scalar(0);
  }
};

/* Continued fraction expansion #1 for incomplete beta integral (small_branch = True)
 * Continued fraction expansion #2 for incomplete beta integral (small_branch = False)
 */
template <typename Scalar>
struct incbeta_cfe {
  EIGEN_STATIC_ASSERT((internal::is_same<Scalar, float>::value || internal::is_same<Scalar, double>::value),
                      THIS_TYPE_IS_NOT_SUPPORTED)

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE Scalar run(Scalar a, Scalar b, Scalar x, bool small_branch) {
    const Scalar big = cephes_helper<Scalar>::big();
    const Scalar machep = cephes_helper<Scalar>::machep();
    const Scalar biginv = cephes_helper<Scalar>::biginv();

    const Scalar zero = 0;
    const Scalar one = 1;
    const Scalar two = 2;

    Scalar xk, pk, pkm1, pkm2, qk, qkm1, qkm2;
    Scalar k1, k2, k3, k4, k5, k6, k7, k8, k26update;
    Scalar ans;
    int n;

    const int num_iters = (internal::is_same<Scalar, float>::value) ? 100 : 300;
    const Scalar thresh = (internal::is_same<Scalar, float>::value) ? machep : Scalar(3) * machep;
    Scalar r = (internal::is_same<Scalar, float>::value) ? zero : one;

    if (small_branch) {
      k1 = a;
      k2 = a + b;
      k3 = a;
      k4 = a + one;
      k5 = one;
      k6 = b - one;
      k7 = k4;
      k8 = a + two;
      k26update = one;
    } else {
      k1 = a;
      k2 = b - one;
      k3 = a;
      k4 = a + one;
      k5 = one;
      k6 = a + b;
      k7 = a + one;
      k8 = a + two;
      k26update = -one;
      x = x / (one - x);
    }

    pkm2 = zero;
    qkm2 = one;
    pkm1 = one;
    qkm1 = one;
    ans = one;
    n = 0;

    do {
      xk = -(x * k1 * k2) / (k3 * k4);
      pk = pkm1 + pkm2 * xk;
      qk = qkm1 + qkm2 * xk;
      pkm2 = pkm1;
      pkm1 = pk;
      qkm2 = qkm1;
      qkm1 = qk;

      xk = (x * k5 * k6) / (k7 * k8);
      pk = pkm1 + pkm2 * xk;
      qk = qkm1 + qkm2 * xk;
      pkm2 = pkm1;
      pkm1 = pk;
      qkm2 = qkm1;
      qkm1 = qk;

      if (qk != zero) {
        r = pk / qk;
        if (numext::abs(ans - r) < numext::abs(r) * thresh) {
          return r;
        }
        ans = r;
      }

      k1 += one;
      k2 += k26update;
      k3 += two;
      k4 += two;
      k5 += one;
      k6 -= k26update;
      k7 += two;
      k8 += two;

      if ((numext::abs(qk) + numext::abs(pk)) > big) {
        pkm2 *= biginv;
        pkm1 *= biginv;
        qkm2 *= biginv;
        qkm1 *= biginv;
      }
      if ((numext::abs(qk) < biginv) || (numext::abs(pk) < biginv)) {
        pkm2 *= big;
        pkm1 *= big;
        qkm2 *= big;
        qkm1 *= big;
      }
    } while (++n < num_iters);

    return ans;
  }
};

/* Helper functions depending on the Scalar type */
template <typename Scalar>
struct betainc_helper {};

template <>
struct betainc_helper<float> {
  /* Core implementation, assumes a large (> 1.0) */
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float incbsa(float aa, float bb, float xx) {
    float ans, a, b, t, x, onemx;
    bool reversed_a_b = false;

    onemx = 1.0f - xx;

    /* see if x is greater than the mean */
    if (xx > (aa / (aa + bb))) {
      reversed_a_b = true;
      a = bb;
      b = aa;
      t = xx;
      x = onemx;
    } else {
      a = aa;
      b = bb;
      t = onemx;
      x = xx;
    }

    /* Choose expansion for optimal convergence */
    if (b > 10.0f) {
      if (numext::abs(b * x / a) < 0.3f) {
        t = betainc_helper<float>::incbps(a, b, x);
        if (reversed_a_b) t = 1.0f - t;
        return t;
      }
    }

    ans = x * (a + b - 2.0f) / (a - 1.0f);
    if (ans < 1.0f) {
      ans = incbeta_cfe<float>::run(a, b, x, true /* small_branch */);
      t = b * numext::log(t);
    } else {
      ans = incbeta_cfe<float>::run(a, b, x, false /* small_branch */);
      t = (b - 1.0f) * numext::log(t);
    }

    t += a * numext::log(x) + lgamma_impl<float>::run(a + b) - lgamma_impl<float>::run(a) - lgamma_impl<float>::run(b);
    t += numext::log(ans / a);
    t = numext::exp(t);

    if (reversed_a_b) t = 1.0f - t;
    return t;
  }

  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE float incbps(float a, float b, float x) {
    float t, u, y, s;
    const float machep = cephes_helper<float>::machep();

    y = a * numext::log(x) + (b - 1.0f) * numext::log1p(-x) - numext::log(a);
    y -= lgamma_impl<float>::run(a) + lgamma_impl<float>::run(b);
    y += lgamma_impl<float>::run(a + b);

    t = x / (1.0f - x);
    s = 0.0f;
    u = 1.0f;
    do {
      b -= 1.0f;
      if (b == 0.0f) {
        break;
      }
      a += 1.0f;
      u *= t * b / a;
      s += u;
    } while (numext::abs(u) > machep);

    return numext::exp(y) * (1.0f + s);
  }
};

template <>
struct betainc_impl<float> {
  EIGEN_DEVICE_FUNC static float run(float a, float b, float x) {
    const float nan = NumTraits<float>::quiet_NaN();
    float ans, t;

    if (a <= 0.0f) return nan;
    if (b <= 0.0f) return nan;
    if ((x <= 0.0f) || (x >= 1.0f)) {
      if (x == 0.0f) return 0.0f;
      if (x == 1.0f) return 1.0f;
      // mtherr("betaincf", DOMAIN);
      return nan;
    }

    /* transformation for small aa */
    if (a <= 1.0f) {
      ans = betainc_helper<float>::incbsa(a + 1.0f, b, x);
      t = a * numext::log(x) + b * numext::log1p(-x) + lgamma_impl<float>::run(a + b) -
          lgamma_impl<float>::run(a + 1.0f) - lgamma_impl<float>::run(b);
      return (ans + numext::exp(t));
    } else {
      return betainc_helper<float>::incbsa(a, b, x);
    }
  }
};

template <>
struct betainc_helper<double> {
  EIGEN_DEVICE_FUNC static EIGEN_STRONG_INLINE double incbps(double a, double b, double x) {
    const double machep = cephes_helper<double>::machep();

    double s, t, u, v, n, t1, z, ai;

    ai = 1.0 / a;
    u = (1.0 - b) * x;
    v = u / (a + 1.0);
    t1 = v;
    t = u;
    n = 2.0;
    s = 0.0;
    z = machep * ai;
    while (numext::abs(v) > z) {
      u = (n - b) * x / n;
      t *= u;
      v = t / (a + n);
      s += v;
      n += 1.0;
    }
    s += t1;
    s += ai;

    u = a * numext::log(x);
    // TODO: gamma() is not directly implemented in Eigen.
    /*
    if ((a + b) < maxgam && numext::abs(u) < maxlog) {
      t = gamma(a + b) / (gamma(a) * gamma(b));
      s = s * t * pow(x, a);
    }
    */
    t = lgamma_impl<double>::run(a + b) - lgamma_impl<double>::run(a) - lgamma_impl<double>::run(b) + u +
        numext::log(s);
    return s = numext::exp(t);
  }
};

template <>
struct betainc_impl<double> {
  EIGEN_DEVICE_FUNC static double run(double aa, double bb, double xx) {
    const double nan = NumTraits<double>::quiet_NaN();
    const double machep = cephes_helper<double>::machep();
    // const double maxgam = 171.624376956302725;

    double a, b, t, x, xc, w, y;
    bool reversed_a_b = false;

    if (aa <= 0.0 || bb <= 0.0) {
      return nan;  // goto domerr;
    }

    if ((xx <= 0.0) || (xx >= 1.0)) {
      if (xx == 0.0) return (0.0);
      if (xx == 1.0) return (1.0);
      // mtherr("incbet", DOMAIN);
      return nan;
    }

    if ((bb * xx) <= 1.0 && xx <= 0.95) {
      return betainc_helper<double>::incbps(aa, bb, xx);
    }

    w = 1.0 - xx;

    /* Reverse a and b if x is greater than the mean. */
    if (xx > (aa / (aa + bb))) {
      reversed_a_b = true;
      a = bb;
      b = aa;
      xc = xx;
      x = w;
    } else {
      a = aa;
      b = bb;
      xc = w;
      x = xx;
    }

    if (reversed_a_b && (b * x) <= 1.0 && x <= 0.95) {
      t = betainc_helper<double>::incbps(a, b, x);
      if (t <= machep) {
        t = 1.0 - machep;
      } else {
        t = 1.0 - t;
      }
      return t;
    }

    /* Choose expansion for better convergence. */
    y = x * (a + b - 2.0) - (a - 1.0);
    if (y < 0.0) {
      w = incbeta_cfe<double>::run(a, b, x, true /* small_branch */);
    } else {
      w = incbeta_cfe<double>::run(a, b, x, false /* small_branch */) / xc;
    }

    /* Multiply w by the factor
         a      b   _             _     _
        x  (1-x)   | (a+b) / ( a | (a) | (b) ) .   */

    y = a * numext::log(x);
    t = b * numext::log(xc);
    // TODO: gamma is not directly implemented in Eigen.
    /*
    if ((a + b) < maxgam && numext::abs(y) < maxlog && numext::abs(t) < maxlog)
    {
      t = pow(xc, b);
      t *= pow(x, a);
      t /= a;
      t *= w;
      t *= gamma(a + b) / (gamma(a) * gamma(b));
    } else {
    */
    /* Resort to logarithms.  */
    y += t + lgamma_impl<double>::run(a + b) - lgamma_impl<double>::run(a) - lgamma_impl<double>::run(b);
    y += numext::log(w / a);
    t = numext::exp(y);

    /* } */
    // done:

    if (reversed_a_b) {
      if (t <= machep) {
        t = 1.0 - machep;
      } else {
        t = 1.0 - t;
      }
    }
    return t;
  }
};

#endif  // EIGEN_HAS_C99_MATH

}  // end namespace internal

namespace numext {

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(lgamma, Scalar) lgamma(const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(lgamma, Scalar)::run(x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(digamma, Scalar) digamma(const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(digamma, Scalar)::run(x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(zeta, Scalar) zeta(const Scalar& x, const Scalar& q) {
  return EIGEN_MATHFUNC_IMPL(zeta, Scalar)::run(x, q);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(polygamma, Scalar) polygamma(const Scalar& n, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(polygamma, Scalar)::run(n, x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(erf, Scalar) erf(const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(erf, Scalar)::run(x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(erfc, Scalar) erfc(const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(erfc, Scalar)::run(x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(ndtri, Scalar) ndtri(const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(ndtri, Scalar)::run(x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(igamma, Scalar) igamma(const Scalar& a, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(igamma, Scalar)::run(a, x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(igamma_der_a, Scalar) igamma_der_a(const Scalar& a, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(igamma_der_a, Scalar)::run(a, x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(gamma_sample_der_alpha, Scalar)
    gamma_sample_der_alpha(const Scalar& a, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(gamma_sample_der_alpha, Scalar)::run(a, x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(igammac, Scalar) igammac(const Scalar& a, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(igammac, Scalar)::run(a, x);
}

template <typename Scalar>
EIGEN_DEVICE_FUNC inline EIGEN_MATHFUNC_RETVAL(betainc, Scalar)
    betainc(const Scalar& a, const Scalar& b, const Scalar& x) {
  return EIGEN_MATHFUNC_IMPL(betainc, Scalar)::run(a, b, x);
}

}  // end namespace numext
}  // end namespace Eigen

#endif  // EIGEN_SPECIAL_FUNCTIONS_H
