#if 1
/* WARNING: these are NOT the original Shewchuk's predicates,
 * but rather a modified version used in Cinolib (https://github.com/mlivesu/cinolib)
 *
 * Edits:
 *   line      130: included float.h to import machine epsilon directly from the standard C library
 *   lines 374-443: initialize machine epsilon and coefficients for roundoff errors at compile time.
 *                  With this edit it is no longer necessary to call exactinit() prior using the exact
 *                  predicates. They should go out of the box without any explicit initialization!
 *   lines 571-758: commented unused random number generation functions and exactinit()
*/


/*****************************************************************************/
/*                                                                           */
/*  Routines for Arbitrary Precision Floating-point Arithmetic               */
/*  and Fast Robust Geometric Predicates                                     */
/*  (predicates.c)                                                           */
/*                                                                           */
/*  May 18, 1996                                                             */
/*                                                                           */
/*  Placed in the public domain by                                           */
/*  Jonathan Richard Shewchuk                                                */
/*  School of Computer Science                                               */
/*  Carnegie Mellon University                                               */
/*  5000 Forbes Avenue                                                       */
/*  Pittsburgh, Pennsylvania  15213-3891                                     */
/*  jrs@cs.cmu.edu                                                           */
/*                                                                           */
/*  This file contains C implementation of algorithms for exact addition     */
/*    and multiplication of floating-point numbers, and predicates for       */
/*    robustly performing the orientation and incircle tests used in         */
/*    computational geometry.  The algorithms and underlying theory are      */
/*    described in Jonathan Richard Shewchuk.  "Adaptive Precision Floating- */
/*    Point Arithmetic and Fast Robust Geometric Predicates."  Technical     */
/*    Report CMU-CS-96-140, School of Computer Science, Carnegie Mellon      */
/*    University, Pittsburgh, Pennsylvania, May 1996.  (Submitted to         */
/*    Discrete & Computational Geometry.)                                    */
/*                                                                           */
/*  This file, the paper listed above, and other information are available   */
/*    from the Web page http://www.cs.cmu.edu/~quake/robust.html .           */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/*                                                                           */
/*  Using this code:                                                         */
/*                                                                           */
/*  First, read the short or long version of the paper (from the Web page    */
/*    above).                                                                */
/*                                                                           */
/*  Be sure to call exactinit() once, before calling any of the arithmetic   */
/*    functions or geometric predicates.  Also be sure to turn on the        */
/*    optimizer when compiling this file.                                    */
/*                                                                           */
/*                                                                           */
/*  Several geometric predicates are defined.  Their parameters are all      */
/*    points.  Each point is an array of two or three floating-point         */
/*    numbers.  The geometric predicates, described in the papers, are       */
/*                                                                           */
/*    orient2d(pa, pb, pc)                                                   */
/*    orient2dfast(pa, pb, pc)                                               */
/*    orient3d(pa, pb, pc, pd)                                               */
/*    orient3dfast(pa, pb, pc, pd)                                           */
/*    incircle(pa, pb, pc, pd)                                               */
/*    incirclefast(pa, pb, pc, pd)                                           */
/*    insphere(pa, pb, pc, pd, pe)                                           */
/*    inspherefast(pa, pb, pc, pd, pe)                                       */
/*                                                                           */
/*  Those with suffix "fast" are approximate, non-robust versions.  Those    */
/*    without the suffix are adaptive precision, robust versions.  There     */
/*    are also versions with the suffices "exact" and "slow", which are      */
/*    non-adaptive, exact arithmetic versions, which I use only for timings  */
/*    in my arithmetic papers.                                               */
/*                                                                           */
/*                                                                           */
/*  An expansion is represented by an array of floating-point numbers,       */
/*    sorted from smallest to largest magnitude (possibly with interspersed  */
/*    zeros).  The length of each expansion is stored as a separate integer, */
/*    and each arithmetic function returns an integer which is the length    */
/*    of the expansion it created.                                           */
/*                                                                           */
/*  Several arithmetic functions are defined.  Their parameters are          */
/*                                                                           */
/*    e, f           Input expansions                                        */
/*    elen, flen     Lengths of input expansions (must be >= 1)              */
/*    h              Output expansion                                        */
/*    b              Input scalar                                            */
/*                                                                           */
/*  The arithmetic functions are                                             */
/*                                                                           */
/*    grow_expansion(elen, e, b, h)                                          */
/*    grow_expansion_zeroelim(elen, e, b, h)                                 */
/*    expansion_sum(elen, e, flen, f, h)                                     */
/*    expansion_sum_zeroelim1(elen, e, flen, f, h)                           */
/*    expansion_sum_zeroelim2(elen, e, flen, f, h)                           */
/*    fast_expansion_sum(elen, e, flen, f, h)                                */
/*    fast_expansion_sum_zeroelim(elen, e, flen, f, h)                       */
/*    linear_expansion_sum(elen, e, flen, f, h)                              */
/*    linear_expansion_sum_zeroelim(elen, e, flen, f, h)                     */
/*    scale_expansion(elen, e, b, h)                                         */
/*    scale_expansion_zeroelim(elen, e, b, h)                                */
/*    compress(elen, e, h)                                                   */
/*                                                                           */
/*  All of these are described in the long version of the paper; some are    */
/*    described in the short version.  All return an integer that is the     */
/*    length of h.  Those with suffix _zeroelim perform zero elimination,    */
/*    and are recommended over their counterparts.  The procedure            */
/*    fast_expansion_sum_zeroelim() (or linear_expansion_sum_zeroelim() on   */
/*    processors that do not use the round-to-even tiebreaking rule) is      */
/*    recommended over expansion_sum_zeroelim().  Each procedure has a       */
/*    little note next to it (in the code below) that tells you whether or   */
/*    not the output expansion may be the same array as one of the input     */
/*    expansions.                                                            */
/*                                                                           */
/*                                                                           */
/*  If you look around below, you'll also find macros for a bunch of         */
/*    simple unrolled arithmetic operations, and procedures for printing     */
/*    expansions (commented out because they don't work with all C           */
/*    compilers) and for generating random floating-point numbers whose      */
/*    significand bits are all random.  Most of the macros have undocumented */
/*    requirements that certain of their parameters should not be the same   */
/*    variable; for safety, better to make sure all the parameters are       */
/*    distinct variables.  Feel free to send email to jrs@cs.cmu.edu if you  */
/*    have questions.                                                        */
/*                                                                           */
/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
//#include <sys/time.h>

/* On some machines, the exact arithmetic routines might be defeated by the  */
/*   use of internal extended precision floating-point registers.  Sometimes */
/*   this problem can be fixed by defining certain values to be volatile,    */
/*   thus forcing them to be stored to memory and rounded off.  This isn't   */
/*   a great solution, though, as it slows the arithmetic down.              */
/*                                                                           */
/* To try this out, write "#define INEXACT volatile" below.  Normally,       */
/*   however, INEXACT should be defined to be nothing.  ("#define INEXACT".) */

#define INEXACT                          /* Nothing */
/* #define INEXACT volatile */

#define REAL double             // Cino edit: if you change to float remember to update epsilon ad splitter!!!
#define REALPRINT doubleprint
#define REALRAND doublerand
#define NARROWRAND narrowdoublerand
#define UNIFORMRAND uniformdoublerand

/* Which of the following two methods of finding the absolute values is      */
/*   fastest is compiler-dependent.  A few compilers can inline and optimize */
/*   the fabs() call; but most will incur the overhead of a function call,   */
/*   which is disastrously slow.  A faster way on IEEE machines might be to  */
/*   mask the appropriate bit, but that's difficult to do in C.              */

#define Absolute(a)  ((a) >= 0.0 ? (a) : -(a))
/* #define Absolute(a)  fabs(a) */

/* Many of the operations are broken up into two pieces, a main part that    */
/*   performs an approximate operation, and a "tail" that computes the       */
/*   roundoff error of that operation.                                       */
/*                                                                           */
/* The operations Fast_Two_Sum(), Fast_Two_Diff(), Two_Sum(), Two_Diff(),    */
/*   Split(), and Two_Product() are all implemented as described in the      */
/*   reference.  Each of these macros requires certain variables to be       */
/*   defined in the calling routine.  The variables `bvirt', `c', `abig',    */
/*   `_i', `_j', `_k', `_l', `_m', and `_n' are declared `INEXACT' because   */
/*   they store the result of an operation that may incur roundoff error.    */
/*   The input parameter `x' (or the highest numbered `x_' parameter) must   */
/*   also be declared `INEXACT'.                                             */

#define Fast_Two_Sum_Tail(a, b, x, y) \
  bvirt = x - a; \
  y = b - bvirt

#define Fast_Two_Sum(a, b, x, y) \
  x = (REAL) (a + b); \
  Fast_Two_Sum_Tail(a, b, x, y)

#define Fast_Two_Diff_Tail(a, b, x, y) \
  bvirt = a - x; \
  y = bvirt - b

#define Fast_Two_Diff(a, b, x, y) \
  x = (REAL) (a - b); \
  Fast_Two_Diff_Tail(a, b, x, y)

#define Two_Sum_Tail(a, b, x, y) \
  bvirt = (REAL) (x - a); \
  avirt = x - bvirt; \
  bround = b - bvirt; \
  around = a - avirt; \
  y = around + bround

#define Two_Sum(a, b, x, y) \
  x = (REAL) (a + b); \
  Two_Sum_Tail(a, b, x, y)

#define Two_Diff_Tail(a, b, x, y) \
  bvirt = (REAL) (a - x); \
  avirt = x + bvirt; \
  bround = bvirt - b; \
  around = a - avirt; \
  y = around + bround

#define Two_Diff(a, b, x, y) \
  x = (REAL) (a - b); \
  Two_Diff_Tail(a, b, x, y)

#define Split(a, ahi, alo) \
  c = (REAL) (splitter * a); \
  abig = (REAL) (c - a); \
  ahi = c - abig; \
  alo = a - ahi

#define Two_Product_Tail(a, b, x, y) \
  Split(a, ahi, alo); \
  Split(b, bhi, blo); \
  err1 = x - (ahi * bhi); \
  err2 = err1 - (alo * bhi); \
  err3 = err2 - (ahi * blo); \
  y = (alo * blo) - err3

#define Two_Product(a, b, x, y) \
  x = (REAL) (a * b); \
  Two_Product_Tail(a, b, x, y)

/* Two_Product_Presplit() is Two_Product() where one of the inputs has       */
/*   already been split.  Avoids redundant splitting.                        */

#define Two_Product_Presplit(a, b, bhi, blo, x, y) \
  x = (REAL) (a * b); \
  Split(a, ahi, alo); \
  err1 = x - (ahi * bhi); \
  err2 = err1 - (alo * bhi); \
  err3 = err2 - (ahi * blo); \
  y = (alo * blo) - err3

/* Two_Product_2Presplit() is Two_Product() where both of the inputs have    */
/*   already been split.  Avoids redundant splitting.                        */

#define Two_Product_2Presplit(a, ahi, alo, b, bhi, blo, x, y) \
  x = (REAL) (a * b); \
  err1 = x - (ahi * bhi); \
  err2 = err1 - (alo * bhi); \
  err3 = err2 - (ahi * blo); \
  y = (alo * blo) - err3

/* Square() can be done more quickly than Two_Product().                     */

#define Square_Tail(a, x, y) \
  Split(a, ahi, alo); \
  err1 = x - (ahi * ahi); \
  err3 = err1 - ((ahi + ahi) * alo); \
  y = (alo * alo) - err3

#define Square(a, x, y) \
  x = (REAL) (a * a); \
  Square_Tail(a, x, y)

/* Macros for summing expansions of various fixed lengths.  These are all    */
/*   unrolled versions of Expansion_Sum().                                   */

#define Two_One_Sum(a1, a0, b, x2, x1, x0) \
  Two_Sum(a0, b , _i, x0); \
  Two_Sum(a1, _i, x2, x1)

#define Two_One_Diff(a1, a0, b, x2, x1, x0) \
  Two_Diff(a0, b , _i, x0); \
  Two_Sum( a1, _i, x2, x1)

#define Two_Two_Sum(a1, a0, b1, b0, x3, x2, x1, x0) \
  Two_One_Sum(a1, a0, b0, _j, _0, x0); \
  Two_One_Sum(_j, _0, b1, x3, x2, x1)

#define Two_Two_Diff(a1, a0, b1, b0, x3, x2, x1, x0) \
  Two_One_Diff(a1, a0, b0, _j, _0, x0); \
  Two_One_Diff(_j, _0, b1, x3, x2, x1)

#define Four_One_Sum(a3, a2, a1, a0, b, x4, x3, x2, x1, x0) \
  Two_One_Sum(a1, a0, b , _j, x1, x0); \
  Two_One_Sum(a3, a2, _j, x4, x3, x2)

#define Four_Two_Sum(a3, a2, a1, a0, b1, b0, x5, x4, x3, x2, x1, x0) \
  Four_One_Sum(a3, a2, a1, a0, b0, _k, _2, _1, _0, x0); \
  Four_One_Sum(_k, _2, _1, _0, b1, x5, x4, x3, x2, x1)

#define Four_Four_Sum(a3, a2, a1, a0, b4, b3, b1, b0, x7, x6, x5, x4, x3, x2, \
                      x1, x0) \
  Four_Two_Sum(a3, a2, a1, a0, b1, b0, _l, _2, _1, _0, x1, x0); \
  Four_Two_Sum(_l, _2, _1, _0, b4, b3, x7, x6, x5, x4, x3, x2)

#define Eight_One_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b, x8, x7, x6, x5, x4, \
                      x3, x2, x1, x0) \
  Four_One_Sum(a3, a2, a1, a0, b , _j, x3, x2, x1, x0); \
  Four_One_Sum(a7, a6, a5, a4, _j, x8, x7, x6, x5, x4)

#define Eight_Two_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b1, b0, x9, x8, x7, \
                      x6, x5, x4, x3, x2, x1, x0) \
  Eight_One_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b0, _k, _6, _5, _4, _3, _2, \
                _1, _0, x0); \
  Eight_One_Sum(_k, _6, _5, _4, _3, _2, _1, _0, b1, x9, x8, x7, x6, x5, x4, \
                x3, x2, x1)

#define Eight_Four_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b4, b3, b1, b0, x11, \
                       x10, x9, x8, x7, x6, x5, x4, x3, x2, x1, x0) \
  Eight_Two_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b1, b0, _l, _6, _5, _4, _3, \
                _2, _1, _0, x1, x0); \
  Eight_Two_Sum(_l, _6, _5, _4, _3, _2, _1, _0, b4, b3, x11, x10, x9, x8, \
                x7, x6, x5, x4, x3, x2)

/* Macros for multiplying expansions of various fixed lengths.               */

#define Two_One_Product(a1, a0, b, x3, x2, x1, x0) \
  Split(b, bhi, blo); \
  Two_Product_Presplit(a0, b, bhi, blo, _i, x0); \
  Two_Product_Presplit(a1, b, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _k, x1); \
  Fast_Two_Sum(_j, _k, x3, x2)

#define Four_One_Product(a3, a2, a1, a0, b, x7, x6, x5, x4, x3, x2, x1, x0) \
  Split(b, bhi, blo); \
  Two_Product_Presplit(a0, b, bhi, blo, _i, x0); \
  Two_Product_Presplit(a1, b, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _k, x1); \
  Fast_Two_Sum(_j, _k, _i, x2); \
  Two_Product_Presplit(a2, b, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _k, x3); \
  Fast_Two_Sum(_j, _k, _i, x4); \
  Two_Product_Presplit(a3, b, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _k, x5); \
  Fast_Two_Sum(_j, _k, x7, x6)

#define Two_Two_Product(a1, a0, b1, b0, x7, x6, x5, x4, x3, x2, x1, x0) \
  Split(a0, a0hi, a0lo); \
  Split(b0, bhi, blo); \
  Two_Product_2Presplit(a0, a0hi, a0lo, b0, bhi, blo, _i, x0); \
  Split(a1, a1hi, a1lo); \
  Two_Product_2Presplit(a1, a1hi, a1lo, b0, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _k, _1); \
  Fast_Two_Sum(_j, _k, _l, _2); \
  Split(b1, bhi, blo); \
  Two_Product_2Presplit(a0, a0hi, a0lo, b1, bhi, blo, _i, _0); \
  Two_Sum(_1, _0, _k, x1); \
  Two_Sum(_2, _k, _j, _1); \
  Two_Sum(_l, _j, _m, _2); \
  Two_Product_2Presplit(a1, a1hi, a1lo, b1, bhi, blo, _j, _0); \
  Two_Sum(_i, _0, _n, _0); \
  Two_Sum(_1, _0, _i, x2); \
  Two_Sum(_2, _i, _k, _1); \
  Two_Sum(_m, _k, _l, _2); \
  Two_Sum(_j, _n, _k, _0); \
  Two_Sum(_1, _0, _j, x3); \
  Two_Sum(_2, _j, _i, _1); \
  Two_Sum(_l, _i, _m, _2); \
  Two_Sum(_1, _k, _i, x4); \
  Two_Sum(_2, _i, _k, x5); \
  Two_Sum(_m, _k, x7, x6)

/* An expansion of length two can be squared more quickly than finding the   */
/*   product of two different expansions of length two, and the result is    */
/*   guaranteed to have no more than six (rather than eight) components.     */

#define Two_Square(a1, a0, x5, x4, x3, x2, x1, x0) \
  Square(a0, _j, x0); \
  _0 = a0 + a0; \
  Two_Product(a1, _0, _k, _1); \
  Two_One_Sum(_k, _1, _j, _l, _2, x1); \
  Square(a1, _j, _1); \
  Two_Two_Sum(_j, _1, _l, _2, x5, x4, x3, x2)

/* Cino edit:
 * this piece of code substitutes the exactinit() routine defined in the
 * original Shewchuk's predicates, and serves to define machine epsilon
 * and the error bounds for the orient and incircle predicates.
 * The header float.h defines DBL_EPSILON such that 1.0 + DBL_EPSILON > 1.0 in
 * double-precision arithmetic, but  1.0 + z = 1.0 for any `z < DBL_EPSILON`.
 * Shewchuk defines the machine epsilon as the largest floating-point number
 * such that 1.0 + epsilon = 1.0. The two definitions are related by
 * SHEWCHUK_EPSILON = DBL_EPSILON / 2
 *
 * In this modified version of the predicates SHEWCHUK_EPSILON is initialized
 * with #define, and not as a global variable like in the original code. This
 * is because GCC complains if I make it const REAL and I use it to inizialize
 * the error bounds below...
 */

/*
#define SHEWCHUK_EPSILON  DBL_EPSILON/2

const REAL resulterrbound = ( 3.0 +    8.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL ccwerrboundA   = ( 3.0 +   16.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL ccwerrboundB   = ( 2.0 +   12.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL ccwerrboundC   = ( 9.0 +   64.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON * SHEWCHUK_EPSILON;
const REAL o3derrboundA   = ( 7.0 +   56.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL o3derrboundB   = ( 3.0 +   28.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL o3derrboundC   = (26.0 +  288.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON * SHEWCHUK_EPSILON;
const REAL iccerrboundA   = (10.0 +   96.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL iccerrboundB   = ( 4.0 +   48.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL iccerrboundC   = (44.0 +  576.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON * SHEWCHUK_EPSILON;
const REAL isperrboundA   = (16.0 +  224.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL isperrboundB   = ( 5.0 +   72.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON;
const REAL isperrboundC   = (71.0 + 1408.0 * SHEWCHUK_EPSILON) * SHEWCHUK_EPSILON * SHEWCHUK_EPSILON;

// this matches the value produced by exactinit().
// However, 1 << (DBL_MANT_DIG/2)) + 1.0 is more correct IMHO
const REAL splitter = (1 << (DBL_MANT_DIG/2+1)) + 1.0;
*/

/* Marco edit:
 * the previous (now commented) code is semantically correct only in C++, and some compilers
 * issue an error if one attempts to consider it as C code. This happens e.g. in MSVC
 * (https://docs.microsoft.com/it-it/cpp/error-messages/compiler-errors-1/compiler-error-c2099).
 * As a workaround, I precalculated all the values as constants here below.
 * These values should be fine for any IEEE 754 compliant system.
*/
const double resulterrbound = 3.3306690738754706e-16;
const double ccwerrboundA   = 3.3306690738754716e-16;
const double ccwerrboundB   = 2.2204460492503146e-16;
const double ccwerrboundC   = 1.1093356479670487e-31;
const double o3derrboundA   = 7.7715611723761027e-16;
const double o3derrboundB   = 3.3306690738754731e-16;
const double o3derrboundC   = 3.2047474274603644e-31;
const double iccerrboundA   = 1.1102230246251577e-15;
const double iccerrboundB   = 4.4408920985006321e-16;
const double iccerrboundC   = 5.4234187233944640e-31;
const double isperrboundA   = 1.7763568394002532e-15;
const double isperrboundB   = 5.5511151231257916e-16;
const double isperrboundC   = 8.7514256672956190e-31;
const double splitter       = 1.34217729e+08;

// Original code from Shewchuk:
//
//REAL splitter;     /* = 2^ceiling(p / 2) + 1.  Used to split floats in half. */
//REAL epsilon;                /* = 2^(-p).  Used to estimate roundoff errors. */
///* A set of coefficients used to calculate maximum roundoff errors.          */
//REAL resulterrbound;
//REAL ccwerrboundA, ccwerrboundB, ccwerrboundC;
//REAL o3derrboundA, o3derrboundB, o3derrboundC;
//REAL iccerrboundA, iccerrboundB, iccerrboundC;
//REAL isperrboundA, isperrboundB, isperrboundC;

/*****************************************************************************/
/*                                                                           */
/*  doubleprint()   Print the bit representation of a double.                */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void doubleprint(number)
double number;
{
  unsigned long long no;
  unsigned long long sign, expo;
  int exponent;
  int i, bottomi;

  no = *(unsigned long long *) &number;
  sign = no & 0x8000000000000000ll;
  expo = (no >> 52) & 0x7ffll;
  exponent = (int) expo;
  exponent = exponent - 1023;
  if (sign) {
    printf("-");
  } else {
    printf(" ");
  }
  if (exponent == -1023) {
    printf(
      "0.0000000000000000000000000000000000000000000000000000_     (   )");
  } else {
    printf("1.");
    bottomi = -1;
    for (i = 0; i < 52; i++) {
      if (no & 0x0008000000000000ll) {
        printf("1");
        bottomi = i;
      } else {
        printf("0");
      }
      no <<= 1;
    }
    printf("_%d  (%d)", exponent, exponent - 1 - bottomi);
  }
}
*/

/*****************************************************************************/
/*                                                                           */
/*  floatprint()   Print the bit representation of a float.                  */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void floatprint(number)
float number;
{
  unsigned no;
  unsigned sign, expo;
  int exponent;
  int i, bottomi;

  no = *(unsigned *) &number;
  sign = no & 0x80000000;
  expo = (no >> 23) & 0xff;
  exponent = (int) expo;
  exponent = exponent - 127;
  if (sign) {
    printf("-");
  } else {
    printf(" ");
  }
  if (exponent == -127) {
    printf("0.00000000000000000000000_     (   )");
  } else {
    printf("1.");
    bottomi = -1;
    for (i = 0; i < 23; i++) {
      if (no & 0x00400000) {
        printf("1");
        bottomi = i;
      } else {
        printf("0");
      }
      no <<= 1;
    }
    printf("_%3d  (%3d)", exponent, exponent - 1 - bottomi);
  }
}
*/

/*****************************************************************************/
/*                                                                           */
/*  expansion_print()   Print the bit representation of an expansion.        */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void expansion_print(elen, e)
int elen;
REAL *e;
{
  int i;

  for (i = elen - 1; i >= 0; i--) {
    REALPRINT(e[i]);
    if (i > 0) {
      printf(" +\n");
    } else {
      printf("\n");
    }
  }
}
*/

/*****************************************************************************/
/*                                                                           */
/*  doublerand()   Generate a double with random 53-bit significand and a    */
/*                 random exponent in [0, 511].                              */
/*                                                                           */
/*****************************************************************************/

//double doublerand()
//{
//  double result;
//  double expo;
//  long a, b, c;
//  long i;
//
//  a = random();
//  b = random();
//  c = random();
//  result = (double) (a - 1073741824) * 8388608.0 + (double) (b >> 8);
//  for (i = 512, expo = 2; i <= 131072; i *= 2, expo = expo * expo) {
//    if (c & i) {
//      result *= expo;
//    }
//  }
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  narrowdoublerand()   Generate a double with random 53-bit significand    */
/*                       and a random exponent in [0, 7].                    */
/*                                                                           */
/*****************************************************************************/

//double narrowdoublerand()
//{
//  double result;
//  double expo;
//  long a, b, c;
//  long i;
//
//  a = random();
//  b = random();
//  c = random();
//  result = (double) (a - 1073741824) * 8388608.0 + (double) (b >> 8);
//  for (i = 512, expo = 2; i <= 2048; i *= 2, expo = expo * expo) {
//    if (c & i) {
//      result *= expo;
//    }
//  }
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  uniformdoublerand()   Generate a double with random 53-bit significand.  */
/*                                                                           */
/*****************************************************************************/

//double uniformdoublerand()
//{
//  double result;
//  long a, b;
//
//  a = random();
//  b = random();
//  result = (double) (a - 1073741824) * 8388608.0 + (double) (b >> 8);
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  floatrand()   Generate a float with random 24-bit significand and a      */
/*                random exponent in [0, 63].                                */
/*                                                                           */
/*****************************************************************************/

//float floatrand()
//{
//  float result;
//  float expo;
//  long a, c;
//  long i;
//
//  a = random();
//  c = random();
//  result = (float) ((a - 1073741824) >> 6);
//  for (i = 512, expo = 2; i <= 16384; i *= 2, expo = expo * expo) {
//    if (c & i) {
//      result *= expo;
//    }
//  }
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  narrowfloatrand()   Generate a float with random 24-bit significand and  */
/*                      a random exponent in [0, 7].                         */
/*                                                                           */
/*****************************************************************************/
//
//float narrowfloatrand()
//{
//  float result;
//  float expo;
//  long a, c;
//  long i;
//
//  a = random();
//  c = random();
//  result = (float) ((a - 1073741824) >> 6);
//  for (i = 512, expo = 2; i <= 2048; i *= 2, expo = expo * expo) {
//    if (c & i) {
//      result *= expo;
//    }
//  }
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  uniformfloatrand()   Generate a float with random 24-bit significand.    */
/*                                                                           */
/*****************************************************************************/

//float uniformfloatrand()
//{
//  float result;
//  long a;
//
//  a = random();
//  result = (float) ((a - 1073741824) >> 6);
//  return result;
//}

/*****************************************************************************/
/*                                                                           */
/*  exactinit()   Initialize the variables used for exact arithmetic.        */
/*                                                                           */
/*  `epsilon' is the largest power of two such that 1.0 + epsilon = 1.0 in   */
/*  floating-point arithmetic.  `epsilon' bounds the relative roundoff       */
/*  error.  It is used for floating-point error analysis.                    */
/*                                                                           */
/*  `splitter' is used to split floating-point numbers into two half-        */
/*  length significands for exact multiplication.                            */
/*                                                                           */
/*  I imagine that a highly optimizing compiler might be too smart for its   */
/*  own good, and somehow cause this routine to fail, if it pretends that    */
/*  floating-point arithmetic is too much like real arithmetic.              */
/*                                                                           */
/*  Don't change this routine unless you fully understand it.                */
/*                                                                           */
/*****************************************************************************/

//void exactinit()
//{
//  REAL half;
//  REAL check, lastcheck;
//  int every_other;

//  every_other = 1;
//  half = 0.5;
//  epsilon = 1.0;
//  splitter = 1.0;
//  check = 1.0;
//  /* Repeatedly divide `epsilon' by two until it is too small to add to    */
//  /*   one without causing roundoff.  (Also check if the sum is equal to   */
//  /*   the previous sum, for machines that round up instead of using exact */
//  /*   rounding.  Not that this library will work on such machines anyway. */
//  do {
//    lastcheck = check;
//    epsilon *= half;
//    if (every_other) {
//      splitter *= 2.0;
//    }
//    every_other = !every_other;
//    check = 1.0 + epsilon;
//  } while ((check != 1.0) && (check != lastcheck));
//  splitter += 1.0;

//  /* Error bounds for orientation and incircle tests. */
//  resulterrbound = (3.0 + 8.0 * epsilon) * epsilon;
//  ccwerrboundA = (3.0 + 16.0 * epsilon) * epsilon;
//  ccwerrboundB = (2.0 + 12.0 * epsilon) * epsilon;
//  ccwerrboundC = (9.0 + 64.0 * epsilon) * epsilon * epsilon;
//  o3derrboundA = (7.0 + 56.0 * epsilon) * epsilon;
//  o3derrboundB = (3.0 + 28.0 * epsilon) * epsilon;
//  o3derrboundC = (26.0 + 288.0 * epsilon) * epsilon * epsilon;
//  iccerrboundA = (10.0 + 96.0 * epsilon) * epsilon;
//  iccerrboundB = (4.0 + 48.0 * epsilon) * epsilon;
//  iccerrboundC = (44.0 + 576.0 * epsilon) * epsilon * epsilon;
//  isperrboundA = (16.0 + 224.0 * epsilon) * epsilon;
//  isperrboundB = (5.0 + 72.0 * epsilon) * epsilon;
//  isperrboundC = (71.0 + 1408.0 * epsilon) * epsilon * epsilon;
//}

/*****************************************************************************/
/*                                                                           */
/*  grow_expansion()   Add a scalar to an expansion.                         */
/*                                                                           */
/*  Sets h = e + b.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int grow_expansion(elen, e, b, h)                /* e and h can be the same. */
int elen;
REAL *e;
REAL b;
REAL *h;
{
  REAL Q;
  INEXACT REAL Qnew;
  int eindex;
  REAL enow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;

  Q = b;
  for (eindex = 0; eindex < elen; eindex++) {
    enow = e[eindex];
    Two_Sum(Q, enow, Qnew, h[eindex]);
    Q = Qnew;
  }
  h[eindex] = Q;
  return eindex + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  grow_expansion_zeroelim()   Add a scalar to an expansion, eliminating    */
/*                              zero components from the output expansion.   */
/*                                                                           */
/*  Sets h = e + b.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int grow_expansion_zeroelim(elen, e, b, h)       /* e and h can be the same. */
int elen;
REAL *e;
REAL b;
REAL *h;
{
  REAL Q, hh;
  INEXACT REAL Qnew;
  int eindex, hindex;
  REAL enow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;

  hindex = 0;
  Q = b;
  for (eindex = 0; eindex < elen; eindex++) {
    enow = e[eindex];
    Two_Sum(Q, enow, Qnew, hh);
    Q = Qnew;
    if (hh != 0.0) {
      h[hindex++] = hh;
    }
  }
  if ((Q != 0.0) || (hindex == 0)) {
    h[hindex++] = Q;
  }
  return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum()   Sum two expansions.                                    */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum(elen, e, flen, f, h)
/* e and h can be the same, but f and h cannot. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q;
  INEXACT REAL Qnew;
  int findex, hindex, hlast;
  REAL hnow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;

  Q = f[0];
  for (hindex = 0; hindex < elen; hindex++) {
    hnow = e[hindex];
    Two_Sum(Q, hnow, Qnew, h[hindex]);
    Q = Qnew;
  }
  h[hindex] = Q;
  hlast = hindex;
  for (findex = 1; findex < flen; findex++) {
    Q = f[findex];
    for (hindex = findex; hindex <= hlast; hindex++) {
      hnow = h[hindex];
      Two_Sum(Q, hnow, Qnew, h[hindex]);
      Q = Qnew;
    }
    h[++hlast] = Q;
  }
  return hlast + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum_zeroelim1()   Sum two expansions, eliminating zero         */
/*                              components from the output expansion.        */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum_zeroelim1(elen, e, flen, f, h)
/* e and h can be the same, but f and h cannot. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q;
  INEXACT REAL Qnew;
  int index, findex, hindex, hlast;
  REAL hnow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;

  Q = f[0];
  for (hindex = 0; hindex < elen; hindex++) {
    hnow = e[hindex];
    Two_Sum(Q, hnow, Qnew, h[hindex]);
    Q = Qnew;
  }
  h[hindex] = Q;
  hlast = hindex;
  for (findex = 1; findex < flen; findex++) {
    Q = f[findex];
    for (hindex = findex; hindex <= hlast; hindex++) {
      hnow = h[hindex];
      Two_Sum(Q, hnow, Qnew, h[hindex]);
      Q = Qnew;
    }
    h[++hlast] = Q;
  }
  hindex = -1;
  for (index = 0; index <= hlast; index++) {
    hnow = h[index];
    if (hnow != 0.0) {
      h[++hindex] = hnow;
    }
  }
  if (hindex == -1) {
    return 1;
  } else {
    return hindex + 1;
  }
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum_zeroelim2()   Sum two expansions, eliminating zero         */
/*                              components from the output expansion.        */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum_zeroelim2(elen, e, flen, f, h)
/* e and h can be the same, but f and h cannot. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q, hh;
  INEXACT REAL Qnew;
  int eindex, findex, hindex, hlast;
  REAL enow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;

  hindex = 0;
  Q = f[0];
  for (eindex = 0; eindex < elen; eindex++) {
    enow = e[eindex];
    Two_Sum(Q, enow, Qnew, hh);
    Q = Qnew;
    if (hh != 0.0) {
      h[hindex++] = hh;
    }
  }
  h[hindex] = Q;
  hlast = hindex;
  for (findex = 1; findex < flen; findex++) {
    hindex = 0;
    Q = f[findex];
    for (eindex = 0; eindex <= hlast; eindex++) {
      enow = h[eindex];
      Two_Sum(Q, enow, Qnew, hh);
      Q = Qnew;
      if (hh != 0) {
        h[hindex++] = hh;
      }
    }
    h[hindex] = Q;
    hlast = hindex;
  }
  return hlast + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  fast_expansion_sum()   Sum two expansions.                               */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  If round-to-even is used (as with IEEE 754), maintains the strongly      */
/*  nonoverlapping property.  (That is, if e is strongly nonoverlapping, h   */
/*  will be also.)  Does NOT maintain the nonoverlapping or nonadjacent      */
/*  properties.                                                              */
/*                                                                           */
/*****************************************************************************/

int fast_expansion_sum(elen, e, flen, f, h)           /* h cannot be e or f. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q;
  INEXACT REAL Qnew;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  int eindex, findex, hindex;
  REAL enow, fnow;

  enow = e[0];
  fnow = f[0];
  eindex = findex = 0;
  if ((fnow > enow) == (fnow > -enow)) {
    Q = enow;
    enow = e[++eindex];
  } else {
    Q = fnow;
    fnow = f[++findex];
  }
  hindex = 0;
  if ((eindex < elen) && (findex < flen)) {
    if ((fnow > enow) == (fnow > -enow)) {
      Fast_Two_Sum(enow, Q, Qnew, h[0]);
      enow = e[++eindex];
    } else {
      Fast_Two_Sum(fnow, Q, Qnew, h[0]);
      fnow = f[++findex];
    }
    Q = Qnew;
    hindex = 1;
    while ((eindex < elen) && (findex < flen)) {
      if ((fnow > enow) == (fnow > -enow)) {
        Two_Sum(Q, enow, Qnew, h[hindex]);
        enow = e[++eindex];
      } else {
        Two_Sum(Q, fnow, Qnew, h[hindex]);
        fnow = f[++findex];
      }
      Q = Qnew;
      hindex++;
    }
  }
  while (eindex < elen) {
    Two_Sum(Q, enow, Qnew, h[hindex]);
    enow = e[++eindex];
    Q = Qnew;
    hindex++;
  }
  while (findex < flen) {
    Two_Sum(Q, fnow, Qnew, h[hindex]);
    fnow = f[++findex];
    Q = Qnew;
    hindex++;
  }
  h[hindex] = Q;
  return hindex + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  fast_expansion_sum_zeroelim()   Sum two expansions, eliminating zero     */
/*                                  components from the output expansion.    */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  If round-to-even is used (as with IEEE 754), maintains the strongly      */
/*  nonoverlapping property.  (That is, if e is strongly nonoverlapping, h   */
/*  will be also.)  Does NOT maintain the nonoverlapping or nonadjacent      */
/*  properties.                                                              */
/*                                                                           */
/*****************************************************************************/

int fast_expansion_sum_zeroelim(elen, e, flen, f, h)  /* h cannot be e or f. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q;
  INEXACT REAL Qnew;
  INEXACT REAL hh;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  int eindex, findex, hindex;
  REAL enow, fnow;

  enow = e[0];
  fnow = f[0];
  eindex = findex = 0;
  if ((fnow > enow) == (fnow > -enow)) {
    Q = enow;
    enow = e[++eindex];
  } else {
    Q = fnow;
    fnow = f[++findex];
  }
  hindex = 0;
  if ((eindex < elen) && (findex < flen)) {
    if ((fnow > enow) == (fnow > -enow)) {
      Fast_Two_Sum(enow, Q, Qnew, hh);
      enow = e[++eindex];
    } else {
      Fast_Two_Sum(fnow, Q, Qnew, hh);
      fnow = f[++findex];
    }
    Q = Qnew;
    if (hh != 0.0) {
      h[hindex++] = hh;
    }
    while ((eindex < elen) && (findex < flen)) {
      if ((fnow > enow) == (fnow > -enow)) {
        Two_Sum(Q, enow, Qnew, hh);
        enow = e[++eindex];
      } else {
        Two_Sum(Q, fnow, Qnew, hh);
        fnow = f[++findex];
      }
      Q = Qnew;
      if (hh != 0.0) {
        h[hindex++] = hh;
      }
    }
  }
  while (eindex < elen) {
    Two_Sum(Q, enow, Qnew, hh);
    enow = e[++eindex];
    Q = Qnew;
    if (hh != 0.0) {
      h[hindex++] = hh;
    }
  }
  while (findex < flen) {
    Two_Sum(Q, fnow, Qnew, hh);
    fnow = f[++findex];
    Q = Qnew;
    if (hh != 0.0) {
      h[hindex++] = hh;
    }
  }
  if ((Q != 0.0) || (hindex == 0)) {
    h[hindex++] = Q;
  }
  return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  linear_expansion_sum()   Sum two expansions.                             */
/*                                                                           */
/*  Sets h = e + f.  See either version of my paper for details.             */
/*                                                                           */
/*  Maintains the nonoverlapping property.  (That is, if e is                */
/*  nonoverlapping, h will be also.)                                         */
/*                                                                           */
/*****************************************************************************/

int linear_expansion_sum(elen, e, flen, f, h)         /* h cannot be e or f. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q, q;
  INEXACT REAL Qnew;
  INEXACT REAL R;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  int eindex, findex, hindex;
  REAL enow, fnow;
  REAL g0;

  enow = e[0];
  fnow = f[0];
  eindex = findex = 0;
  if ((fnow > enow) == (fnow > -enow)) {
    g0 = enow;
    enow = e[++eindex];
  } else {
    g0 = fnow;
    fnow = f[++findex];
  }
  if ((eindex < elen) && ((findex >= flen)
                          || ((fnow > enow) == (fnow > -enow)))) {
    Fast_Two_Sum(enow, g0, Qnew, q);
    enow = e[++eindex];
  } else {
    Fast_Two_Sum(fnow, g0, Qnew, q);
    fnow = f[++findex];
  }
  Q = Qnew;
  for (hindex = 0; hindex < elen + flen - 2; hindex++) {
    if ((eindex < elen) && ((findex >= flen)
                            || ((fnow > enow) == (fnow > -enow)))) {
      Fast_Two_Sum(enow, q, R, h[hindex]);
      enow = e[++eindex];
    } else {
      Fast_Two_Sum(fnow, q, R, h[hindex]);
      fnow = f[++findex];
    }
    Two_Sum(Q, R, Qnew, q);
    Q = Qnew;
  }
  h[hindex] = q;
  h[hindex + 1] = Q;
  return hindex + 2;
}

/*****************************************************************************/
/*                                                                           */
/*  linear_expansion_sum_zeroelim()   Sum two expansions, eliminating zero   */
/*                                    components from the output expansion.  */
/*                                                                           */
/*  Sets h = e + f.  See either version of my paper for details.             */
/*                                                                           */
/*  Maintains the nonoverlapping property.  (That is, if e is                */
/*  nonoverlapping, h will be also.)                                         */
/*                                                                           */
/*****************************************************************************/

int linear_expansion_sum_zeroelim(elen, e, flen, f, h)/* h cannot be e or f. */
int elen;
REAL *e;
int flen;
REAL *f;
REAL *h;
{
  REAL Q, q, hh;
  INEXACT REAL Qnew;
  INEXACT REAL R;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  int eindex, findex, hindex;
  int count;
  REAL enow, fnow;
  REAL g0;

  enow = e[0];
  fnow = f[0];
  eindex = findex = 0;
  hindex = 0;
  if ((fnow > enow) == (fnow > -enow)) {
    g0 = enow;
    enow = e[++eindex];
  } else {
    g0 = fnow;
    fnow = f[++findex];
  }
  if ((eindex < elen) && ((findex >= flen)
                          || ((fnow > enow) == (fnow > -enow)))) {
    Fast_Two_Sum(enow, g0, Qnew, q);
    enow = e[++eindex];
  } else {
    Fast_Two_Sum(fnow, g0, Qnew, q);
    fnow = f[++findex];
  }
  Q = Qnew;
  for (count = 2; count < elen + flen; count++) {
    if ((eindex < elen) && ((findex >= flen)
                            || ((fnow > enow) == (fnow > -enow)))) {
      Fast_Two_Sum(enow, q, R, hh);
      enow = e[++eindex];
    } else {
      Fast_Two_Sum(fnow, q, R, hh);
      fnow = f[++findex];
    }
    Two_Sum(Q, R, Qnew, q);
    Q = Qnew;
    if (hh != 0) {
      h[hindex++] = hh;
    }
  }
  if (q != 0) {
    h[hindex++] = q;
  }
  if ((Q != 0.0) || (hindex == 0)) {
    h[hindex++] = Q;
  }
  return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  scale_expansion()   Multiply an expansion by a scalar.                   */
/*                                                                           */
/*  Sets h = be.  See either version of my paper for details.                */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int scale_expansion(elen, e, b, h)            /* e and h cannot be the same. */
int elen;
REAL *e;
REAL b;
REAL *h;
{
  INEXACT REAL Q;
  INEXACT REAL sum;
  INEXACT REAL product1;
  REAL product0;
  int eindex, hindex;
  REAL enow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;

  Split(b, bhi, blo);
  Two_Product_Presplit(e[0], b, bhi, blo, Q, h[0]);
  hindex = 1;
  for (eindex = 1; eindex < elen; eindex++) {
    enow = e[eindex];
    Two_Product_Presplit(enow, b, bhi, blo, product1, product0);
    Two_Sum(Q, product0, sum, h[hindex]);
    hindex++;
    Two_Sum(product1, sum, Q, h[hindex]);
    hindex++;
  }
  h[hindex] = Q;
  return elen + elen;
}

/*****************************************************************************/
/*                                                                           */
/*  scale_expansion_zeroelim()   Multiply an expansion by a scalar,          */
/*                               eliminating zero components from the        */
/*                               output expansion.                           */
/*                                                                           */
/*  Sets h = be.  See either version of my paper for details.                */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int scale_expansion_zeroelim(elen, e, b, h)   /* e and h cannot be the same. */
int elen;
REAL *e;
REAL b;
REAL *h;
{
  INEXACT REAL Q, sum;
  REAL hh;
  INEXACT REAL product1;
  REAL product0;
  int eindex, hindex;
  REAL enow;
  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;

  Split(b, bhi, blo);
  Two_Product_Presplit(e[0], b, bhi, blo, Q, hh);
  hindex = 0;
  if (hh != 0) {
    h[hindex++] = hh;
  }
  for (eindex = 1; eindex < elen; eindex++) {
    enow = e[eindex];
    Two_Product_Presplit(enow, b, bhi, blo, product1, product0);
    Two_Sum(Q, product0, sum, hh);
    if (hh != 0) {
      h[hindex++] = hh;
    }
    Fast_Two_Sum(product1, sum, Q, hh);
    if (hh != 0) {
      h[hindex++] = hh;
    }
  }
  if ((Q != 0.0) || (hindex == 0)) {
    h[hindex++] = Q;
  }
  return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  compress()   Compress an expansion.                                      */
/*                                                                           */
/*  See the long version of my paper for details.                            */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), then any nonoverlapping expansion is converted to a      */
/*  nonadjacent expansion.                                                   */
/*                                                                           */
/*****************************************************************************/

//int compress(elen, e, h)                         /* e and h may be the same. */
//int elen;
//REAL *e;
//REAL *h;
//{
//  REAL Q, q;
//  INEXACT REAL Qnew;
//  int eindex, hindex;
//  INEXACT REAL bvirt;
//  REAL enow, hnow;
//  int top, bottom;
//
//  bottom = elen - 1;
//  Q = e[bottom];
//  for (eindex = elen - 2; eindex >= 0; eindex--) {
//    enow = e[eindex];
//    Fast_Two_Sum(Q, enow, Qnew, q);
//    if (q != 0) {
//      h[bottom--] = Qnew;
//      Q = q;
//    } else {
//      Q = Qnew;
//    }
//  }
//  top = 0;
//  for (hindex = bottom + 1; hindex < elen; hindex++) {
//    hnow = h[hindex];
//    Fast_Two_Sum(hnow, Q, Qnew, q);
//    if (q != 0) {
//      h[top++] = q;
//    }
//    Q = Qnew;
//  }
//  h[top] = Q;
//  return top + 1;
//}

/*****************************************************************************/
/*                                                                           */
/*  estimate()   Produce a one-word estimate of an expansion's value.        */
/*                                                                           */
/*  See either version of my paper for details.                              */
/*                                                                           */
/*****************************************************************************/

REAL estimate(elen, e)
int elen;
REAL *e;
{
  REAL Q;
  int eindex;

  Q = e[0];
  for (eindex = 1; eindex < elen; eindex++) {
    Q += e[eindex];
  }
  return Q;
}

/*****************************************************************************/
/*                                                                           */
/*  orient2dfast()   Approximate 2D orientation test.  Nonrobust.            */
/*  orient2dexact()   Exact 2D orientation test.  Robust.                    */
/*  orient2dslow()   Another exact 2D orientation test.  Robust.             */
/*  orient2d()   Adaptive exact 2D orientation test.  Robust.                */
/*                                                                           */
/*               Return a positive value if the points pa, pb, and pc occur  */
/*               in counterclockwise order; a negative value if they occur   */
/*               in clockwise order; and zero if they are collinear.  The    */
/*               result is also a rough approximation of twice the signed    */
/*               area of the triangle defined by the three points.           */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In orient2d() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, orient2d() is usually quite */
/*  fast, but will run more slowly when the input points are collinear or    */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL orient2dfast(pa, pb, pc)
REAL *pa;
REAL *pb;
REAL *pc;
{
  REAL acx, bcx, acy, bcy;

  acx = pa[0] - pc[0];
  bcx = pb[0] - pc[0];
  acy = pa[1] - pc[1];
  bcy = pb[1] - pc[1];
  return acx * bcy - acy * bcx;
}

REAL orient2dexact(pa, pb, pc)
REAL *pa;
REAL *pb;
REAL *pc;
{
  INEXACT REAL axby1, axcy1, bxcy1, bxay1, cxay1, cxby1;
  REAL axby0, axcy0, bxcy0, bxay0, cxay0, cxby0;
  REAL aterms[4], bterms[4], cterms[4];
  INEXACT REAL aterms3, bterms3, cterms3;
  REAL v[8], w[12];
  int vlength, wlength;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  Two_Product(pa[0], pb[1], axby1, axby0);
  Two_Product(pa[0], pc[1], axcy1, axcy0);
  Two_Two_Diff(axby1, axby0, axcy1, axcy0,
               aterms3, aterms[2], aterms[1], aterms[0]);
  aterms[3] = aterms3;

  Two_Product(pb[0], pc[1], bxcy1, bxcy0);
  Two_Product(pb[0], pa[1], bxay1, bxay0);
  Two_Two_Diff(bxcy1, bxcy0, bxay1, bxay0,
               bterms3, bterms[2], bterms[1], bterms[0]);
  bterms[3] = bterms3;

  Two_Product(pc[0], pa[1], cxay1, cxay0);
  Two_Product(pc[0], pb[1], cxby1, cxby0);
  Two_Two_Diff(cxay1, cxay0, cxby1, cxby0,
               cterms3, cterms[2], cterms[1], cterms[0]);
  cterms[3] = cterms3;

  vlength = fast_expansion_sum_zeroelim(4, aterms, 4, bterms, v);
  wlength = fast_expansion_sum_zeroelim(vlength, v, 4, cterms, w);

  return w[wlength - 1];
}

REAL orient2dslow(pa, pb, pc)
REAL *pa;
REAL *pb;
REAL *pc;
{
  INEXACT REAL acx, acy, bcx, bcy;
  REAL acxtail, acytail;
  REAL bcxtail, bcytail;
  REAL negate, negatetail;
  REAL axby[8], bxay[8];
  INEXACT REAL axby7, bxay7;
  REAL deter[16];
  int deterlen;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j, _k, _l, _m, _n;
  REAL _0, _1, _2;

  Two_Diff(pa[0], pc[0], acx, acxtail);
  Two_Diff(pa[1], pc[1], acy, acytail);
  Two_Diff(pb[0], pc[0], bcx, bcxtail);
  Two_Diff(pb[1], pc[1], bcy, bcytail);

  Two_Two_Product(acx, acxtail, bcy, bcytail,
                  axby7, axby[6], axby[5], axby[4],
                  axby[3], axby[2], axby[1], axby[0]);
  axby[7] = axby7;
  negate = -acy;
  negatetail = -acytail;
  Two_Two_Product(bcx, bcxtail, negate, negatetail,
                  bxay7, bxay[6], bxay[5], bxay[4],
                  bxay[3], bxay[2], bxay[1], bxay[0]);
  bxay[7] = bxay7;

  deterlen = fast_expansion_sum_zeroelim(8, axby, 8, bxay, deter);

  return deter[deterlen - 1];
}

REAL orient2dadapt(pa, pb, pc, detsum)
REAL *pa;
REAL *pb;
REAL *pc;
REAL detsum;
{
  INEXACT REAL acx, acy, bcx, bcy;
  REAL acxtail, acytail, bcxtail, bcytail;
  INEXACT REAL detleft, detright;
  REAL detlefttail, detrighttail;
  REAL det, errbound;
  REAL B[4], C1[8], C2[12], D[16];
  INEXACT REAL B3;
  int C1length, C2length, Dlength;
  REAL u[4];
  INEXACT REAL u3;
  INEXACT REAL s1, t1;
  REAL s0, t0;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  acx = (REAL) (pa[0] - pc[0]);
  bcx = (REAL) (pb[0] - pc[0]);
  acy = (REAL) (pa[1] - pc[1]);
  bcy = (REAL) (pb[1] - pc[1]);

  Two_Product(acx, bcy, detleft, detlefttail);
  Two_Product(acy, bcx, detright, detrighttail);

  Two_Two_Diff(detleft, detlefttail, detright, detrighttail,
               B3, B[2], B[1], B[0]);
  B[3] = B3;

  det = estimate(4, B);
  errbound = ccwerrboundB * detsum;
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  Two_Diff_Tail(pa[0], pc[0], acx, acxtail);
  Two_Diff_Tail(pb[0], pc[0], bcx, bcxtail);
  Two_Diff_Tail(pa[1], pc[1], acy, acytail);
  Two_Diff_Tail(pb[1], pc[1], bcy, bcytail);

  if ((acxtail == 0.0) && (acytail == 0.0)
      && (bcxtail == 0.0) && (bcytail == 0.0)) {
    return det;
  }

  errbound = ccwerrboundC * detsum + resulterrbound * Absolute(det);
  det += (acx * bcytail + bcy * acxtail)
       - (acy * bcxtail + bcx * acytail);
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  Two_Product(acxtail, bcy, s1, s0);
  Two_Product(acytail, bcx, t1, t0);
  Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
  u[3] = u3;
  C1length = fast_expansion_sum_zeroelim(4, B, 4, u, C1);

  Two_Product(acx, bcytail, s1, s0);
  Two_Product(acy, bcxtail, t1, t0);
  Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
  u[3] = u3;
  C2length = fast_expansion_sum_zeroelim(C1length, C1, 4, u, C2);

  Two_Product(acxtail, bcytail, s1, s0);
  Two_Product(acytail, bcxtail, t1, t0);
  Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
  u[3] = u3;
  Dlength = fast_expansion_sum_zeroelim(C2length, C2, 4, u, D);

  return(D[Dlength - 1]);
}

REAL orient2d(pa, pb, pc)
REAL *pa;
REAL *pb;
REAL *pc;
{
  REAL detleft, detright, det;
  REAL detsum, errbound;

  detleft = (pa[0] - pc[0]) * (pb[1] - pc[1]);
  detright = (pa[1] - pc[1]) * (pb[0] - pc[0]);
  det = detleft - detright;

  if (detleft > 0.0) {
    if (detright <= 0.0) {
      return det;
    } else {
      detsum = detleft + detright;
    }
  } else if (detleft < 0.0) {
    if (detright >= 0.0) {
      return det;
    } else {
      detsum = -detleft - detright;
    }
  } else {
    return det;
  }

  errbound = ccwerrboundA * detsum;
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  return orient2dadapt(pa, pb, pc, detsum);
}

/*****************************************************************************/
/*                                                                           */
/*  orient3dfast()   Approximate 3D orientation test.  Nonrobust.            */
/*  orient3dexact()   Exact 3D orientation test.  Robust.                    */
/*  orient3dslow()   Another exact 3D orientation test.  Robust.             */
/*  orient3d()   Adaptive exact 3D orientation test.  Robust.                */
/*                                                                           */
/*               Return a positive value if the point pd lies below the      */
/*               plane passing through pa, pb, and pc; "below" is defined so */
/*               that pa, pb, and pc appear in counterclockwise order when   */
/*               viewed from above the plane.  Returns a negative value if   */
/*               pd lies above the plane.  Returns zero if the points are    */
/*               coplanar.  The result is also a rough approximation of six  */
/*               times the signed volume of the tetrahedron defined by the   */
/*               four points.                                                */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In orient3d() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, orient3d() is usually quite */
/*  fast, but will run more slowly when the input points are coplanar or     */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL orient3dfast(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  REAL adx, bdx, cdx;
  REAL ady, bdy, cdy;
  REAL adz, bdz, cdz;

  adx = pa[0] - pd[0];
  bdx = pb[0] - pd[0];
  cdx = pc[0] - pd[0];
  ady = pa[1] - pd[1];
  bdy = pb[1] - pd[1];
  cdy = pc[1] - pd[1];
  adz = pa[2] - pd[2];
  bdz = pb[2] - pd[2];
  cdz = pc[2] - pd[2];

  return adx * (bdy * cdz - bdz * cdy)
       + bdx * (cdy * adz - cdz * ady)
       + cdx * (ady * bdz - adz * bdy);
}

REAL orient3dexact(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  INEXACT REAL axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
  INEXACT REAL bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
  REAL axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
  REAL bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
  REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
  REAL temp8[8];
  int templen;
  REAL abc[12], bcd[12], cda[12], dab[12];
  int abclen, bcdlen, cdalen, dablen;
  REAL adet[24], bdet[24], cdet[24], ddet[24];
  int alen, blen, clen, dlen;
  REAL abdet[48], cddet[48];
  int ablen, cdlen;
  REAL deter[96];
  int deterlen;
  int i;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  Two_Product(pa[0], pb[1], axby1, axby0);
  Two_Product(pb[0], pa[1], bxay1, bxay0);
  Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

  Two_Product(pb[0], pc[1], bxcy1, bxcy0);
  Two_Product(pc[0], pb[1], cxby1, cxby0);
  Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

  Two_Product(pc[0], pd[1], cxdy1, cxdy0);
  Two_Product(pd[0], pc[1], dxcy1, dxcy0);
  Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

  Two_Product(pd[0], pa[1], dxay1, dxay0);
  Two_Product(pa[0], pd[1], axdy1, axdy0);
  Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

  Two_Product(pa[0], pc[1], axcy1, axcy0);
  Two_Product(pc[0], pa[1], cxay1, cxay0);
  Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

  Two_Product(pb[0], pd[1], bxdy1, bxdy0);
  Two_Product(pd[0], pb[1], dxby1, dxby0);
  Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

  templen = fast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
  cdalen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
  templen = fast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
  dablen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);
  for (i = 0; i < 4; i++) {
    bd[i] = -bd[i];
    ac[i] = -ac[i];
  }
  templen = fast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
  abclen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
  templen = fast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
  bcdlen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

  alen = scale_expansion_zeroelim(bcdlen, bcd, pa[2], adet);
  blen = scale_expansion_zeroelim(cdalen, cda, -pb[2], bdet);
  clen = scale_expansion_zeroelim(dablen, dab, pc[2], cdet);
  dlen = scale_expansion_zeroelim(abclen, abc, -pd[2], ddet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

  return deter[deterlen - 1];
}

REAL orient3dslow(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  INEXACT REAL adx, ady, adz, bdx, bdy, bdz, cdx, cdy, cdz;
  REAL adxtail, adytail, adztail;
  REAL bdxtail, bdytail, bdztail;
  REAL cdxtail, cdytail, cdztail;
  REAL negate, negatetail;
  INEXACT REAL axby7, bxcy7, axcy7, bxay7, cxby7, cxay7;
  REAL axby[8], bxcy[8], axcy[8], bxay[8], cxby[8], cxay[8];
  REAL temp16[16], temp32[32], temp32t[32];
  int temp16len, temp32len, temp32tlen;
  REAL adet[64], bdet[64], cdet[64];
  int alen, blen, clen;
  REAL abdet[128];
  int ablen;
  REAL deter[192];
  int deterlen;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j, _k, _l, _m, _n;
  REAL _0, _1, _2;

  Two_Diff(pa[0], pd[0], adx, adxtail);
  Two_Diff(pa[1], pd[1], ady, adytail);
  Two_Diff(pa[2], pd[2], adz, adztail);
  Two_Diff(pb[0], pd[0], bdx, bdxtail);
  Two_Diff(pb[1], pd[1], bdy, bdytail);
  Two_Diff(pb[2], pd[2], bdz, bdztail);
  Two_Diff(pc[0], pd[0], cdx, cdxtail);
  Two_Diff(pc[1], pd[1], cdy, cdytail);
  Two_Diff(pc[2], pd[2], cdz, cdztail);

  Two_Two_Product(adx, adxtail, bdy, bdytail,
                  axby7, axby[6], axby[5], axby[4],
                  axby[3], axby[2], axby[1], axby[0]);
  axby[7] = axby7;
  negate = -ady;
  negatetail = -adytail;
  Two_Two_Product(bdx, bdxtail, negate, negatetail,
                  bxay7, bxay[6], bxay[5], bxay[4],
                  bxay[3], bxay[2], bxay[1], bxay[0]);
  bxay[7] = bxay7;
  Two_Two_Product(bdx, bdxtail, cdy, cdytail,
                  bxcy7, bxcy[6], bxcy[5], bxcy[4],
                  bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
  bxcy[7] = bxcy7;
  negate = -bdy;
  negatetail = -bdytail;
  Two_Two_Product(cdx, cdxtail, negate, negatetail,
                  cxby7, cxby[6], cxby[5], cxby[4],
                  cxby[3], cxby[2], cxby[1], cxby[0]);
  cxby[7] = cxby7;
  Two_Two_Product(cdx, cdxtail, ady, adytail,
                  cxay7, cxay[6], cxay[5], cxay[4],
                  cxay[3], cxay[2], cxay[1], cxay[0]);
  cxay[7] = cxay7;
  negate = -cdy;
  negatetail = -cdytail;
  Two_Two_Product(adx, adxtail, negate, negatetail,
                  axcy7, axcy[6], axcy[5], axcy[4],
                  axcy[3], axcy[2], axcy[1], axcy[0]);
  axcy[7] = axcy7;

  temp16len = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, temp16);
  temp32len = scale_expansion_zeroelim(temp16len, temp16, adz, temp32);
  temp32tlen = scale_expansion_zeroelim(temp16len, temp16, adztail, temp32t);
  alen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
                                     adet);

  temp16len = fast_expansion_sum_zeroelim(8, cxay, 8, axcy, temp16);
  temp32len = scale_expansion_zeroelim(temp16len, temp16, bdz, temp32);
  temp32tlen = scale_expansion_zeroelim(temp16len, temp16, bdztail, temp32t);
  blen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
                                     bdet);

  temp16len = fast_expansion_sum_zeroelim(8, axby, 8, bxay, temp16);
  temp32len = scale_expansion_zeroelim(temp16len, temp16, cdz, temp32);
  temp32tlen = scale_expansion_zeroelim(temp16len, temp16, cdztail, temp32t);
  clen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
                                     cdet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, deter);

  return deter[deterlen - 1];
}

REAL orient3dadapt(pa, pb, pc, pd, permanent)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL permanent;
{
  INEXACT REAL adx, bdx, cdx, ady, bdy, cdy, adz, bdz, cdz;
  REAL det, errbound;

  INEXACT REAL bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
  REAL bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
  REAL bc[4], ca[4], ab[4];
  INEXACT REAL bc3, ca3, ab3;
  REAL adet[8], bdet[8], cdet[8];
  int alen, blen, clen;
  REAL abdet[16];
  int ablen;
  REAL *finnow, *finother, *finswap;
  REAL fin1[192], fin2[192];
  int finlength;

  REAL adxtail, bdxtail, cdxtail;
  REAL adytail, bdytail, cdytail;
  REAL adztail, bdztail, cdztail;
  INEXACT REAL at_blarge, at_clarge;
  INEXACT REAL bt_clarge, bt_alarge;
  INEXACT REAL ct_alarge, ct_blarge;
  REAL at_b[4], at_c[4], bt_c[4], bt_a[4], ct_a[4], ct_b[4];
  int at_blen, at_clen, bt_clen, bt_alen, ct_alen, ct_blen;
  INEXACT REAL bdxt_cdy1, cdxt_bdy1, cdxt_ady1;
  INEXACT REAL adxt_cdy1, adxt_bdy1, bdxt_ady1;
  REAL bdxt_cdy0, cdxt_bdy0, cdxt_ady0;
  REAL adxt_cdy0, adxt_bdy0, bdxt_ady0;
  INEXACT REAL bdyt_cdx1, cdyt_bdx1, cdyt_adx1;
  INEXACT REAL adyt_cdx1, adyt_bdx1, bdyt_adx1;
  REAL bdyt_cdx0, cdyt_bdx0, cdyt_adx0;
  REAL adyt_cdx0, adyt_bdx0, bdyt_adx0;
  REAL bct[8], cat[8], abt[8];
  int bctlen, catlen, abtlen;
  INEXACT REAL bdxt_cdyt1, cdxt_bdyt1, cdxt_adyt1;
  INEXACT REAL adxt_cdyt1, adxt_bdyt1, bdxt_adyt1;
  REAL bdxt_cdyt0, cdxt_bdyt0, cdxt_adyt0;
  REAL adxt_cdyt0, adxt_bdyt0, bdxt_adyt0;
  REAL u[4], v[12], w[16];
  INEXACT REAL u3;
  int vlength, wlength;
  REAL negate;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j, _k;
  REAL _0;

  adx = (REAL) (pa[0] - pd[0]);
  bdx = (REAL) (pb[0] - pd[0]);
  cdx = (REAL) (pc[0] - pd[0]);
  ady = (REAL) (pa[1] - pd[1]);
  bdy = (REAL) (pb[1] - pd[1]);
  cdy = (REAL) (pc[1] - pd[1]);
  adz = (REAL) (pa[2] - pd[2]);
  bdz = (REAL) (pb[2] - pd[2]);
  cdz = (REAL) (pc[2] - pd[2]);

  Two_Product(bdx, cdy, bdxcdy1, bdxcdy0);
  Two_Product(cdx, bdy, cdxbdy1, cdxbdy0);
  Two_Two_Diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
  bc[3] = bc3;
  alen = scale_expansion_zeroelim(4, bc, adz, adet);

  Two_Product(cdx, ady, cdxady1, cdxady0);
  Two_Product(adx, cdy, adxcdy1, adxcdy0);
  Two_Two_Diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
  ca[3] = ca3;
  blen = scale_expansion_zeroelim(4, ca, bdz, bdet);

  Two_Product(adx, bdy, adxbdy1, adxbdy0);
  Two_Product(bdx, ady, bdxady1, bdxady0);
  Two_Two_Diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
  ab[3] = ab3;
  clen = scale_expansion_zeroelim(4, ab, cdz, cdet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

  det = estimate(finlength, fin1);
  errbound = o3derrboundB * permanent;
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  Two_Diff_Tail(pa[0], pd[0], adx, adxtail);
  Two_Diff_Tail(pb[0], pd[0], bdx, bdxtail);
  Two_Diff_Tail(pc[0], pd[0], cdx, cdxtail);
  Two_Diff_Tail(pa[1], pd[1], ady, adytail);
  Two_Diff_Tail(pb[1], pd[1], bdy, bdytail);
  Two_Diff_Tail(pc[1], pd[1], cdy, cdytail);
  Two_Diff_Tail(pa[2], pd[2], adz, adztail);
  Two_Diff_Tail(pb[2], pd[2], bdz, bdztail);
  Two_Diff_Tail(pc[2], pd[2], cdz, cdztail);

  if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
      && (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)
      && (adztail == 0.0) && (bdztail == 0.0) && (cdztail == 0.0)) {
    return det;
  }

  errbound = o3derrboundC * permanent + resulterrbound * Absolute(det);
  det += (adz * ((bdx * cdytail + cdy * bdxtail)
                 - (bdy * cdxtail + cdx * bdytail))
          + adztail * (bdx * cdy - bdy * cdx))
       + (bdz * ((cdx * adytail + ady * cdxtail)
                 - (cdy * adxtail + adx * cdytail))
          + bdztail * (cdx * ady - cdy * adx))
       + (cdz * ((adx * bdytail + bdy * adxtail)
                 - (ady * bdxtail + bdx * adytail))
          + cdztail * (adx * bdy - ady * bdx));
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  finnow = fin1;
  finother = fin2;

  if (adxtail == 0.0) {
    if (adytail == 0.0) {
      at_b[0] = 0.0;
      at_blen = 1;
      at_c[0] = 0.0;
      at_clen = 1;
    } else {
      negate = -adytail;
      Two_Product(negate, bdx, at_blarge, at_b[0]);
      at_b[1] = at_blarge;
      at_blen = 2;
      Two_Product(adytail, cdx, at_clarge, at_c[0]);
      at_c[1] = at_clarge;
      at_clen = 2;
    }
  } else {
    if (adytail == 0.0) {
      Two_Product(adxtail, bdy, at_blarge, at_b[0]);
      at_b[1] = at_blarge;
      at_blen = 2;
      negate = -adxtail;
      Two_Product(negate, cdy, at_clarge, at_c[0]);
      at_c[1] = at_clarge;
      at_clen = 2;
    } else {
      Two_Product(adxtail, bdy, adxt_bdy1, adxt_bdy0);
      Two_Product(adytail, bdx, adyt_bdx1, adyt_bdx0);
      Two_Two_Diff(adxt_bdy1, adxt_bdy0, adyt_bdx1, adyt_bdx0,
                   at_blarge, at_b[2], at_b[1], at_b[0]);
      at_b[3] = at_blarge;
      at_blen = 4;
      Two_Product(adytail, cdx, adyt_cdx1, adyt_cdx0);
      Two_Product(adxtail, cdy, adxt_cdy1, adxt_cdy0);
      Two_Two_Diff(adyt_cdx1, adyt_cdx0, adxt_cdy1, adxt_cdy0,
                   at_clarge, at_c[2], at_c[1], at_c[0]);
      at_c[3] = at_clarge;
      at_clen = 4;
    }
  }
  if (bdxtail == 0.0) {
    if (bdytail == 0.0) {
      bt_c[0] = 0.0;
      bt_clen = 1;
      bt_a[0] = 0.0;
      bt_alen = 1;
    } else {
      negate = -bdytail;
      Two_Product(negate, cdx, bt_clarge, bt_c[0]);
      bt_c[1] = bt_clarge;
      bt_clen = 2;
      Two_Product(bdytail, adx, bt_alarge, bt_a[0]);
      bt_a[1] = bt_alarge;
      bt_alen = 2;
    }
  } else {
    if (bdytail == 0.0) {
      Two_Product(bdxtail, cdy, bt_clarge, bt_c[0]);
      bt_c[1] = bt_clarge;
      bt_clen = 2;
      negate = -bdxtail;
      Two_Product(negate, ady, bt_alarge, bt_a[0]);
      bt_a[1] = bt_alarge;
      bt_alen = 2;
    } else {
      Two_Product(bdxtail, cdy, bdxt_cdy1, bdxt_cdy0);
      Two_Product(bdytail, cdx, bdyt_cdx1, bdyt_cdx0);
      Two_Two_Diff(bdxt_cdy1, bdxt_cdy0, bdyt_cdx1, bdyt_cdx0,
                   bt_clarge, bt_c[2], bt_c[1], bt_c[0]);
      bt_c[3] = bt_clarge;
      bt_clen = 4;
      Two_Product(bdytail, adx, bdyt_adx1, bdyt_adx0);
      Two_Product(bdxtail, ady, bdxt_ady1, bdxt_ady0);
      Two_Two_Diff(bdyt_adx1, bdyt_adx0, bdxt_ady1, bdxt_ady0,
                  bt_alarge, bt_a[2], bt_a[1], bt_a[0]);
      bt_a[3] = bt_alarge;
      bt_alen = 4;
    }
  }
  if (cdxtail == 0.0) {
    if (cdytail == 0.0) {
      ct_a[0] = 0.0;
      ct_alen = 1;
      ct_b[0] = 0.0;
      ct_blen = 1;
    } else {
      negate = -cdytail;
      Two_Product(negate, adx, ct_alarge, ct_a[0]);
      ct_a[1] = ct_alarge;
      ct_alen = 2;
      Two_Product(cdytail, bdx, ct_blarge, ct_b[0]);
      ct_b[1] = ct_blarge;
      ct_blen = 2;
    }
  } else {
    if (cdytail == 0.0) {
      Two_Product(cdxtail, ady, ct_alarge, ct_a[0]);
      ct_a[1] = ct_alarge;
      ct_alen = 2;
      negate = -cdxtail;
      Two_Product(negate, bdy, ct_blarge, ct_b[0]);
      ct_b[1] = ct_blarge;
      ct_blen = 2;
    } else {
      Two_Product(cdxtail, ady, cdxt_ady1, cdxt_ady0);
      Two_Product(cdytail, adx, cdyt_adx1, cdyt_adx0);
      Two_Two_Diff(cdxt_ady1, cdxt_ady0, cdyt_adx1, cdyt_adx0,
                   ct_alarge, ct_a[2], ct_a[1], ct_a[0]);
      ct_a[3] = ct_alarge;
      ct_alen = 4;
      Two_Product(cdytail, bdx, cdyt_bdx1, cdyt_bdx0);
      Two_Product(cdxtail, bdy, cdxt_bdy1, cdxt_bdy0);
      Two_Two_Diff(cdyt_bdx1, cdyt_bdx0, cdxt_bdy1, cdxt_bdy0,
                   ct_blarge, ct_b[2], ct_b[1], ct_b[0]);
      ct_b[3] = ct_blarge;
      ct_blen = 4;
    }
  }

  bctlen = fast_expansion_sum_zeroelim(bt_clen, bt_c, ct_blen, ct_b, bct);
  wlength = scale_expansion_zeroelim(bctlen, bct, adz, w);
  finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                          finother);
  finswap = finnow; finnow = finother; finother = finswap;

  catlen = fast_expansion_sum_zeroelim(ct_alen, ct_a, at_clen, at_c, cat);
  wlength = scale_expansion_zeroelim(catlen, cat, bdz, w);
  finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                          finother);
  finswap = finnow; finnow = finother; finother = finswap;

  abtlen = fast_expansion_sum_zeroelim(at_blen, at_b, bt_alen, bt_a, abt);
  wlength = scale_expansion_zeroelim(abtlen, abt, cdz, w);
  finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                          finother);
  finswap = finnow; finnow = finother; finother = finswap;

  if (adztail != 0.0) {
    vlength = scale_expansion_zeroelim(4, bc, adztail, v);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (bdztail != 0.0) {
    vlength = scale_expansion_zeroelim(4, ca, bdztail, v);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (cdztail != 0.0) {
    vlength = scale_expansion_zeroelim(4, ab, cdztail, v);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }

  if (adxtail != 0.0) {
    if (bdytail != 0.0) {
      Two_Product(adxtail, bdytail, adxt_bdyt1, adxt_bdyt0);
      Two_One_Product(adxt_bdyt1, adxt_bdyt0, cdz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (cdztail != 0.0) {
        Two_One_Product(adxt_bdyt1, adxt_bdyt0, cdztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
    if (cdytail != 0.0) {
      negate = -adxtail;
      Two_Product(negate, cdytail, adxt_cdyt1, adxt_cdyt0);
      Two_One_Product(adxt_cdyt1, adxt_cdyt0, bdz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (bdztail != 0.0) {
        Two_One_Product(adxt_cdyt1, adxt_cdyt0, bdztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
  }
  if (bdxtail != 0.0) {
    if (cdytail != 0.0) {
      Two_Product(bdxtail, cdytail, bdxt_cdyt1, bdxt_cdyt0);
      Two_One_Product(bdxt_cdyt1, bdxt_cdyt0, adz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (adztail != 0.0) {
        Two_One_Product(bdxt_cdyt1, bdxt_cdyt0, adztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
    if (adytail != 0.0) {
      negate = -bdxtail;
      Two_Product(negate, adytail, bdxt_adyt1, bdxt_adyt0);
      Two_One_Product(bdxt_adyt1, bdxt_adyt0, cdz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (cdztail != 0.0) {
        Two_One_Product(bdxt_adyt1, bdxt_adyt0, cdztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
  }
  if (cdxtail != 0.0) {
    if (adytail != 0.0) {
      Two_Product(cdxtail, adytail, cdxt_adyt1, cdxt_adyt0);
      Two_One_Product(cdxt_adyt1, cdxt_adyt0, bdz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (bdztail != 0.0) {
        Two_One_Product(cdxt_adyt1, cdxt_adyt0, bdztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
    if (bdytail != 0.0) {
      negate = -cdxtail;
      Two_Product(negate, bdytail, cdxt_bdyt1, cdxt_bdyt0);
      Two_One_Product(cdxt_bdyt1, cdxt_bdyt0, adz, u3, u[2], u[1], u[0]);
      u[3] = u3;
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                              finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (adztail != 0.0) {
        Two_One_Product(cdxt_bdyt1, cdxt_bdyt0, adztail, u3, u[2], u[1], u[0]);
        u[3] = u3;
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                                                finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
    }
  }

  if (adztail != 0.0) {
    wlength = scale_expansion_zeroelim(bctlen, bct, adztail, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (bdztail != 0.0) {
    wlength = scale_expansion_zeroelim(catlen, cat, bdztail, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (cdztail != 0.0) {
    wlength = scale_expansion_zeroelim(abtlen, abt, cdztail, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
                                            finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }

  return finnow[finlength - 1];
}

REAL orient3d(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  REAL adx, bdx, cdx, ady, bdy, cdy, adz, bdz, cdz;
  REAL bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
  REAL det;
  REAL permanent, errbound;

  adx = pa[0] - pd[0];
  bdx = pb[0] - pd[0];
  cdx = pc[0] - pd[0];
  ady = pa[1] - pd[1];
  bdy = pb[1] - pd[1];
  cdy = pc[1] - pd[1];
  adz = pa[2] - pd[2];
  bdz = pb[2] - pd[2];
  cdz = pc[2] - pd[2];

  bdxcdy = bdx * cdy;
  cdxbdy = cdx * bdy;

  cdxady = cdx * ady;
  adxcdy = adx * cdy;

  adxbdy = adx * bdy;
  bdxady = bdx * ady;

  det = adz * (bdxcdy - cdxbdy)
      + bdz * (cdxady - adxcdy)
      + cdz * (adxbdy - bdxady);

  permanent = (Absolute(bdxcdy) + Absolute(cdxbdy)) * Absolute(adz)
            + (Absolute(cdxady) + Absolute(adxcdy)) * Absolute(bdz)
            + (Absolute(adxbdy) + Absolute(bdxady)) * Absolute(cdz);
  errbound = o3derrboundA * permanent;
  if ((det > errbound) || (-det > errbound)) {
    return det;
  }

  return orient3dadapt(pa, pb, pc, pd, permanent);
}

/*****************************************************************************/
/*                                                                           */
/*  incirclefast()   Approximate 2D incircle test.  Nonrobust.               */
/*  incircleexact()   Exact 2D incircle test.  Robust.                       */
/*  incircleslow()   Another exact 2D incircle test.  Robust.                */
/*  incircle()   Adaptive exact 2D incircle test.  Robust.                   */
/*                                                                           */
/*               Return a positive value if the point pd lies inside the     */
/*               circle passing through pa, pb, and pc; a negative value if  */
/*               it lies outside; and zero if the four points are cocircular.*/
/*               The points pa, pb, and pc must be in counterclockwise       */
/*               order, or the sign of the result will be reversed.          */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In incircle() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, incircle() is usually quite */
/*  fast, but will run more slowly when the input points are cocircular or   */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL incirclefast(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  REAL adx, ady, bdx, bdy, cdx, cdy;
  REAL abdet, bcdet, cadet;
  REAL alift, blift, clift;

  adx = pa[0] - pd[0];
  ady = pa[1] - pd[1];
  bdx = pb[0] - pd[0];
  bdy = pb[1] - pd[1];
  cdx = pc[0] - pd[0];
  cdy = pc[1] - pd[1];

  abdet = adx * bdy - bdx * ady;
  bcdet = bdx * cdy - cdx * bdy;
  cadet = cdx * ady - adx * cdy;
  alift = adx * adx + ady * ady;
  blift = bdx * bdx + bdy * bdy;
  clift = cdx * cdx + cdy * cdy;

  return alift * bcdet + blift * cadet + clift * abdet;
}

REAL incircleexact(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  INEXACT REAL axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
  INEXACT REAL bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
  REAL axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
  REAL bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
  REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
  REAL temp8[8];
  int templen;
  REAL abc[12], bcd[12], cda[12], dab[12];
  int abclen, bcdlen, cdalen, dablen;
  REAL det24x[24], det24y[24], det48x[48], det48y[48];
  int xlen, ylen;
  REAL adet[96], bdet[96], cdet[96], ddet[96];
  int alen, blen, clen, dlen;
  REAL abdet[192], cddet[192];
  int ablen, cdlen;
  REAL deter[384];
  int deterlen;
  int i;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  Two_Product(pa[0], pb[1], axby1, axby0);
  Two_Product(pb[0], pa[1], bxay1, bxay0);
  Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

  Two_Product(pb[0], pc[1], bxcy1, bxcy0);
  Two_Product(pc[0], pb[1], cxby1, cxby0);
  Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

  Two_Product(pc[0], pd[1], cxdy1, cxdy0);
  Two_Product(pd[0], pc[1], dxcy1, dxcy0);
  Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

  Two_Product(pd[0], pa[1], dxay1, dxay0);
  Two_Product(pa[0], pd[1], axdy1, axdy0);
  Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

  Two_Product(pa[0], pc[1], axcy1, axcy0);
  Two_Product(pc[0], pa[1], cxay1, cxay0);
  Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

  Two_Product(pb[0], pd[1], bxdy1, bxdy0);
  Two_Product(pd[0], pb[1], dxby1, dxby0);
  Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

  templen = fast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
  cdalen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
  templen = fast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
  dablen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);
  for (i = 0; i < 4; i++) {
    bd[i] = -bd[i];
    ac[i] = -ac[i];
  }
  templen = fast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
  abclen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
  templen = fast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
  bcdlen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

  xlen = scale_expansion_zeroelim(bcdlen, bcd, pa[0], det24x);
  xlen = scale_expansion_zeroelim(xlen, det24x, pa[0], det48x);
  ylen = scale_expansion_zeroelim(bcdlen, bcd, pa[1], det24y);
  ylen = scale_expansion_zeroelim(ylen, det24y, pa[1], det48y);
  alen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, adet);

  xlen = scale_expansion_zeroelim(cdalen, cda, pb[0], det24x);
  xlen = scale_expansion_zeroelim(xlen, det24x, -pb[0], det48x);
  ylen = scale_expansion_zeroelim(cdalen, cda, pb[1], det24y);
  ylen = scale_expansion_zeroelim(ylen, det24y, -pb[1], det48y);
  blen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, bdet);

  xlen = scale_expansion_zeroelim(dablen, dab, pc[0], det24x);
  xlen = scale_expansion_zeroelim(xlen, det24x, pc[0], det48x);
  ylen = scale_expansion_zeroelim(dablen, dab, pc[1], det24y);
  ylen = scale_expansion_zeroelim(ylen, det24y, pc[1], det48y);
  clen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, cdet);

  xlen = scale_expansion_zeroelim(abclen, abc, pd[0], det24x);
  xlen = scale_expansion_zeroelim(xlen, det24x, -pd[0], det48x);
  ylen = scale_expansion_zeroelim(abclen, abc, pd[1], det24y);
  ylen = scale_expansion_zeroelim(ylen, det24y, -pd[1], det48y);
  dlen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, ddet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

  return deter[deterlen - 1];
}

REAL incircleslow(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  INEXACT REAL adx, bdx, cdx, ady, bdy, cdy;
  REAL adxtail, bdxtail, cdxtail;
  REAL adytail, bdytail, cdytail;
  REAL negate, negatetail;
  INEXACT REAL axby7, bxcy7, axcy7, bxay7, cxby7, cxay7;
  REAL axby[8], bxcy[8], axcy[8], bxay[8], cxby[8], cxay[8];
  REAL temp16[16];
  int temp16len;
  REAL detx[32], detxx[64], detxt[32], detxxt[64], detxtxt[64];
  int xlen, xxlen, xtlen, xxtlen, xtxtlen;
  REAL x1[128], x2[192];
  int x1len, x2len;
  REAL dety[32], detyy[64], detyt[32], detyyt[64], detytyt[64];
  int ylen, yylen, ytlen, yytlen, ytytlen;
  REAL y1[128], y2[192];
  int y1len, y2len;
  REAL adet[384], bdet[384], cdet[384], abdet[768], deter[1152];
  int alen, blen, clen, ablen, deterlen;
  int i;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j, _k, _l, _m, _n;
  REAL _0, _1, _2;

  Two_Diff(pa[0], pd[0], adx, adxtail);
  Two_Diff(pa[1], pd[1], ady, adytail);
  Two_Diff(pb[0], pd[0], bdx, bdxtail);
  Two_Diff(pb[1], pd[1], bdy, bdytail);
  Two_Diff(pc[0], pd[0], cdx, cdxtail);
  Two_Diff(pc[1], pd[1], cdy, cdytail);

  Two_Two_Product(adx, adxtail, bdy, bdytail,
                  axby7, axby[6], axby[5], axby[4],
                  axby[3], axby[2], axby[1], axby[0]);
  axby[7] = axby7;
  negate = -ady;
  negatetail = -adytail;
  Two_Two_Product(bdx, bdxtail, negate, negatetail,
                  bxay7, bxay[6], bxay[5], bxay[4],
                  bxay[3], bxay[2], bxay[1], bxay[0]);
  bxay[7] = bxay7;
  Two_Two_Product(bdx, bdxtail, cdy, cdytail,
                  bxcy7, bxcy[6], bxcy[5], bxcy[4],
                  bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
  bxcy[7] = bxcy7;
  negate = -bdy;
  negatetail = -bdytail;
  Two_Two_Product(cdx, cdxtail, negate, negatetail,
                  cxby7, cxby[6], cxby[5], cxby[4],
                  cxby[3], cxby[2], cxby[1], cxby[0]);
  cxby[7] = cxby7;
  Two_Two_Product(cdx, cdxtail, ady, adytail,
                  cxay7, cxay[6], cxay[5], cxay[4],
                  cxay[3], cxay[2], cxay[1], cxay[0]);
  cxay[7] = cxay7;
  negate = -cdy;
  negatetail = -cdytail;
  Two_Two_Product(adx, adxtail, negate, negatetail,
                  axcy7, axcy[6], axcy[5], axcy[4],
                  axcy[3], axcy[2], axcy[1], axcy[0]);
  axcy[7] = axcy7;


  temp16len = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, temp16);

  xlen = scale_expansion_zeroelim(temp16len, temp16, adx, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, adx, detxx);
  xtlen = scale_expansion_zeroelim(temp16len, temp16, adxtail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, adx, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, adxtail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

  ylen = scale_expansion_zeroelim(temp16len, temp16, ady, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, ady, detyy);
  ytlen = scale_expansion_zeroelim(temp16len, temp16, adytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, ady, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, adytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

  alen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, adet);


  temp16len = fast_expansion_sum_zeroelim(8, cxay, 8, axcy, temp16);

  xlen = scale_expansion_zeroelim(temp16len, temp16, bdx, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, bdx, detxx);
  xtlen = scale_expansion_zeroelim(temp16len, temp16, bdxtail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, bdx, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, bdxtail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

  ylen = scale_expansion_zeroelim(temp16len, temp16, bdy, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, bdy, detyy);
  ytlen = scale_expansion_zeroelim(temp16len, temp16, bdytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, bdy, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, bdytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

  blen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, bdet);


  temp16len = fast_expansion_sum_zeroelim(8, axby, 8, bxay, temp16);

  xlen = scale_expansion_zeroelim(temp16len, temp16, cdx, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, cdx, detxx);
  xtlen = scale_expansion_zeroelim(temp16len, temp16, cdxtail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, cdx, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, cdxtail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

  ylen = scale_expansion_zeroelim(temp16len, temp16, cdy, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, cdy, detyy);
  ytlen = scale_expansion_zeroelim(temp16len, temp16, cdytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, cdy, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, cdytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

  clen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, cdet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, deter);

  return deter[deterlen - 1];
}

REAL incircleadapt(pa, pb, pc, pd, permanent)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL permanent;
{
  INEXACT REAL adx, bdx, cdx, ady, bdy, cdy;
  REAL det, errbound;

  INEXACT REAL bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
  REAL bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
  REAL bc[4], ca[4], ab[4];
  INEXACT REAL bc3, ca3, ab3;
  REAL axbc[8], axxbc[16], aybc[8], ayybc[16], adet[32];
  int axbclen, axxbclen, aybclen, ayybclen, alen;
  REAL bxca[8], bxxca[16], byca[8], byyca[16], bdet[32];
  int bxcalen, bxxcalen, bycalen, byycalen, blen;
  REAL cxab[8], cxxab[16], cyab[8], cyyab[16], cdet[32];
  int cxablen, cxxablen, cyablen, cyyablen, clen;
  REAL abdet[64];
  int ablen;
  REAL fin1[1152], fin2[1152];
  REAL *finnow, *finother, *finswap;
  int finlength;

  REAL adxtail, bdxtail, cdxtail, adytail, bdytail, cdytail;
  INEXACT REAL adxadx1, adyady1, bdxbdx1, bdybdy1, cdxcdx1, cdycdy1;
  REAL adxadx0, adyady0, bdxbdx0, bdybdy0, cdxcdx0, cdycdy0;
  REAL aa[4], bb[4], cc[4];
  INEXACT REAL aa3, bb3, cc3;
  INEXACT REAL ti1, tj1;
  REAL ti0, tj0;
  REAL u[4], v[4];
  INEXACT REAL u3, v3;
  REAL temp8[8], temp16a[16], temp16b[16], temp16c[16];
  REAL temp32a[32], temp32b[32], temp48[48], temp64[64];
  int temp8len, temp16alen, temp16blen, temp16clen;
  int temp32alen, temp32blen, temp48len, temp64len;
  REAL axtbb[8], axtcc[8], aytbb[8], aytcc[8];
  int axtbblen, axtcclen, aytbblen, aytcclen;
  REAL bxtaa[8], bxtcc[8], bytaa[8], bytcc[8];
  int bxtaalen, bxtcclen, bytaalen, bytcclen;
  REAL cxtaa[8], cxtbb[8], cytaa[8], cytbb[8];
  int cxtaalen, cxtbblen, cytaalen, cytbblen;
  REAL axtbc[8], aytbc[8], bxtca[8], bytca[8], cxtab[8], cytab[8];
  int axtbclen = 0, aytbclen = 0, bxtcalen = 0, bytcalen = 0, cxtablen = 0, cytablen = 0;
  REAL axtbct[16], aytbct[16], bxtcat[16], bytcat[16], cxtabt[16], cytabt[16];
  int axtbctlen, aytbctlen, bxtcatlen, bytcatlen, cxtabtlen, cytabtlen;
  REAL axtbctt[8], aytbctt[8], bxtcatt[8];
  REAL bytcatt[8], cxtabtt[8], cytabtt[8];
  int axtbcttlen, aytbcttlen, bxtcattlen, bytcattlen, cxtabttlen, cytabttlen;
  REAL abt[8], bct[8], cat[8];
  int abtlen, bctlen, catlen;
  REAL abtt[4], bctt[4], catt[4];
  int abttlen, bcttlen, cattlen;
  INEXACT REAL abtt3, bctt3, catt3;
  REAL negate;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  adx = (REAL) (pa[0] - pd[0]);
  bdx = (REAL) (pb[0] - pd[0]);
  cdx = (REAL) (pc[0] - pd[0]);
  ady = (REAL) (pa[1] - pd[1]);
  bdy = (REAL) (pb[1] - pd[1]);
  cdy = (REAL) (pc[1] - pd[1]);

  Two_Product(bdx, cdy, bdxcdy1, bdxcdy0);
  Two_Product(cdx, bdy, cdxbdy1, cdxbdy0);
  Two_Two_Diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
  bc[3] = bc3;
  axbclen = scale_expansion_zeroelim(4, bc, adx, axbc);
  axxbclen = scale_expansion_zeroelim(axbclen, axbc, adx, axxbc);
  aybclen = scale_expansion_zeroelim(4, bc, ady, aybc);
  ayybclen = scale_expansion_zeroelim(aybclen, aybc, ady, ayybc);
  alen = fast_expansion_sum_zeroelim(axxbclen, axxbc, ayybclen, ayybc, adet);

  Two_Product(cdx, ady, cdxady1, cdxady0);
  Two_Product(adx, cdy, adxcdy1, adxcdy0);
  Two_Two_Diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
  ca[3] = ca3;
  bxcalen = scale_expansion_zeroelim(4, ca, bdx, bxca);
  bxxcalen = scale_expansion_zeroelim(bxcalen, bxca, bdx, bxxca);
  bycalen = scale_expansion_zeroelim(4, ca, bdy, byca);
  byycalen = scale_expansion_zeroelim(bycalen, byca, bdy, byyca);
  blen = fast_expansion_sum_zeroelim(bxxcalen, bxxca, byycalen, byyca, bdet);

  Two_Product(adx, bdy, adxbdy1, adxbdy0);
  Two_Product(bdx, ady, bdxady1, bdxady0);
  Two_Two_Diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
  ab[3] = ab3;
  cxablen = scale_expansion_zeroelim(4, ab, cdx, cxab);
  cxxablen = scale_expansion_zeroelim(cxablen, cxab, cdx, cxxab);
  cyablen = scale_expansion_zeroelim(4, ab, cdy, cyab);
  cyyablen = scale_expansion_zeroelim(cyablen, cyab, cdy, cyyab);
  clen = fast_expansion_sum_zeroelim(cxxablen, cxxab, cyyablen, cyyab, cdet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

  det = estimate(finlength, fin1);
  errbound = iccerrboundB * permanent;
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  Two_Diff_Tail(pa[0], pd[0], adx, adxtail);
  Two_Diff_Tail(pa[1], pd[1], ady, adytail);
  Two_Diff_Tail(pb[0], pd[0], bdx, bdxtail);
  Two_Diff_Tail(pb[1], pd[1], bdy, bdytail);
  Two_Diff_Tail(pc[0], pd[0], cdx, cdxtail);
  Two_Diff_Tail(pc[1], pd[1], cdy, cdytail);
  if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
      && (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)) {
    return det;
  }

  errbound = iccerrboundC * permanent + resulterrbound * Absolute(det);
  det += ((adx * adx + ady * ady) * ((bdx * cdytail + cdy * bdxtail)
                                     - (bdy * cdxtail + cdx * bdytail))
          + 2.0 * (adx * adxtail + ady * adytail) * (bdx * cdy - bdy * cdx))
       + ((bdx * bdx + bdy * bdy) * ((cdx * adytail + ady * cdxtail)
                                     - (cdy * adxtail + adx * cdytail))
          + 2.0 * (bdx * bdxtail + bdy * bdytail) * (cdx * ady - cdy * adx))
       + ((cdx * cdx + cdy * cdy) * ((adx * bdytail + bdy * adxtail)
                                     - (ady * bdxtail + bdx * adytail))
          + 2.0 * (cdx * cdxtail + cdy * cdytail) * (adx * bdy - ady * bdx));
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  finnow = fin1;
  finother = fin2;

  if ((bdxtail != 0.0) || (bdytail != 0.0)
      || (cdxtail != 0.0) || (cdytail != 0.0)) {
    Square(adx, adxadx1, adxadx0);
    Square(ady, adyady1, adyady0);
    Two_Two_Sum(adxadx1, adxadx0, adyady1, adyady0, aa3, aa[2], aa[1], aa[0]);
    aa[3] = aa3;
  }
  if ((cdxtail != 0.0) || (cdytail != 0.0)
      || (adxtail != 0.0) || (adytail != 0.0)) {
    Square(bdx, bdxbdx1, bdxbdx0);
    Square(bdy, bdybdy1, bdybdy0);
    Two_Two_Sum(bdxbdx1, bdxbdx0, bdybdy1, bdybdy0, bb3, bb[2], bb[1], bb[0]);
    bb[3] = bb3;
  }
  if ((adxtail != 0.0) || (adytail != 0.0)
      || (bdxtail != 0.0) || (bdytail != 0.0)) {
    Square(cdx, cdxcdx1, cdxcdx0);
    Square(cdy, cdycdy1, cdycdy0);
    Two_Two_Sum(cdxcdx1, cdxcdx0, cdycdy1, cdycdy0, cc3, cc[2], cc[1], cc[0]);
    cc[3] = cc3;
  }

  if (adxtail != 0.0) {
    axtbclen = scale_expansion_zeroelim(4, bc, adxtail, axtbc);
    temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, 2.0 * adx,
                                          temp16a);

    axtcclen = scale_expansion_zeroelim(4, cc, adxtail, axtcc);
    temp16blen = scale_expansion_zeroelim(axtcclen, axtcc, bdy, temp16b);

    axtbblen = scale_expansion_zeroelim(4, bb, adxtail, axtbb);
    temp16clen = scale_expansion_zeroelim(axtbblen, axtbb, -cdy, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (adytail != 0.0) {
    aytbclen = scale_expansion_zeroelim(4, bc, adytail, aytbc);
    temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, 2.0 * ady,
                                          temp16a);

    aytbblen = scale_expansion_zeroelim(4, bb, adytail, aytbb);
    temp16blen = scale_expansion_zeroelim(aytbblen, aytbb, cdx, temp16b);

    aytcclen = scale_expansion_zeroelim(4, cc, adytail, aytcc);
    temp16clen = scale_expansion_zeroelim(aytcclen, aytcc, -bdx, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (bdxtail != 0.0) {
    bxtcalen = scale_expansion_zeroelim(4, ca, bdxtail, bxtca);
    temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, 2.0 * bdx,
                                          temp16a);

    bxtaalen = scale_expansion_zeroelim(4, aa, bdxtail, bxtaa);
    temp16blen = scale_expansion_zeroelim(bxtaalen, bxtaa, cdy, temp16b);

    bxtcclen = scale_expansion_zeroelim(4, cc, bdxtail, bxtcc);
    temp16clen = scale_expansion_zeroelim(bxtcclen, bxtcc, -ady, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (bdytail != 0.0) {
    bytcalen = scale_expansion_zeroelim(4, ca, bdytail, bytca);
    temp16alen = scale_expansion_zeroelim(bytcalen, bytca, 2.0 * bdy,
                                          temp16a);

    bytcclen = scale_expansion_zeroelim(4, cc, bdytail, bytcc);
    temp16blen = scale_expansion_zeroelim(bytcclen, bytcc, adx, temp16b);

    bytaalen = scale_expansion_zeroelim(4, aa, bdytail, bytaa);
    temp16clen = scale_expansion_zeroelim(bytaalen, bytaa, -cdx, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (cdxtail != 0.0) {
    cxtablen = scale_expansion_zeroelim(4, ab, cdxtail, cxtab);
    temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, 2.0 * cdx,
                                          temp16a);

    cxtbblen = scale_expansion_zeroelim(4, bb, cdxtail, cxtbb);
    temp16blen = scale_expansion_zeroelim(cxtbblen, cxtbb, ady, temp16b);

    cxtaalen = scale_expansion_zeroelim(4, aa, cdxtail, cxtaa);
    temp16clen = scale_expansion_zeroelim(cxtaalen, cxtaa, -bdy, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }
  if (cdytail != 0.0) {
    cytablen = scale_expansion_zeroelim(4, ab, cdytail, cytab);
    temp16alen = scale_expansion_zeroelim(cytablen, cytab, 2.0 * cdy,
                                          temp16a);

    cytaalen = scale_expansion_zeroelim(4, aa, cdytail, cytaa);
    temp16blen = scale_expansion_zeroelim(cytaalen, cytaa, bdx, temp16b);

    cytbblen = scale_expansion_zeroelim(4, bb, cdytail, cytbb);
    temp16clen = scale_expansion_zeroelim(cytbblen, cytbb, -adx, temp16c);

    temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                            temp16blen, temp16b, temp32a);
    temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
                                            temp32alen, temp32a, temp48);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                            temp48, finother);
    finswap = finnow; finnow = finother; finother = finswap;
  }

  if ((adxtail != 0.0) || (adytail != 0.0)) {
    if ((bdxtail != 0.0) || (bdytail != 0.0)
        || (cdxtail != 0.0) || (cdytail != 0.0)) {
      Two_Product(bdxtail, cdy, ti1, ti0);
      Two_Product(bdx, cdytail, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
      u[3] = u3;
      negate = -bdy;
      Two_Product(cdxtail, negate, ti1, ti0);
      negate = -bdytail;
      Two_Product(cdx, negate, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
      v[3] = v3;
      bctlen = fast_expansion_sum_zeroelim(4, u, 4, v, bct);

      Two_Product(bdxtail, cdytail, ti1, ti0);
      Two_Product(cdxtail, bdytail, tj1, tj0);
      Two_Two_Diff(ti1, ti0, tj1, tj0, bctt3, bctt[2], bctt[1], bctt[0]);
      bctt[3] = bctt3;
      bcttlen = 4;
    } else {
      bct[0] = 0.0;
      bctlen = 1;
      bctt[0] = 0.0;
      bcttlen = 1;
    }

    if (adxtail != 0.0) {
      temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, adxtail, temp16a);
      axtbctlen = scale_expansion_zeroelim(bctlen, bct, adxtail, axtbct);
      temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, 2.0 * adx,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (bdytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, cc, adxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
      if (cdytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, bb, -adxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }

      temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, adxtail,
                                            temp32a);
      axtbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adxtail, axtbctt);
      temp16alen = scale_expansion_zeroelim(axtbcttlen, axtbctt, 2.0 * adx,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(axtbcttlen, axtbctt, adxtail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
    if (adytail != 0.0) {
      temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, adytail, temp16a);
      aytbctlen = scale_expansion_zeroelim(bctlen, bct, adytail, aytbct);
      temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, 2.0 * ady,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;


      temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, adytail,
                                            temp32a);
      aytbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adytail, aytbctt);
      temp16alen = scale_expansion_zeroelim(aytbcttlen, aytbctt, 2.0 * ady,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(aytbcttlen, aytbctt, adytail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
  }
  if ((bdxtail != 0.0) || (bdytail != 0.0)) {
    if ((cdxtail != 0.0) || (cdytail != 0.0)
        || (adxtail != 0.0) || (adytail != 0.0)) {
      Two_Product(cdxtail, ady, ti1, ti0);
      Two_Product(cdx, adytail, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
      u[3] = u3;
      negate = -cdy;
      Two_Product(adxtail, negate, ti1, ti0);
      negate = -cdytail;
      Two_Product(adx, negate, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
      v[3] = v3;
      catlen = fast_expansion_sum_zeroelim(4, u, 4, v, cat);

      Two_Product(cdxtail, adytail, ti1, ti0);
      Two_Product(adxtail, cdytail, tj1, tj0);
      Two_Two_Diff(ti1, ti0, tj1, tj0, catt3, catt[2], catt[1], catt[0]);
      catt[3] = catt3;
      cattlen = 4;
    } else {
      cat[0] = 0.0;
      catlen = 1;
      catt[0] = 0.0;
      cattlen = 1;
    }

    if (bdxtail != 0.0) {
      temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, bdxtail, temp16a);
      bxtcatlen = scale_expansion_zeroelim(catlen, cat, bdxtail, bxtcat);
      temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, 2.0 * bdx,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (cdytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, aa, bdxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
      if (adytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, cc, -bdxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }

      temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, bdxtail,
                                            temp32a);
      bxtcattlen = scale_expansion_zeroelim(cattlen, catt, bdxtail, bxtcatt);
      temp16alen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, 2.0 * bdx,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, bdxtail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
    if (bdytail != 0.0) {
      temp16alen = scale_expansion_zeroelim(bytcalen, bytca, bdytail, temp16a);
      bytcatlen = scale_expansion_zeroelim(catlen, cat, bdytail, bytcat);
      temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, 2.0 * bdy,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;


      temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, bdytail,
                                            temp32a);
      bytcattlen = scale_expansion_zeroelim(cattlen, catt, bdytail, bytcatt);
      temp16alen = scale_expansion_zeroelim(bytcattlen, bytcatt, 2.0 * bdy,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(bytcattlen, bytcatt, bdytail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
  }
  if ((cdxtail != 0.0) || (cdytail != 0.0)) {
    if ((adxtail != 0.0) || (adytail != 0.0)
        || (bdxtail != 0.0) || (bdytail != 0.0)) {
      Two_Product(adxtail, bdy, ti1, ti0);
      Two_Product(adx, bdytail, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
      u[3] = u3;
      negate = -ady;
      Two_Product(bdxtail, negate, ti1, ti0);
      negate = -adytail;
      Two_Product(bdx, negate, tj1, tj0);
      Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
      v[3] = v3;
      abtlen = fast_expansion_sum_zeroelim(4, u, 4, v, abt);

      Two_Product(adxtail, bdytail, ti1, ti0);
      Two_Product(bdxtail, adytail, tj1, tj0);
      Two_Two_Diff(ti1, ti0, tj1, tj0, abtt3, abtt[2], abtt[1], abtt[0]);
      abtt[3] = abtt3;
      abttlen = 4;
    } else {
      abt[0] = 0.0;
      abtlen = 1;
      abtt[0] = 0.0;
      abttlen = 1;
    }

    if (cdxtail != 0.0) {
      temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, cdxtail, temp16a);
      cxtabtlen = scale_expansion_zeroelim(abtlen, abt, cdxtail, cxtabt);
      temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, 2.0 * cdx,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;
      if (adytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, bb, cdxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }
      if (bdytail != 0.0) {
        temp8len = scale_expansion_zeroelim(4, aa, -cdxtail, temp8);
        temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail,
                                              temp16a);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                                                temp16a, finother);
        finswap = finnow; finnow = finother; finother = finswap;
      }

      temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, cdxtail,
                                            temp32a);
      cxtabttlen = scale_expansion_zeroelim(abttlen, abtt, cdxtail, cxtabtt);
      temp16alen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, 2.0 * cdx,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, cdxtail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
    if (cdytail != 0.0) {
      temp16alen = scale_expansion_zeroelim(cytablen, cytab, cdytail, temp16a);
      cytabtlen = scale_expansion_zeroelim(abtlen, abt, cdytail, cytabt);
      temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, 2.0 * cdy,
                                            temp32a);
      temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp32alen, temp32a, temp48);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                                              temp48, finother);
      finswap = finnow; finnow = finother; finother = finswap;


      temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, cdytail,
                                            temp32a);
      cytabttlen = scale_expansion_zeroelim(abttlen, abtt, cdytail, cytabtt);
      temp16alen = scale_expansion_zeroelim(cytabttlen, cytabtt, 2.0 * cdy,
                                            temp16a);
      temp16blen = scale_expansion_zeroelim(cytabttlen, cytabtt, cdytail,
                                            temp16b);
      temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                                              temp16blen, temp16b, temp32b);
      temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                              temp32blen, temp32b, temp64);
      finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                                              temp64, finother);
      finswap = finnow; finnow = finother; finother = finswap;
    }
  }

  return finnow[finlength - 1];
}

REAL incircle(pa, pb, pc, pd)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
{
  REAL adx, bdx, cdx, ady, bdy, cdy;
  REAL bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
  REAL alift, blift, clift;
  REAL det;
  REAL permanent, errbound;

  adx = pa[0] - pd[0];
  bdx = pb[0] - pd[0];
  cdx = pc[0] - pd[0];
  ady = pa[1] - pd[1];
  bdy = pb[1] - pd[1];
  cdy = pc[1] - pd[1];

  bdxcdy = bdx * cdy;
  cdxbdy = cdx * bdy;
  alift = adx * adx + ady * ady;

  cdxady = cdx * ady;
  adxcdy = adx * cdy;
  blift = bdx * bdx + bdy * bdy;

  adxbdy = adx * bdy;
  bdxady = bdx * ady;
  clift = cdx * cdx + cdy * cdy;

  det = alift * (bdxcdy - cdxbdy)
      + blift * (cdxady - adxcdy)
      + clift * (adxbdy - bdxady);

  permanent = (Absolute(bdxcdy) + Absolute(cdxbdy)) * alift
            + (Absolute(cdxady) + Absolute(adxcdy)) * blift
            + (Absolute(adxbdy) + Absolute(bdxady)) * clift;
  errbound = iccerrboundA * permanent;
  if ((det > errbound) || (-det > errbound)) {
    return det;
  }

  return incircleadapt(pa, pb, pc, pd, permanent);
}

/*****************************************************************************/
/*                                                                           */
/*  inspherefast()   Approximate 3D insphere test.  Nonrobust.               */
/*  insphereexact()   Exact 3D insphere test.  Robust.                       */
/*  insphereslow()   Another exact 3D insphere test.  Robust.                */
/*  insphere()   Adaptive exact 3D insphere test.  Robust.                   */
/*                                                                           */
/*               Return a positive value if the point pe lies inside the     */
/*               sphere passing through pa, pb, pc, and pd; a negative value */
/*               if it lies outside; and zero if the five points are         */
/*               cospherical.  The points pa, pb, pc, and pd must be ordered */
/*               so that they have a positive orientation (as defined by     */
/*               orient3d()), or the sign of the result will be reversed.    */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In insphere() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, insphere() is usually quite */
/*  fast, but will run more slowly when the input points are cospherical or  */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL inspherefast(pa, pb, pc, pd, pe)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL *pe;
{
  REAL aex, bex, cex, dex;
  REAL aey, bey, cey, dey;
  REAL aez, bez, cez, dez;
  REAL alift, blift, clift, dlift;
  REAL ab, bc, cd, da, ac, bd;
  REAL abc, bcd, cda, dab;

  aex = pa[0] - pe[0];
  bex = pb[0] - pe[0];
  cex = pc[0] - pe[0];
  dex = pd[0] - pe[0];
  aey = pa[1] - pe[1];
  bey = pb[1] - pe[1];
  cey = pc[1] - pe[1];
  dey = pd[1] - pe[1];
  aez = pa[2] - pe[2];
  bez = pb[2] - pe[2];
  cez = pc[2] - pe[2];
  dez = pd[2] - pe[2];

  ab = aex * bey - bex * aey;
  bc = bex * cey - cex * bey;
  cd = cex * dey - dex * cey;
  da = dex * aey - aex * dey;

  ac = aex * cey - cex * aey;
  bd = bex * dey - dex * bey;

  abc = aez * bc - bez * ac + cez * ab;
  bcd = bez * cd - cez * bd + dez * bc;
  cda = cez * da + dez * ac + aez * cd;
  dab = dez * ab + aez * bd + bez * da;

  alift = aex * aex + aey * aey + aez * aez;
  blift = bex * bex + bey * bey + bez * bez;
  clift = cex * cex + cey * cey + cez * cez;
  dlift = dex * dex + dey * dey + dez * dez;

  return (dlift * abc - clift * dab) + (blift * cda - alift * bcd);
}

REAL insphereexact(pa, pb, pc, pd, pe)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL *pe;
{
  INEXACT REAL axby1, bxcy1, cxdy1, dxey1, exay1;
  INEXACT REAL bxay1, cxby1, dxcy1, exdy1, axey1;
  INEXACT REAL axcy1, bxdy1, cxey1, dxay1, exby1;
  INEXACT REAL cxay1, dxby1, excy1, axdy1, bxey1;
  REAL axby0, bxcy0, cxdy0, dxey0, exay0;
  REAL bxay0, cxby0, dxcy0, exdy0, axey0;
  REAL axcy0, bxdy0, cxey0, dxay0, exby0;
  REAL cxay0, dxby0, excy0, axdy0, bxey0;
  REAL ab[4], bc[4], cd[4], de[4], ea[4];
  REAL ac[4], bd[4], ce[4], da[4], eb[4];
  REAL temp8a[8], temp8b[8], temp16[16];
  int temp8alen, temp8blen, temp16len;
  REAL abc[24], bcd[24], cde[24], dea[24], eab[24];
  REAL abd[24], bce[24], cda[24], deb[24], eac[24];
  int abclen, bcdlen, cdelen, dealen, eablen;
  int abdlen, bcelen, cdalen, deblen, eaclen;
  REAL temp48a[48], temp48b[48];
  int temp48alen, temp48blen;
  REAL abcd[96], bcde[96], cdea[96], deab[96], eabc[96];
  int abcdlen, bcdelen, cdealen, deablen, eabclen;
  REAL temp192[192];
  REAL det384x[384], det384y[384], det384z[384];
  int xlen, ylen, zlen;
  REAL detxy[768];
  int xylen;
  REAL adet[1152], bdet[1152], cdet[1152], ddet[1152], edet[1152];
  int alen, blen, clen, dlen, elen;
  REAL abdet[2304], cddet[2304], cdedet[3456];
  int ablen, cdlen;
  REAL deter[5760];
  int deterlen;
  int i;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  Two_Product(pa[0], pb[1], axby1, axby0);
  Two_Product(pb[0], pa[1], bxay1, bxay0);
  Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

  Two_Product(pb[0], pc[1], bxcy1, bxcy0);
  Two_Product(pc[0], pb[1], cxby1, cxby0);
  Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

  Two_Product(pc[0], pd[1], cxdy1, cxdy0);
  Two_Product(pd[0], pc[1], dxcy1, dxcy0);
  Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

  Two_Product(pd[0], pe[1], dxey1, dxey0);
  Two_Product(pe[0], pd[1], exdy1, exdy0);
  Two_Two_Diff(dxey1, dxey0, exdy1, exdy0, de[3], de[2], de[1], de[0]);

  Two_Product(pe[0], pa[1], exay1, exay0);
  Two_Product(pa[0], pe[1], axey1, axey0);
  Two_Two_Diff(exay1, exay0, axey1, axey0, ea[3], ea[2], ea[1], ea[0]);

  Two_Product(pa[0], pc[1], axcy1, axcy0);
  Two_Product(pc[0], pa[1], cxay1, cxay0);
  Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

  Two_Product(pb[0], pd[1], bxdy1, bxdy0);
  Two_Product(pd[0], pb[1], dxby1, dxby0);
  Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

  Two_Product(pc[0], pe[1], cxey1, cxey0);
  Two_Product(pe[0], pc[1], excy1, excy0);
  Two_Two_Diff(cxey1, cxey0, excy1, excy0, ce[3], ce[2], ce[1], ce[0]);

  Two_Product(pd[0], pa[1], dxay1, dxay0);
  Two_Product(pa[0], pd[1], axdy1, axdy0);
  Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

  Two_Product(pe[0], pb[1], exby1, exby0);
  Two_Product(pb[0], pe[1], bxey1, bxey0);
  Two_Two_Diff(exby1, exby0, bxey1, bxey0, eb[3], eb[2], eb[1], eb[0]);

  temp8alen = scale_expansion_zeroelim(4, bc, pa[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, ac, -pb[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, ab, pc[2], temp8a);
  abclen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       abc);

  temp8alen = scale_expansion_zeroelim(4, cd, pb[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, bd, -pc[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, bc, pd[2], temp8a);
  bcdlen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       bcd);

  temp8alen = scale_expansion_zeroelim(4, de, pc[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, ce, -pd[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, cd, pe[2], temp8a);
  cdelen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       cde);

  temp8alen = scale_expansion_zeroelim(4, ea, pd[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, da, -pe[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, de, pa[2], temp8a);
  dealen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       dea);

  temp8alen = scale_expansion_zeroelim(4, ab, pe[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, eb, -pa[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, ea, pb[2], temp8a);
  eablen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       eab);

  temp8alen = scale_expansion_zeroelim(4, bd, pa[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, da, pb[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, ab, pd[2], temp8a);
  abdlen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       abd);

  temp8alen = scale_expansion_zeroelim(4, ce, pb[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, eb, pc[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, bc, pe[2], temp8a);
  bcelen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       bce);

  temp8alen = scale_expansion_zeroelim(4, da, pc[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, ac, pd[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, cd, pa[2], temp8a);
  cdalen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       cda);

  temp8alen = scale_expansion_zeroelim(4, eb, pd[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, bd, pe[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, de, pb[2], temp8a);
  deblen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       deb);

  temp8alen = scale_expansion_zeroelim(4, ac, pe[2], temp8a);
  temp8blen = scale_expansion_zeroelim(4, ce, pa[2], temp8b);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
                                          temp16);
  temp8alen = scale_expansion_zeroelim(4, ea, pc[2], temp8a);
  eaclen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
                                       eac);

  temp48alen = fast_expansion_sum_zeroelim(cdelen, cde, bcelen, bce, temp48a);
  temp48blen = fast_expansion_sum_zeroelim(deblen, deb, bcdlen, bcd, temp48b);
  for (i = 0; i < temp48blen; i++) {
    temp48b[i] = -temp48b[i];
  }
  bcdelen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
                                        temp48blen, temp48b, bcde);
  xlen = scale_expansion_zeroelim(bcdelen, bcde, pa[0], temp192);
  xlen = scale_expansion_zeroelim(xlen, temp192, pa[0], det384x);
  ylen = scale_expansion_zeroelim(bcdelen, bcde, pa[1], temp192);
  ylen = scale_expansion_zeroelim(ylen, temp192, pa[1], det384y);
  zlen = scale_expansion_zeroelim(bcdelen, bcde, pa[2], temp192);
  zlen = scale_expansion_zeroelim(zlen, temp192, pa[2], det384z);
  xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
  alen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, adet);

  temp48alen = fast_expansion_sum_zeroelim(dealen, dea, cdalen, cda, temp48a);
  temp48blen = fast_expansion_sum_zeroelim(eaclen, eac, cdelen, cde, temp48b);
  for (i = 0; i < temp48blen; i++) {
    temp48b[i] = -temp48b[i];
  }
  cdealen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
                                        temp48blen, temp48b, cdea);
  xlen = scale_expansion_zeroelim(cdealen, cdea, pb[0], temp192);
  xlen = scale_expansion_zeroelim(xlen, temp192, pb[0], det384x);
  ylen = scale_expansion_zeroelim(cdealen, cdea, pb[1], temp192);
  ylen = scale_expansion_zeroelim(ylen, temp192, pb[1], det384y);
  zlen = scale_expansion_zeroelim(cdealen, cdea, pb[2], temp192);
  zlen = scale_expansion_zeroelim(zlen, temp192, pb[2], det384z);
  xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
  blen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, bdet);

  temp48alen = fast_expansion_sum_zeroelim(eablen, eab, deblen, deb, temp48a);
  temp48blen = fast_expansion_sum_zeroelim(abdlen, abd, dealen, dea, temp48b);
  for (i = 0; i < temp48blen; i++) {
    temp48b[i] = -temp48b[i];
  }
  deablen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
                                        temp48blen, temp48b, deab);
  xlen = scale_expansion_zeroelim(deablen, deab, pc[0], temp192);
  xlen = scale_expansion_zeroelim(xlen, temp192, pc[0], det384x);
  ylen = scale_expansion_zeroelim(deablen, deab, pc[1], temp192);
  ylen = scale_expansion_zeroelim(ylen, temp192, pc[1], det384y);
  zlen = scale_expansion_zeroelim(deablen, deab, pc[2], temp192);
  zlen = scale_expansion_zeroelim(zlen, temp192, pc[2], det384z);
  xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
  clen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, cdet);

  temp48alen = fast_expansion_sum_zeroelim(abclen, abc, eaclen, eac, temp48a);
  temp48blen = fast_expansion_sum_zeroelim(bcelen, bce, eablen, eab, temp48b);
  for (i = 0; i < temp48blen; i++) {
    temp48b[i] = -temp48b[i];
  }
  eabclen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
                                        temp48blen, temp48b, eabc);
  xlen = scale_expansion_zeroelim(eabclen, eabc, pd[0], temp192);
  xlen = scale_expansion_zeroelim(xlen, temp192, pd[0], det384x);
  ylen = scale_expansion_zeroelim(eabclen, eabc, pd[1], temp192);
  ylen = scale_expansion_zeroelim(ylen, temp192, pd[1], det384y);
  zlen = scale_expansion_zeroelim(eabclen, eabc, pd[2], temp192);
  zlen = scale_expansion_zeroelim(zlen, temp192, pd[2], det384z);
  xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
  dlen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, ddet);

  temp48alen = fast_expansion_sum_zeroelim(bcdlen, bcd, abdlen, abd, temp48a);
  temp48blen = fast_expansion_sum_zeroelim(cdalen, cda, abclen, abc, temp48b);
  for (i = 0; i < temp48blen; i++) {
    temp48b[i] = -temp48b[i];
  }
  abcdlen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
                                        temp48blen, temp48b, abcd);
  xlen = scale_expansion_zeroelim(abcdlen, abcd, pe[0], temp192);
  xlen = scale_expansion_zeroelim(xlen, temp192, pe[0], det384x);
  ylen = scale_expansion_zeroelim(abcdlen, abcd, pe[1], temp192);
  ylen = scale_expansion_zeroelim(ylen, temp192, pe[1], det384y);
  zlen = scale_expansion_zeroelim(abcdlen, abcd, pe[2], temp192);
  zlen = scale_expansion_zeroelim(zlen, temp192, pe[2], det384z);
  xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
  elen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, edet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
  cdelen = fast_expansion_sum_zeroelim(cdlen, cddet, elen, edet, cdedet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdelen, cdedet, deter);

  return deter[deterlen - 1];
}

REAL insphereslow(pa, pb, pc, pd, pe)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL *pe;
{
  INEXACT REAL aex, bex, cex, dex, aey, bey, cey, dey, aez, bez, cez, dez;
  REAL aextail, bextail, cextail, dextail;
  REAL aeytail, beytail, ceytail, deytail;
  REAL aeztail, beztail, ceztail, deztail;
  REAL negate, negatetail;
  INEXACT REAL axby7, bxcy7, cxdy7, dxay7, axcy7, bxdy7;
  INEXACT REAL bxay7, cxby7, dxcy7, axdy7, cxay7, dxby7;
  REAL axby[8], bxcy[8], cxdy[8], dxay[8], axcy[8], bxdy[8];
  REAL bxay[8], cxby[8], dxcy[8], axdy[8], cxay[8], dxby[8];
  REAL ab[16], bc[16], cd[16], da[16], ac[16], bd[16];
  int ablen, bclen, cdlen, dalen, aclen, bdlen;
  REAL temp32a[32], temp32b[32], temp64a[64], temp64b[64], temp64c[64];
  int temp32alen, temp32blen, temp64alen, temp64blen, temp64clen;
  REAL temp128[128], temp192[192];
  int temp128len, temp192len;
  REAL detx[384], detxx[768], detxt[384], detxxt[768], detxtxt[768];
  int xlen, xxlen, xtlen, xxtlen, xtxtlen;
  REAL x1[1536], x2[2304];
  int x1len, x2len;
  REAL dety[384], detyy[768], detyt[384], detyyt[768], detytyt[768];
  int ylen, yylen, ytlen, yytlen, ytytlen;
  REAL y1[1536], y2[2304];
  int y1len, y2len;
  REAL detz[384], detzz[768], detzt[384], detzzt[768], detztzt[768];
  int zlen, zzlen, ztlen, zztlen, ztztlen;
  REAL z1[1536], z2[2304];
  int z1len, z2len;
  REAL detxy[4608];
  int xylen;
  REAL adet[6912], bdet[6912], cdet[6912], ddet[6912];
  int alen, blen, clen, dlen;
  REAL abdet[13824], cddet[13824], deter[27648];
  int deterlen;
  int i;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j, _k, _l, _m, _n;
  REAL _0, _1, _2;

  Two_Diff(pa[0], pe[0], aex, aextail);
  Two_Diff(pa[1], pe[1], aey, aeytail);
  Two_Diff(pa[2], pe[2], aez, aeztail);
  Two_Diff(pb[0], pe[0], bex, bextail);
  Two_Diff(pb[1], pe[1], bey, beytail);
  Two_Diff(pb[2], pe[2], bez, beztail);
  Two_Diff(pc[0], pe[0], cex, cextail);
  Two_Diff(pc[1], pe[1], cey, ceytail);
  Two_Diff(pc[2], pe[2], cez, ceztail);
  Two_Diff(pd[0], pe[0], dex, dextail);
  Two_Diff(pd[1], pe[1], dey, deytail);
  Two_Diff(pd[2], pe[2], dez, deztail);

  Two_Two_Product(aex, aextail, bey, beytail,
                  axby7, axby[6], axby[5], axby[4],
                  axby[3], axby[2], axby[1], axby[0]);
  axby[7] = axby7;
  negate = -aey;
  negatetail = -aeytail;
  Two_Two_Product(bex, bextail, negate, negatetail,
                  bxay7, bxay[6], bxay[5], bxay[4],
                  bxay[3], bxay[2], bxay[1], bxay[0]);
  bxay[7] = bxay7;
  ablen = fast_expansion_sum_zeroelim(8, axby, 8, bxay, ab);
  Two_Two_Product(bex, bextail, cey, ceytail,
                  bxcy7, bxcy[6], bxcy[5], bxcy[4],
                  bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
  bxcy[7] = bxcy7;
  negate = -bey;
  negatetail = -beytail;
  Two_Two_Product(cex, cextail, negate, negatetail,
                  cxby7, cxby[6], cxby[5], cxby[4],
                  cxby[3], cxby[2], cxby[1], cxby[0]);
  cxby[7] = cxby7;
  bclen = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, bc);
  Two_Two_Product(cex, cextail, dey, deytail,
                  cxdy7, cxdy[6], cxdy[5], cxdy[4],
                  cxdy[3], cxdy[2], cxdy[1], cxdy[0]);
  cxdy[7] = cxdy7;
  negate = -cey;
  negatetail = -ceytail;
  Two_Two_Product(dex, dextail, negate, negatetail,
                  dxcy7, dxcy[6], dxcy[5], dxcy[4],
                  dxcy[3], dxcy[2], dxcy[1], dxcy[0]);
  dxcy[7] = dxcy7;
  cdlen = fast_expansion_sum_zeroelim(8, cxdy, 8, dxcy, cd);
  Two_Two_Product(dex, dextail, aey, aeytail,
                  dxay7, dxay[6], dxay[5], dxay[4],
                  dxay[3], dxay[2], dxay[1], dxay[0]);
  dxay[7] = dxay7;
  negate = -dey;
  negatetail = -deytail;
  Two_Two_Product(aex, aextail, negate, negatetail,
                  axdy7, axdy[6], axdy[5], axdy[4],
                  axdy[3], axdy[2], axdy[1], axdy[0]);
  axdy[7] = axdy7;
  dalen = fast_expansion_sum_zeroelim(8, dxay, 8, axdy, da);
  Two_Two_Product(aex, aextail, cey, ceytail,
                  axcy7, axcy[6], axcy[5], axcy[4],
                  axcy[3], axcy[2], axcy[1], axcy[0]);
  axcy[7] = axcy7;
  negate = -aey;
  negatetail = -aeytail;
  Two_Two_Product(cex, cextail, negate, negatetail,
                  cxay7, cxay[6], cxay[5], cxay[4],
                  cxay[3], cxay[2], cxay[1], cxay[0]);
  cxay[7] = cxay7;
  aclen = fast_expansion_sum_zeroelim(8, axcy, 8, cxay, ac);
  Two_Two_Product(bex, bextail, dey, deytail,
                  bxdy7, bxdy[6], bxdy[5], bxdy[4],
                  bxdy[3], bxdy[2], bxdy[1], bxdy[0]);
  bxdy[7] = bxdy7;
  negate = -bey;
  negatetail = -beytail;
  Two_Two_Product(dex, dextail, negate, negatetail,
                  dxby7, dxby[6], dxby[5], dxby[4],
                  dxby[3], dxby[2], dxby[1], dxby[0]);
  dxby[7] = dxby7;
  bdlen = fast_expansion_sum_zeroelim(8, bxdy, 8, dxby, bd);

  temp32alen = scale_expansion_zeroelim(cdlen, cd, -bez, temp32a);
  temp32blen = scale_expansion_zeroelim(cdlen, cd, -beztail, temp32b);
  temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64a);
  temp32alen = scale_expansion_zeroelim(bdlen, bd, cez, temp32a);
  temp32blen = scale_expansion_zeroelim(bdlen, bd, ceztail, temp32b);
  temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64b);
  temp32alen = scale_expansion_zeroelim(bclen, bc, -dez, temp32a);
  temp32blen = scale_expansion_zeroelim(bclen, bc, -deztail, temp32b);
  temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64c);
  temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
                                           temp64blen, temp64b, temp128);
  temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
                                           temp128len, temp128, temp192);
  xlen = scale_expansion_zeroelim(temp192len, temp192, aex, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, aex, detxx);
  xtlen = scale_expansion_zeroelim(temp192len, temp192, aextail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, aex, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, aextail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
  ylen = scale_expansion_zeroelim(temp192len, temp192, aey, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, aey, detyy);
  ytlen = scale_expansion_zeroelim(temp192len, temp192, aeytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, aey, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, aeytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
  zlen = scale_expansion_zeroelim(temp192len, temp192, aez, detz);
  zzlen = scale_expansion_zeroelim(zlen, detz, aez, detzz);
  ztlen = scale_expansion_zeroelim(temp192len, temp192, aeztail, detzt);
  zztlen = scale_expansion_zeroelim(ztlen, detzt, aez, detzzt);
  for (i = 0; i < zztlen; i++) {
    detzzt[i] *= 2.0;
  }
  ztztlen = scale_expansion_zeroelim(ztlen, detzt, aeztail, detztzt);
  z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
  z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
  xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
  alen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, adet);

  temp32alen = scale_expansion_zeroelim(dalen, da, cez, temp32a);
  temp32blen = scale_expansion_zeroelim(dalen, da, ceztail, temp32b);
  temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64a);
  temp32alen = scale_expansion_zeroelim(aclen, ac, dez, temp32a);
  temp32blen = scale_expansion_zeroelim(aclen, ac, deztail, temp32b);
  temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64b);
  temp32alen = scale_expansion_zeroelim(cdlen, cd, aez, temp32a);
  temp32blen = scale_expansion_zeroelim(cdlen, cd, aeztail, temp32b);
  temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64c);
  temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
                                           temp64blen, temp64b, temp128);
  temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
                                           temp128len, temp128, temp192);
  xlen = scale_expansion_zeroelim(temp192len, temp192, bex, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, bex, detxx);
  xtlen = scale_expansion_zeroelim(temp192len, temp192, bextail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, bex, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, bextail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
  ylen = scale_expansion_zeroelim(temp192len, temp192, bey, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, bey, detyy);
  ytlen = scale_expansion_zeroelim(temp192len, temp192, beytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, bey, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, beytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
  zlen = scale_expansion_zeroelim(temp192len, temp192, bez, detz);
  zzlen = scale_expansion_zeroelim(zlen, detz, bez, detzz);
  ztlen = scale_expansion_zeroelim(temp192len, temp192, beztail, detzt);
  zztlen = scale_expansion_zeroelim(ztlen, detzt, bez, detzzt);
  for (i = 0; i < zztlen; i++) {
    detzzt[i] *= 2.0;
  }
  ztztlen = scale_expansion_zeroelim(ztlen, detzt, beztail, detztzt);
  z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
  z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
  xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
  blen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, bdet);

  temp32alen = scale_expansion_zeroelim(ablen, ab, -dez, temp32a);
  temp32blen = scale_expansion_zeroelim(ablen, ab, -deztail, temp32b);
  temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64a);
  temp32alen = scale_expansion_zeroelim(bdlen, bd, -aez, temp32a);
  temp32blen = scale_expansion_zeroelim(bdlen, bd, -aeztail, temp32b);
  temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64b);
  temp32alen = scale_expansion_zeroelim(dalen, da, -bez, temp32a);
  temp32blen = scale_expansion_zeroelim(dalen, da, -beztail, temp32b);
  temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64c);
  temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
                                           temp64blen, temp64b, temp128);
  temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
                                           temp128len, temp128, temp192);
  xlen = scale_expansion_zeroelim(temp192len, temp192, cex, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, cex, detxx);
  xtlen = scale_expansion_zeroelim(temp192len, temp192, cextail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, cex, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, cextail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
  ylen = scale_expansion_zeroelim(temp192len, temp192, cey, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, cey, detyy);
  ytlen = scale_expansion_zeroelim(temp192len, temp192, ceytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, cey, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, ceytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
  zlen = scale_expansion_zeroelim(temp192len, temp192, cez, detz);
  zzlen = scale_expansion_zeroelim(zlen, detz, cez, detzz);
  ztlen = scale_expansion_zeroelim(temp192len, temp192, ceztail, detzt);
  zztlen = scale_expansion_zeroelim(ztlen, detzt, cez, detzzt);
  for (i = 0; i < zztlen; i++) {
    detzzt[i] *= 2.0;
  }
  ztztlen = scale_expansion_zeroelim(ztlen, detzt, ceztail, detztzt);
  z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
  z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
  xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
  clen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, cdet);

  temp32alen = scale_expansion_zeroelim(bclen, bc, aez, temp32a);
  temp32blen = scale_expansion_zeroelim(bclen, bc, aeztail, temp32b);
  temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64a);
  temp32alen = scale_expansion_zeroelim(aclen, ac, -bez, temp32a);
  temp32blen = scale_expansion_zeroelim(aclen, ac, -beztail, temp32b);
  temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64b);
  temp32alen = scale_expansion_zeroelim(ablen, ab, cez, temp32a);
  temp32blen = scale_expansion_zeroelim(ablen, ab, ceztail, temp32b);
  temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                                           temp32blen, temp32b, temp64c);
  temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
                                           temp64blen, temp64b, temp128);
  temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
                                           temp128len, temp128, temp192);
  xlen = scale_expansion_zeroelim(temp192len, temp192, dex, detx);
  xxlen = scale_expansion_zeroelim(xlen, detx, dex, detxx);
  xtlen = scale_expansion_zeroelim(temp192len, temp192, dextail, detxt);
  xxtlen = scale_expansion_zeroelim(xtlen, detxt, dex, detxxt);
  for (i = 0; i < xxtlen; i++) {
    detxxt[i] *= 2.0;
  }
  xtxtlen = scale_expansion_zeroelim(xtlen, detxt, dextail, detxtxt);
  x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
  x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
  ylen = scale_expansion_zeroelim(temp192len, temp192, dey, dety);
  yylen = scale_expansion_zeroelim(ylen, dety, dey, detyy);
  ytlen = scale_expansion_zeroelim(temp192len, temp192, deytail, detyt);
  yytlen = scale_expansion_zeroelim(ytlen, detyt, dey, detyyt);
  for (i = 0; i < yytlen; i++) {
    detyyt[i] *= 2.0;
  }
  ytytlen = scale_expansion_zeroelim(ytlen, detyt, deytail, detytyt);
  y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
  y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
  zlen = scale_expansion_zeroelim(temp192len, temp192, dez, detz);
  zzlen = scale_expansion_zeroelim(zlen, detz, dez, detzz);
  ztlen = scale_expansion_zeroelim(temp192len, temp192, deztail, detzt);
  zztlen = scale_expansion_zeroelim(ztlen, detzt, dez, detzzt);
  for (i = 0; i < zztlen; i++) {
    detzzt[i] *= 2.0;
  }
  ztztlen = scale_expansion_zeroelim(ztlen, detzt, deztail, detztzt);
  z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
  z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
  xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
  dlen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, ddet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
  deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

  return deter[deterlen - 1];
}

REAL insphereadapt(pa, pb, pc, pd, pe, permanent)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL *pe;
REAL permanent;
{
  INEXACT REAL aex, bex, cex, dex, aey, bey, cey, dey, aez, bez, cez, dez;
  REAL det, errbound;

  INEXACT REAL aexbey1, bexaey1, bexcey1, cexbey1;
  INEXACT REAL cexdey1, dexcey1, dexaey1, aexdey1;
  INEXACT REAL aexcey1, cexaey1, bexdey1, dexbey1;
  REAL aexbey0, bexaey0, bexcey0, cexbey0;
  REAL cexdey0, dexcey0, dexaey0, aexdey0;
  REAL aexcey0, cexaey0, bexdey0, dexbey0;
  REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
  INEXACT REAL ab3, bc3, cd3, da3, ac3, bd3;
  REAL abeps, bceps, cdeps, daeps, aceps, bdeps;
  REAL temp8a[8], temp8b[8], temp8c[8], temp16[16], temp24[24], temp48[48];
  int temp8alen, temp8blen, temp8clen, temp16len, temp24len, temp48len;
  REAL xdet[96], ydet[96], zdet[96], xydet[192];
  int xlen, ylen, zlen, xylen;
  REAL adet[288], bdet[288], cdet[288], ddet[288];
  int alen, blen, clen, dlen;
  REAL abdet[576], cddet[576];
  int ablen, cdlen;
  REAL fin1[1152];
  int finlength;

  REAL aextail, bextail, cextail, dextail;
  REAL aeytail, beytail, ceytail, deytail;
  REAL aeztail, beztail, ceztail, deztail;

  INEXACT REAL bvirt;
  REAL avirt, bround, around;
  INEXACT REAL c;
  INEXACT REAL abig;
  REAL ahi, alo, bhi, blo;
  REAL err1, err2, err3;
  INEXACT REAL _i, _j;
  REAL _0;

  aex = (REAL) (pa[0] - pe[0]);
  bex = (REAL) (pb[0] - pe[0]);
  cex = (REAL) (pc[0] - pe[0]);
  dex = (REAL) (pd[0] - pe[0]);
  aey = (REAL) (pa[1] - pe[1]);
  bey = (REAL) (pb[1] - pe[1]);
  cey = (REAL) (pc[1] - pe[1]);
  dey = (REAL) (pd[1] - pe[1]);
  aez = (REAL) (pa[2] - pe[2]);
  bez = (REAL) (pb[2] - pe[2]);
  cez = (REAL) (pc[2] - pe[2]);
  dez = (REAL) (pd[2] - pe[2]);

  Two_Product(aex, bey, aexbey1, aexbey0);
  Two_Product(bex, aey, bexaey1, bexaey0);
  Two_Two_Diff(aexbey1, aexbey0, bexaey1, bexaey0, ab3, ab[2], ab[1], ab[0]);
  ab[3] = ab3;

  Two_Product(bex, cey, bexcey1, bexcey0);
  Two_Product(cex, bey, cexbey1, cexbey0);
  Two_Two_Diff(bexcey1, bexcey0, cexbey1, cexbey0, bc3, bc[2], bc[1], bc[0]);
  bc[3] = bc3;

  Two_Product(cex, dey, cexdey1, cexdey0);
  Two_Product(dex, cey, dexcey1, dexcey0);
  Two_Two_Diff(cexdey1, cexdey0, dexcey1, dexcey0, cd3, cd[2], cd[1], cd[0]);
  cd[3] = cd3;

  Two_Product(dex, aey, dexaey1, dexaey0);
  Two_Product(aex, dey, aexdey1, aexdey0);
  Two_Two_Diff(dexaey1, dexaey0, aexdey1, aexdey0, da3, da[2], da[1], da[0]);
  da[3] = da3;

  Two_Product(aex, cey, aexcey1, aexcey0);
  Two_Product(cex, aey, cexaey1, cexaey0);
  Two_Two_Diff(aexcey1, aexcey0, cexaey1, cexaey0, ac3, ac[2], ac[1], ac[0]);
  ac[3] = ac3;

  Two_Product(bex, dey, bexdey1, bexdey0);
  Two_Product(dex, bey, dexbey1, dexbey0);
  Two_Two_Diff(bexdey1, bexdey0, dexbey1, dexbey0, bd3, bd[2], bd[1], bd[0]);
  bd[3] = bd3;

  temp8alen = scale_expansion_zeroelim(4, cd, bez, temp8a);
  temp8blen = scale_expansion_zeroelim(4, bd, -cez, temp8b);
  temp8clen = scale_expansion_zeroelim(4, bc, dez, temp8c);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
                                          temp8blen, temp8b, temp16);
  temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
                                          temp16len, temp16, temp24);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, aex, temp48);
  xlen = scale_expansion_zeroelim(temp48len, temp48, -aex, xdet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, aey, temp48);
  ylen = scale_expansion_zeroelim(temp48len, temp48, -aey, ydet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, aez, temp48);
  zlen = scale_expansion_zeroelim(temp48len, temp48, -aez, zdet);
  xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
  alen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, adet);

  temp8alen = scale_expansion_zeroelim(4, da, cez, temp8a);
  temp8blen = scale_expansion_zeroelim(4, ac, dez, temp8b);
  temp8clen = scale_expansion_zeroelim(4, cd, aez, temp8c);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
                                          temp8blen, temp8b, temp16);
  temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
                                          temp16len, temp16, temp24);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, bex, temp48);
  xlen = scale_expansion_zeroelim(temp48len, temp48, bex, xdet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, bey, temp48);
  ylen = scale_expansion_zeroelim(temp48len, temp48, bey, ydet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, bez, temp48);
  zlen = scale_expansion_zeroelim(temp48len, temp48, bez, zdet);
  xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
  blen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, bdet);

  temp8alen = scale_expansion_zeroelim(4, ab, dez, temp8a);
  temp8blen = scale_expansion_zeroelim(4, bd, aez, temp8b);
  temp8clen = scale_expansion_zeroelim(4, da, bez, temp8c);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
                                          temp8blen, temp8b, temp16);
  temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
                                          temp16len, temp16, temp24);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, cex, temp48);
  xlen = scale_expansion_zeroelim(temp48len, temp48, -cex, xdet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, cey, temp48);
  ylen = scale_expansion_zeroelim(temp48len, temp48, -cey, ydet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, cez, temp48);
  zlen = scale_expansion_zeroelim(temp48len, temp48, -cez, zdet);
  xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
  clen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, cdet);

  temp8alen = scale_expansion_zeroelim(4, bc, aez, temp8a);
  temp8blen = scale_expansion_zeroelim(4, ac, -bez, temp8b);
  temp8clen = scale_expansion_zeroelim(4, ab, cez, temp8c);
  temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
                                          temp8blen, temp8b, temp16);
  temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
                                          temp16len, temp16, temp24);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, dex, temp48);
  xlen = scale_expansion_zeroelim(temp48len, temp48, dex, xdet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, dey, temp48);
  ylen = scale_expansion_zeroelim(temp48len, temp48, dey, ydet);
  temp48len = scale_expansion_zeroelim(temp24len, temp24, dez, temp48);
  zlen = scale_expansion_zeroelim(temp48len, temp48, dez, zdet);
  xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
  dlen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, ddet);

  ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
  cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
  finlength = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, fin1);

  det = estimate(finlength, fin1);
  errbound = isperrboundB * permanent;
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  Two_Diff_Tail(pa[0], pe[0], aex, aextail);
  Two_Diff_Tail(pa[1], pe[1], aey, aeytail);
  Two_Diff_Tail(pa[2], pe[2], aez, aeztail);
  Two_Diff_Tail(pb[0], pe[0], bex, bextail);
  Two_Diff_Tail(pb[1], pe[1], bey, beytail);
  Two_Diff_Tail(pb[2], pe[2], bez, beztail);
  Two_Diff_Tail(pc[0], pe[0], cex, cextail);
  Two_Diff_Tail(pc[1], pe[1], cey, ceytail);
  Two_Diff_Tail(pc[2], pe[2], cez, ceztail);
  Two_Diff_Tail(pd[0], pe[0], dex, dextail);
  Two_Diff_Tail(pd[1], pe[1], dey, deytail);
  Two_Diff_Tail(pd[2], pe[2], dez, deztail);
  if ((aextail == 0.0) && (aeytail == 0.0) && (aeztail == 0.0)
      && (bextail == 0.0) && (beytail == 0.0) && (beztail == 0.0)
      && (cextail == 0.0) && (ceytail == 0.0) && (ceztail == 0.0)
      && (dextail == 0.0) && (deytail == 0.0) && (deztail == 0.0)) {
    return det;
  }

  errbound = isperrboundC * permanent + resulterrbound * Absolute(det);
  abeps = (aex * beytail + bey * aextail)
        - (aey * bextail + bex * aeytail);
  bceps = (bex * ceytail + cey * bextail)
        - (bey * cextail + cex * beytail);
  cdeps = (cex * deytail + dey * cextail)
        - (cey * dextail + dex * ceytail);
  daeps = (dex * aeytail + aey * dextail)
        - (dey * aextail + aex * deytail);
  aceps = (aex * ceytail + cey * aextail)
        - (aey * cextail + cex * aeytail);
  bdeps = (bex * deytail + dey * bextail)
        - (bey * dextail + dex * beytail);
  det += (((bex * bex + bey * bey + bez * bez)
           * ((cez * daeps + dez * aceps + aez * cdeps)
              + (ceztail * da3 + deztail * ac3 + aeztail * cd3))
           + (dex * dex + dey * dey + dez * dez)
           * ((aez * bceps - bez * aceps + cez * abeps)
              + (aeztail * bc3 - beztail * ac3 + ceztail * ab3)))
          - ((aex * aex + aey * aey + aez * aez)
           * ((bez * cdeps - cez * bdeps + dez * bceps)
              + (beztail * cd3 - ceztail * bd3 + deztail * bc3))
           + (cex * cex + cey * cey + cez * cez)
           * ((dez * abeps + aez * bdeps + bez * daeps)
              + (deztail * ab3 + aeztail * bd3 + beztail * da3))))
       + 2.0 * (((bex * bextail + bey * beytail + bez * beztail)
                 * (cez * da3 + dez * ac3 + aez * cd3)
                 + (dex * dextail + dey * deytail + dez * deztail)
                 * (aez * bc3 - bez * ac3 + cez * ab3))
                - ((aex * aextail + aey * aeytail + aez * aeztail)
                 * (bez * cd3 - cez * bd3 + dez * bc3)
                 + (cex * cextail + cey * ceytail + cez * ceztail)
                 * (dez * ab3 + aez * bd3 + bez * da3)));
  if ((det >= errbound) || (-det >= errbound)) {
    return det;
  }

  return insphereexact(pa, pb, pc, pd, pe);
}

REAL insphere(pa, pb, pc, pd, pe)
REAL *pa;
REAL *pb;
REAL *pc;
REAL *pd;
REAL *pe;
{
  REAL aex, bex, cex, dex;
  REAL aey, bey, cey, dey;
  REAL aez, bez, cez, dez;
  REAL aexbey, bexaey, bexcey, cexbey, cexdey, dexcey, dexaey, aexdey;
  REAL aexcey, cexaey, bexdey, dexbey;
  REAL alift, blift, clift, dlift;
  REAL ab, bc, cd, da, ac, bd;
  REAL abc, bcd, cda, dab;
  REAL aezplus, bezplus, cezplus, dezplus;
  REAL aexbeyplus, bexaeyplus, bexceyplus, cexbeyplus;
  REAL cexdeyplus, dexceyplus, dexaeyplus, aexdeyplus;
  REAL aexceyplus, cexaeyplus, bexdeyplus, dexbeyplus;
  REAL det;
  REAL permanent, errbound;

  aex = pa[0] - pe[0];
  bex = pb[0] - pe[0];
  cex = pc[0] - pe[0];
  dex = pd[0] - pe[0];
  aey = pa[1] - pe[1];
  bey = pb[1] - pe[1];
  cey = pc[1] - pe[1];
  dey = pd[1] - pe[1];
  aez = pa[2] - pe[2];
  bez = pb[2] - pe[2];
  cez = pc[2] - pe[2];
  dez = pd[2] - pe[2];

  aexbey = aex * bey;
  bexaey = bex * aey;
  ab = aexbey - bexaey;
  bexcey = bex * cey;
  cexbey = cex * bey;
  bc = bexcey - cexbey;
  cexdey = cex * dey;
  dexcey = dex * cey;
  cd = cexdey - dexcey;
  dexaey = dex * aey;
  aexdey = aex * dey;
  da = dexaey - aexdey;

  aexcey = aex * cey;
  cexaey = cex * aey;
  ac = aexcey - cexaey;
  bexdey = bex * dey;
  dexbey = dex * bey;
  bd = bexdey - dexbey;

  abc = aez * bc - bez * ac + cez * ab;
  bcd = bez * cd - cez * bd + dez * bc;
  cda = cez * da + dez * ac + aez * cd;
  dab = dez * ab + aez * bd + bez * da;

  alift = aex * aex + aey * aey + aez * aez;
  blift = bex * bex + bey * bey + bez * bez;
  clift = cex * cex + cey * cey + cez * cez;
  dlift = dex * dex + dey * dey + dez * dez;

  det = (dlift * abc - clift * dab) + (blift * cda - alift * bcd);

  aezplus = Absolute(aez);
  bezplus = Absolute(bez);
  cezplus = Absolute(cez);
  dezplus = Absolute(dez);
  aexbeyplus = Absolute(aexbey);
  bexaeyplus = Absolute(bexaey);
  bexceyplus = Absolute(bexcey);
  cexbeyplus = Absolute(cexbey);
  cexdeyplus = Absolute(cexdey);
  dexceyplus = Absolute(dexcey);
  dexaeyplus = Absolute(dexaey);
  aexdeyplus = Absolute(aexdey);
  aexceyplus = Absolute(aexcey);
  cexaeyplus = Absolute(cexaey);
  bexdeyplus = Absolute(bexdey);
  dexbeyplus = Absolute(dexbey);
  permanent = ((cexdeyplus + dexceyplus) * bezplus
               + (dexbeyplus + bexdeyplus) * cezplus
               + (bexceyplus + cexbeyplus) * dezplus)
            * alift
            + ((dexaeyplus + aexdeyplus) * cezplus
               + (aexceyplus + cexaeyplus) * dezplus
               + (cexdeyplus + dexceyplus) * aezplus)
            * blift
            + ((aexbeyplus + bexaeyplus) * dezplus
               + (bexdeyplus + dexbeyplus) * aezplus
               + (dexaeyplus + aexdeyplus) * bezplus)
            * clift
            + ((bexceyplus + cexbeyplus) * aezplus
               + (cexaeyplus + aexceyplus) * bezplus
               + (aexbeyplus + bexaeyplus) * cezplus)
            * dlift;
  errbound = isperrboundA * permanent;
  if ((det > errbound) || (-det > errbound)) {
    return det;
  }

  return insphereadapt(pa, pb, pc, pd, pe, permanent);
}
#else

/*****************************************************************************/
/*                                                                           */
/*  Routines for Arbitrary Precision Floating-point Arithmetic               */
/*  and Fast Robust Geometric Predicates                                     */
/*  (predicates.c)                                                           */
/*                                                                           */
/*  May 18, 1996                                                             */
/*                                                                           */
/*  Placed in the public domain by                                           */
/*  Jonathan Richard Shewchuk                                                */
/*  School of Computer Science                                               */
/*  Carnegie Mellon University                                               */
/*  5000 Forbes Avenue                                                       */
/*  Pittsburgh, Pennsylvania  15213-3891                                     */
/*  jrs@cs.cmu.edu                                                           */
/*                                                                           */
/*  This file contains C implementation of algorithms for exact addition     */
/*    and multiplication of floating-point numbers, and predicates for       */
/*    robustly performing the orientation and incircle tests used in         */
/*    computational geometry.  The algorithms and underlying theory are      */
/*    described in Jonathan Richard Shewchuk.  "Adaptive Precision Floating- */
/*    Point Arithmetic and Fast Robust Geometric Predicates."  Technical     */
/*    Report CMU-CS-96-140, School of Computer Science, Carnegie Mellon      */
/*    University, Pittsburgh, Pennsylvania, May 1996.  (Submitted to         */
/*    Discrete & Computational Geometry.)                                    */
/*                                                                           */
/*  This file, the paper listed above, and other information are available   */
/*    from the Web page http://www.cs.cmu.edu/~quake/robust.html .           */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/*                                                                           */
/*  Using this code:                                                         */
/*                                                                           */
/*  First, read the short or long version of the paper (from the Web page    */
/*    above).                                                                */
/*                                                                           */
/*  Be sure to call exactinit() once, before calling any of the arithmetic   */
/*    functions or geometric predicates.  Also be sure to turn on the        */
/*    optimizer when compiling this file.                                    */
/*                                                                           */
/*                                                                           */
/*  Several geometric predicates are defined.  Their parameters are all      */
/*    points.  Each point is an array of two or three floating-point         */
/*    numbers.  The geometric predicates, described in the papers, are       */
/*                                                                           */
/*    orient2d(pa, pb, pc)                                                   */
/*    orient2dfast(pa, pb, pc)                                               */
/*    orient3d(pa, pb, pc, pd)                                               */
/*    orient3dfast(pa, pb, pc, pd)                                           */
/*    incircle(pa, pb, pc, pd)                                               */
/*    incirclefast(pa, pb, pc, pd)                                           */
/*    insphere(pa, pb, pc, pd, pe)                                           */
/*    inspherefast(pa, pb, pc, pd, pe)                                       */
/*                                                                           */
/*  Those with suffix "fast" are approximate, non-robust versions.  Those    */
/*    without the suffix are adaptive precision, robust versions.  There     */
/*    are also versions with the suffices "exact" and "slow", which are      */
/*    non-adaptive, exact arithmetic versions, which I use only for timings  */
/*    in my arithmetic papers.                                               */
/*                                                                           */
/*                                                                           */
/*  An expansion is represented by an array of floating-point numbers,       */
/*    sorted from smallest to largest magnitude (possibly with interspersed  */
/*    zeros).  The length of each expansion is stored as a separate integer, */
/*    and each arithmetic function returns an integer which is the length    */
/*    of the expansion it created.                                           */
/*                                                                           */
/*  Several arithmetic functions are defined.  Their parameters are          */
/*                                                                           */
/*    e, f           Input expansions                                        */
/*    elen, flen     Lengths of input expansions (must be >= 1)              */
/*    h              Output expansion                                        */
/*    b              Input scalar                                            */
/*                                                                           */
/*  The arithmetic functions are                                             */
/*                                                                           */
/*    grow_expansion(elen, e, b, h)                                          */
/*    grow_expansion_zeroelim(elen, e, b, h)                                 */
/*    expansion_sum(elen, e, flen, f, h)                                     */
/*    expansion_sum_zeroelim1(elen, e, flen, f, h)                           */
/*    expansion_sum_zeroelim2(elen, e, flen, f, h)                           */
/*    fast_expansion_sum(elen, e, flen, f, h)                                */
/*    fast_expansion_sum_zeroelim(elen, e, flen, f, h)                       */
/*    linear_expansion_sum(elen, e, flen, f, h)                              */
/*    linear_expansion_sum_zeroelim(elen, e, flen, f, h)                     */
/*    scale_expansion(elen, e, b, h)                                         */
/*    scale_expansion_zeroelim(elen, e, b, h)                                */
/*    compress(elen, e, h)                                                   */
/*                                                                           */
/*  All of these are described in the long version of the paper; some are    */
/*    described in the short version.  All return an integer that is the     */
/*    length of h.  Those with suffix _zeroelim perform zero elimination,    */
/*    and are recommended over their counterparts.  The procedure            */
/*    fast_expansion_sum_zeroelim() (or linear_expansion_sum_zeroelim() on   */
/*    processors that do not use the round-to-even tiebreaking rule) is      */
/*    recommended over expansion_sum_zeroelim().  Each procedure has a       */
/*    little note next to it (in the code below) that tells you whether or   */
/*    not the output expansion may be the same array as one of the input     */
/*    expansions.                                                            */
/*                                                                           */
/*                                                                           */
/*  If you look around below, you'll also find macros for a bunch of         */
/*    simple unrolled arithmetic operations, and procedures for printing     */
/*    expansions (commented out because they don't work with all C           */
/*    compilers) and for generating random floating-point numbers whose      */
/*    significand bits are all random.  Most of the macros have undocumented */
/*    requirements that certain of their parameters should not be the same   */
/*    variable; for safety, better to make sure all the parameters are       */
/*    distinct variables.  Feel free to send email to jrs@cs.cmu.edu if you  */
/*    have questions.                                                        */
/*                                                                           */
/*****************************************************************************/

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4131)
#endif // ifdef _WIN32

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
//#include <sys/time.h>

/* On some machines, the exact arithmetic routines might be defeated by the  */
/*   use of internal extended precision floating-point registers.  Sometimes */
/*   this problem can be fixed by defining certain values to be volatile,    */
/*   thus forcing them to be stored to memory and rounded off.  This isn't   */
/*   a great solution, though, as it slows the arithmetic down.              */
/*                                                                           */
/* To try this out, write "#define INEXACT volatile" below.  Normally,       */
/*   however, INEXACT should be defined to be nothing.  ("#define INEXACT".) */

#define INEXACT /* Nothing */
/* #define INEXACT volatile */

#define REAL double /* float or double */
#define REALPRINT doubleprint
#define REALRAND doublerand
#define NARROWRAND narrowdoublerand
#define UNIFORMRAND uniformdoublerand

/* Which of the following two methods of finding the absolute values is      */
/*   fastest is compiler-dependent.  A few compilers can inline and optimize */
/*   the fabs() call; but most will incur the overhead of a function call,   */
/*   which is disastrously slow.  A faster way on IEEE machines might be to  */
/*   mask the appropriate bit, but that's difficult to do in C.              */

#define Absolute(a) ((a) >= 0.0 ? (a) : -(a))
/* #define Absolute(a)  fabs(a) */

/* Many of the operations are broken up into two pieces, a main part that    */
/*   performs an approximate operation, and a "tail" that computes the       */
/*   roundoff error of that operation.                                       */
/*                                                                           */
/* The operations Fast_Two_Sum(), Fast_Two_Diff(), Two_Sum(), Two_Diff(),    */
/*   Split(), and Two_Product() are all implemented as described in the      */
/*   reference.  Each of these macros requires certain variables to be       */
/*   defined in the calling routine.  The variables `bvirt', `c', `abig',    */
/*   `_i', `_j', `_k', `_l', `_m', and `_n' are declared `INEXACT' because   */
/*   they store the result of an operation that may incur roundoff error.    */
/*   The input parameter `x' (or the highest numbered `x_' parameter) must   */
/*   also be declared `INEXACT'.                                             */

#define Fast_Two_Sum_Tail(a, b, x, y) \
    bvirt = x - a;                    \
    y = b - bvirt

#define Fast_Two_Sum(a, b, x, y) \
    x = (REAL)(a + b);           \
    Fast_Two_Sum_Tail(a, b, x, y)

#define Fast_Two_Diff_Tail(a, b, x, y) \
    bvirt = a - x;                     \
    y = bvirt - b

#define Fast_Two_Diff(a, b, x, y) \
    x = (REAL)(a - b);            \
    Fast_Two_Diff_Tail(a, b, x, y)

#define Two_Sum_Tail(a, b, x, y) \
    bvirt = (REAL)(x - a);       \
    avirt = x - bvirt;           \
    bround = b - bvirt;          \
    around = a - avirt;          \
    y = around + bround

#define Two_Sum(a, b, x, y) \
    x = (REAL)(a + b);      \
    Two_Sum_Tail(a, b, x, y)

#define Two_Diff_Tail(a, b, x, y) \
    bvirt = (REAL)(a - x);        \
    avirt = x + bvirt;            \
    bround = bvirt - b;           \
    around = a - avirt;           \
    y = around + bround

#define Two_Diff(a, b, x, y) \
    x = (REAL)(a - b);       \
    Two_Diff_Tail(a, b, x, y)

#define Split(a, ahi, alo)    \
    c = (REAL)(splitter * a); \
    abig = (REAL)(c - a);     \
    ahi = c - abig;           \
    alo = a - ahi

#define Two_Product_Tail(a, b, x, y) \
    Split(a, ahi, alo);              \
    Split(b, bhi, blo);              \
    err1 = x - (ahi * bhi);          \
    err2 = err1 - (alo * bhi);       \
    err3 = err2 - (ahi * blo);       \
    y = (alo * blo) - err3

#define Two_Product(a, b, x, y) \
    x = (REAL)(a * b);          \
    Two_Product_Tail(a, b, x, y)

/* Two_Product_Presplit() is Two_Product() where one of the inputs has       */
/*   already been split.  Avoids redundant splitting.                        */

#define Two_Product_Presplit(a, b, bhi, blo, x, y) \
    x = (REAL)(a * b);                             \
    Split(a, ahi, alo);                            \
    err1 = x - (ahi * bhi);                        \
    err2 = err1 - (alo * bhi);                     \
    err3 = err2 - (ahi * blo);                     \
    y = (alo * blo) - err3

/* Two_Product_2Presplit() is Two_Product() where both of the inputs have    */
/*   already been split.  Avoids redundant splitting.                        */

#define Two_Product_2Presplit(a, ahi, alo, b, bhi, blo, x, y) \
    x = (REAL)(a * b);                                        \
    err1 = x - (ahi * bhi);                                   \
    err2 = err1 - (alo * bhi);                                \
    err3 = err2 - (ahi * blo);                                \
    y = (alo * blo) - err3

/* Square() can be done more quickly than Two_Product().                     */

#define Square_Tail(a, x, y)           \
    Split(a, ahi, alo);                \
    err1 = x - (ahi * ahi);            \
    err3 = err1 - ((ahi + ahi) * alo); \
    y = (alo * alo) - err3

#define Square(a, x, y) \
    x = (REAL)(a * a);  \
    Square_Tail(a, x, y)

/* Macros for summing expansions of various fixed lengths.  These are all    */
/*   unrolled versions of Expansion_Sum().                                   */

#define Two_One_Sum(a1, a0, b, x2, x1, x0) \
    Two_Sum(a0, b, _i, x0);                \
    Two_Sum(a1, _i, x2, x1)

#define Two_One_Diff(a1, a0, b, x2, x1, x0) \
    Two_Diff(a0, b, _i, x0);                \
    Two_Sum(a1, _i, x2, x1)

#define Two_Two_Sum(a1, a0, b1, b0, x3, x2, x1, x0) \
    Two_One_Sum(a1, a0, b0, _j, _0, x0);            \
    Two_One_Sum(_j, _0, b1, x3, x2, x1)

#define Two_Two_Diff(a1, a0, b1, b0, x3, x2, x1, x0) \
    Two_One_Diff(a1, a0, b0, _j, _0, x0);            \
    Two_One_Diff(_j, _0, b1, x3, x2, x1)

#define Four_One_Sum(a3, a2, a1, a0, b, x4, x3, x2, x1, x0) \
    Two_One_Sum(a1, a0, b, _j, x1, x0);                     \
    Two_One_Sum(a3, a2, _j, x4, x3, x2)

#define Four_Two_Sum(a3, a2, a1, a0, b1, b0, x5, x4, x3, x2, x1, x0) \
    Four_One_Sum(a3, a2, a1, a0, b0, _k, _2, _1, _0, x0);            \
    Four_One_Sum(_k, _2, _1, _0, b1, x5, x4, x3, x2, x1)

#define Four_Four_Sum(a3, a2, a1, a0, b4, b3, b1, b0, x7, x6, x5, x4, x3, x2, \
    x1, x0)                                                                   \
    Four_Two_Sum(a3, a2, a1, a0, b1, b0, _l, _2, _1, _0, x1, x0);             \
    Four_Two_Sum(_l, _2, _1, _0, b4, b3, x7, x6, x5, x4, x3, x2)

#define Eight_One_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b, x8, x7, x6, x5, x4, \
    x3, x2, x1, x0)                                                          \
    Four_One_Sum(a3, a2, a1, a0, b, _j, x3, x2, x1, x0);                     \
    Four_One_Sum(a7, a6, a5, a4, _j, x8, x7, x6, x5, x4)

#define Eight_Two_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b1, b0, x9, x8, x7,     \
    x6, x5, x4, x3, x2, x1, x0)                                               \
    Eight_One_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b0, _k, _6, _5, _4, _3, _2, \
        _1, _0, x0);                                                          \
    Eight_One_Sum(_k, _6, _5, _4, _3, _2, _1, _0, b1, x9, x8, x7, x6, x5, x4, \
        x3, x2, x1)

#define Eight_Four_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b4, b3, b1, b0, x11,   \
    x10, x9, x8, x7, x6, x5, x4, x3, x2, x1, x0)                              \
    Eight_Two_Sum(a7, a6, a5, a4, a3, a2, a1, a0, b1, b0, _l, _6, _5, _4, _3, \
        _2, _1, _0, x1, x0);                                                  \
    Eight_Two_Sum(_l, _6, _5, _4, _3, _2, _1, _0, b4, b3, x11, x10, x9, x8,   \
        x7, x6, x5, x4, x3, x2)

/* Macros for multiplying expansions of various fixed lengths.               */

#define Two_One_Product(a1, a0, b, x3, x2, x1, x0) \
    Split(b, bhi, blo);                            \
    Two_Product_Presplit(a0, b, bhi, blo, _i, x0); \
    Two_Product_Presplit(a1, b, bhi, blo, _j, _0); \
    Two_Sum(_i, _0, _k, x1);                       \
    Fast_Two_Sum(_j, _k, x3, x2)

#define Four_One_Product(a3, a2, a1, a0, b, x7, x6, x5, x4, x3, x2, x1, x0) \
    Split(b, bhi, blo);                                                     \
    Two_Product_Presplit(a0, b, bhi, blo, _i, x0);                          \
    Two_Product_Presplit(a1, b, bhi, blo, _j, _0);                          \
    Two_Sum(_i, _0, _k, x1);                                                \
    Fast_Two_Sum(_j, _k, _i, x2);                                           \
    Two_Product_Presplit(a2, b, bhi, blo, _j, _0);                          \
    Two_Sum(_i, _0, _k, x3);                                                \
    Fast_Two_Sum(_j, _k, _i, x4);                                           \
    Two_Product_Presplit(a3, b, bhi, blo, _j, _0);                          \
    Two_Sum(_i, _0, _k, x5);                                                \
    Fast_Two_Sum(_j, _k, x7, x6)

#define Two_Two_Product(a1, a0, b1, b0, x7, x6, x5, x4, x3, x2, x1, x0) \
    Split(a0, a0hi, a0lo);                                              \
    Split(b0, bhi, blo);                                                \
    Two_Product_2Presplit(a0, a0hi, a0lo, b0, bhi, blo, _i, x0);        \
    Split(a1, a1hi, a1lo);                                              \
    Two_Product_2Presplit(a1, a1hi, a1lo, b0, bhi, blo, _j, _0);        \
    Two_Sum(_i, _0, _k, _1);                                            \
    Fast_Two_Sum(_j, _k, _l, _2);                                       \
    Split(b1, bhi, blo);                                                \
    Two_Product_2Presplit(a0, a0hi, a0lo, b1, bhi, blo, _i, _0);        \
    Two_Sum(_1, _0, _k, x1);                                            \
    Two_Sum(_2, _k, _j, _1);                                            \
    Two_Sum(_l, _j, _m, _2);                                            \
    Two_Product_2Presplit(a1, a1hi, a1lo, b1, bhi, blo, _j, _0);        \
    Two_Sum(_i, _0, _n, _0);                                            \
    Two_Sum(_1, _0, _i, x2);                                            \
    Two_Sum(_2, _i, _k, _1);                                            \
    Two_Sum(_m, _k, _l, _2);                                            \
    Two_Sum(_j, _n, _k, _0);                                            \
    Two_Sum(_1, _0, _j, x3);                                            \
    Two_Sum(_2, _j, _i, _1);                                            \
    Two_Sum(_l, _i, _m, _2);                                            \
    Two_Sum(_1, _k, _i, x4);                                            \
    Two_Sum(_2, _i, _k, x5);                                            \
    Two_Sum(_m, _k, x7, x6)

/* An expansion of length two can be squared more quickly than finding the   */
/*   product of two different expansions of length two, and the result is    */
/*   guaranteed to have no more than six (rather than eight) components.     */

#define Two_Square(a1, a0, x5, x4, x3, x2, x1, x0) \
    Square(a0, _j, x0);                            \
    _0 = a0 + a0;                                  \
    Two_Product(a1, _0, _k, _1);                   \
    Two_One_Sum(_k, _1, _j, _l, _2, x1);           \
    Square(a1, _j, _1);                            \
    Two_Two_Sum(_j, _1, _l, _2, x5, x4, x3, x2)

REAL splitter; /* = 2^ceiling(p / 2) + 1.  Used to split floats in half. */
REAL epsilon; /* = 2^(-p).  Used to estimate roundoff errors. */
/* A set of coefficients used to calculate maximum roundoff errors.          */
REAL resulterrbound;
REAL ccwerrboundA, ccwerrboundB, ccwerrboundC;
REAL o3derrboundA, o3derrboundB, o3derrboundC;
REAL iccerrboundA, iccerrboundB, iccerrboundC;
REAL isperrboundA, isperrboundB, isperrboundC;

/*****************************************************************************/
/*                                                                           */
/*  doubleprint()   Print the bit representation of a double.                */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void doubleprint(number)
double number;
{
  unsigned long long no;
  unsigned long long sign, expo;
  int exponent;
  int i, bottomi;

  no = *(unsigned long long *) &number;
  sign = no & 0x8000000000000000ll;
  expo = (no >> 52) & 0x7ffll;
  exponent = (int) expo;
  exponent = exponent - 1023;
  if (sign) {
    printf("-");
  } else {
    printf(" ");
  }
  if (exponent == -1023) {
    printf(
      "0.0000000000000000000000000000000000000000000000000000_     (   )");
  } else {
    printf("1.");
    bottomi = -1;
    for (i = 0; i < 52; i++) {
      if (no & 0x0008000000000000ll) {
        printf("1");
        bottomi = i;
      } else {
        printf("0");
      }
      no <<= 1;
    }
    printf("_%d  (%d)", exponent, exponent - 1 - bottomi);
  }
}
*/

/*****************************************************************************/
/*                                                                           */
/*  floatprint()   Print the bit representation of a float.                  */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void floatprint(number)
float number;
{
  unsigned no;
  unsigned sign, expo;
  int exponent;
  int i, bottomi;

  no = *(unsigned *) &number;
  sign = no & 0x80000000;
  expo = (no >> 23) & 0xff;
  exponent = (int) expo;
  exponent = exponent - 127;
  if (sign) {
    printf("-");
  } else {
    printf(" ");
  }
  if (exponent == -127) {
    printf("0.00000000000000000000000_     (   )");
  } else {
    printf("1.");
    bottomi = -1;
    for (i = 0; i < 23; i++) {
      if (no & 0x00400000) {
        printf("1");
        bottomi = i;
      } else {
        printf("0");
      }
      no <<= 1;
    }
    printf("_%3d  (%3d)", exponent, exponent - 1 - bottomi);
  }
}
*/

/*****************************************************************************/
/*                                                                           */
/*  expansion_print()   Print the bit representation of an expansion.        */
/*                                                                           */
/*  Useful for debugging exact arithmetic routines.                          */
/*                                                                           */
/*****************************************************************************/

/*
void expansion_print(elen, e)
int elen;
REAL *e;
{
  int i;

  for (i = elen - 1; i >= 0; i--) {
    REALPRINT(e[i]);
    if (i > 0) {
      printf(" +\n");
    } else {
      printf("\n");
    }
  }
}
*/

#if 0
/*****************************************************************************/
/*                                                                           */
/*  doublerand()   Generate a double with random 53-bit significand and a    */
/*                 random exponent in [0, 511].                              */
/*                                                                           */
/*****************************************************************************/

double doublerand()
{
    double result;
    double expo;
    long a, b, c;
    long i;

    a = random();
    b = random();
    c = random();
    result = (double)(a - 1073741824) * 8388608.0 + (double)(b >> 8);
    for (i = 512, expo = 2; i <= 131072; i *= 2, expo = expo * expo) {
        if (c & i) {
            result *= expo;
        }
    }
    return result;
}

/*****************************************************************************/
/*                                                                           */
/*  narrowdoublerand()   Generate a double with random 53-bit significand    */
/*                       and a random exponent in [0, 7].                    */
/*                                                                           */
/*****************************************************************************/

double narrowdoublerand()
{
    double result;
    double expo;
    long a, b, c;
    long i;

    a = random();
    b = random();
    c = random();
    result = (double)(a - 1073741824) * 8388608.0 + (double)(b >> 8);
    for (i = 512, expo = 2; i <= 2048; i *= 2, expo = expo * expo) {
        if (c & i) {
            result *= expo;
        }
    }
    return result;
}

/*****************************************************************************/
/*                                                                           */
/*  uniformdoublerand()   Generate a double with random 53-bit significand.  */
/*                                                                           */
/*****************************************************************************/

double uniformdoublerand()
{
    double result;
    long a, b;

    a = random();
    b = random();
    result = (double)(a - 1073741824) * 8388608.0 + (double)(b >> 8);
    return result;
}

/*****************************************************************************/
/*                                                                           */
/*  floatrand()   Generate a float with random 24-bit significand and a      */
/*                random exponent in [0, 63].                                */
/*                                                                           */
/*****************************************************************************/

float floatrand()
{
    float result;
    float expo;
    long a, c;
    long i;

    a = random();
    c = random();
    result = (float)((a - 1073741824) >> 6);
    for (i = 512, expo = 2; i <= 16384; i *= 2, expo = expo * expo) {
        if (c & i) {
            result *= expo;
        }
    }
    return result;
}

/*****************************************************************************/
/*                                                                           */
/*  narrowfloatrand()   Generate a float with random 24-bit significand and  */
/*                      a random exponent in [0, 7].                         */
/*                                                                           */
/*****************************************************************************/

float narrowfloatrand()
{
    float result;
    float expo;
    long a, c;
    long i;

    a = random();
    c = random();
    result = (float)((a - 1073741824) >> 6);
    for (i = 512, expo = 2; i <= 2048; i *= 2, expo = expo * expo) {
        if (c & i) {
            result *= expo;
        }
    }
    return result;
}

/*****************************************************************************/
/*                                                                           */
/*  uniformfloatrand()   Generate a float with random 24-bit significand.    */
/*                                                                           */
/*****************************************************************************/

float uniformfloatrand()
{
    float result;
    long a;

    a = random();
    result = (float)((a - 1073741824) >> 6);
    return result;
}
#endif
/*****************************************************************************/
/*                                                                           */
/*  exactinit()   Initialize the variables used for exact arithmetic.        */
/*                                                                           */
/*  `epsilon' is the largest power of two such that 1.0 + epsilon = 1.0 in   */
/*  floating-point arithmetic.  `epsilon' bounds the relative roundoff       */
/*  error.  It is used for floating-point error analysis.                    */
/*                                                                           */
/*  `splitter' is used to split floating-point numbers into two half-        */
/*  length significands for exact multiplication.                            */
/*                                                                           */
/*  I imagine that a highly optimizing compiler might be too smart for its   */
/*  own good, and somehow cause this routine to fail, if it pretends that    */
/*  floating-point arithmetic is too much like real arithmetic.              */
/*                                                                           */
/*  Don't change this routine unless you fully understand it.                */
/*                                                                           */
/*****************************************************************************/

void exactinit()
{
    REAL half;
    REAL check, lastcheck;
    int every_other;

    every_other = 1;
    half = 0.5;
    epsilon = 1.0;
    splitter = 1.0;
    check = 1.0;
    /* Repeatedly divide `epsilon' by two until it is too small to add to    */
    /*   one without causing roundoff.  (Also check if the sum is equal to   */
    /*   the previous sum, for machines that round up instead of using exact */
    /*   rounding.  Not that this library will work on such machines anyway. */
    do {
        lastcheck = check;
        epsilon *= half;
        if (every_other) {
            splitter *= 2.0;
        }
        every_other = !every_other;
        check = 1.0 + epsilon;
    } while ((check != 1.0) && (check != lastcheck));
    splitter += 1.0;

    /* Error bounds for orientation and incircle tests. */
    resulterrbound = (3.0 + 8.0 * epsilon) * epsilon;
    ccwerrboundA = (3.0 + 16.0 * epsilon) * epsilon;
    ccwerrboundB = (2.0 + 12.0 * epsilon) * epsilon;
    ccwerrboundC = (9.0 + 64.0 * epsilon) * epsilon * epsilon;
    o3derrboundA = (7.0 + 56.0 * epsilon) * epsilon;
    o3derrboundB = (3.0 + 28.0 * epsilon) * epsilon;
    o3derrboundC = (26.0 + 288.0 * epsilon) * epsilon * epsilon;
    iccerrboundA = (10.0 + 96.0 * epsilon) * epsilon;
    iccerrboundB = (4.0 + 48.0 * epsilon) * epsilon;
    iccerrboundC = (44.0 + 576.0 * epsilon) * epsilon * epsilon;
    isperrboundA = (16.0 + 224.0 * epsilon) * epsilon;
    isperrboundB = (5.0 + 72.0 * epsilon) * epsilon;
    isperrboundC = (71.0 + 1408.0 * epsilon) * epsilon * epsilon;
}

/*****************************************************************************/
/*                                                                           */
/*  grow_expansion()   Add a scalar to an expansion.                         */
/*                                                                           */
/*  Sets h = e + b.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int grow_expansion(elen, e, b, h) /* e and h can be the same. */
    int elen;
REAL* e;
REAL b;
REAL* h;
{
    REAL Q;
    INEXACT REAL Qnew;
    int eindex;
    REAL enow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;

    Q = b;
    for (eindex = 0; eindex < elen; eindex++) {
        enow = e[eindex];
        Two_Sum(Q, enow, Qnew, h[eindex]);
        Q = Qnew;
    }
    h[eindex] = Q;
    return eindex + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  grow_expansion_zeroelim()   Add a scalar to an expansion, eliminating    */
/*                              zero components from the output expansion.   */
/*                                                                           */
/*  Sets h = e + b.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int grow_expansion_zeroelim(elen, e, b, h) /* e and h can be the same. */
    int elen;
REAL* e;
REAL b;
REAL* h;
{
    REAL Q, hh;
    INEXACT REAL Qnew;
    int eindex, hindex;
    REAL enow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;

    hindex = 0;
    Q = b;
    for (eindex = 0; eindex < elen; eindex++) {
        enow = e[eindex];
        Two_Sum(Q, enow, Qnew, hh);
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum()   Sum two expansions.                                    */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum(elen, e, flen, f, h)
    /* e and h can be the same, but f and h cannot. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q;
    INEXACT REAL Qnew;
    int findex, hindex, hlast;
    REAL hnow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;

    Q = f[0];
    for (hindex = 0; hindex < elen; hindex++) {
        hnow = e[hindex];
        Two_Sum(Q, hnow, Qnew, h[hindex]);
        Q = Qnew;
    }
    h[hindex] = Q;
    hlast = hindex;
    for (findex = 1; findex < flen; findex++) {
        Q = f[findex];
        for (hindex = findex; hindex <= hlast; hindex++) {
            hnow = h[hindex];
            Two_Sum(Q, hnow, Qnew, h[hindex]);
            Q = Qnew;
        }
        h[++hlast] = Q;
    }
    return hlast + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum_zeroelim1()   Sum two expansions, eliminating zero         */
/*                              components from the output expansion.        */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum_zeroelim1(elen, e, flen, f, h)
    /* e and h can be the same, but f and h cannot. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q;
    INEXACT REAL Qnew;
    int index, findex, hindex, hlast;
    REAL hnow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;

    Q = f[0];
    for (hindex = 0; hindex < elen; hindex++) {
        hnow = e[hindex];
        Two_Sum(Q, hnow, Qnew, h[hindex]);
        Q = Qnew;
    }
    h[hindex] = Q;
    hlast = hindex;
    for (findex = 1; findex < flen; findex++) {
        Q = f[findex];
        for (hindex = findex; hindex <= hlast; hindex++) {
            hnow = h[hindex];
            Two_Sum(Q, hnow, Qnew, h[hindex]);
            Q = Qnew;
        }
        h[++hlast] = Q;
    }
    hindex = -1;
    for (index = 0; index <= hlast; index++) {
        hnow = h[index];
        if (hnow != 0.0) {
            h[++hindex] = hnow;
        }
    }
    if (hindex == -1) {
        return 1;
    } else {
        return hindex + 1;
    }
}

/*****************************************************************************/
/*                                                                           */
/*  expansion_sum_zeroelim2()   Sum two expansions, eliminating zero         */
/*                              components from the output expansion.        */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the nonadjacent property as well.  (That is,   */
/*  if e has one of these properties, so will h.)  Does NOT maintain the     */
/*  strongly nonoverlapping property.                                        */
/*                                                                           */
/*****************************************************************************/

int expansion_sum_zeroelim2(elen, e, flen, f, h)
    /* e and h can be the same, but f and h cannot. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q, hh;
    INEXACT REAL Qnew;
    int eindex, findex, hindex, hlast;
    REAL enow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;

    hindex = 0;
    Q = f[0];
    for (eindex = 0; eindex < elen; eindex++) {
        enow = e[eindex];
        Two_Sum(Q, enow, Qnew, hh);
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    h[hindex] = Q;
    hlast = hindex;
    for (findex = 1; findex < flen; findex++) {
        hindex = 0;
        Q = f[findex];
        for (eindex = 0; eindex <= hlast; eindex++) {
            enow = h[eindex];
            Two_Sum(Q, enow, Qnew, hh);
            Q = Qnew;
            if (hh != 0) {
                h[hindex++] = hh;
            }
        }
        h[hindex] = Q;
        hlast = hindex;
    }
    return hlast + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  fast_expansion_sum()   Sum two expansions.                               */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  If round-to-even is used (as with IEEE 754), maintains the strongly      */
/*  nonoverlapping property.  (That is, if e is strongly nonoverlapping, h   */
/*  will be also.)  Does NOT maintain the nonoverlapping or nonadjacent      */
/*  properties.                                                              */
/*                                                                           */
/*****************************************************************************/

int fast_expansion_sum(elen, e, flen, f, h) /* h cannot be e or f. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q;
    INEXACT REAL Qnew;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    int eindex, findex, hindex;
    REAL enow, fnow;

    enow = e[0];
    fnow = f[0];
    eindex = findex = 0;
    if ((fnow > enow) == (fnow > -enow)) {
        Q = enow;
        enow = e[++eindex];
    } else {
        Q = fnow;
        fnow = f[++findex];
    }
    hindex = 0;
    if ((eindex < elen) && (findex < flen)) {
        if ((fnow > enow) == (fnow > -enow)) {
            Fast_Two_Sum(enow, Q, Qnew, h[0]);
            enow = e[++eindex];
        } else {
            Fast_Two_Sum(fnow, Q, Qnew, h[0]);
            fnow = f[++findex];
        }
        Q = Qnew;
        hindex = 1;
        while ((eindex < elen) && (findex < flen)) {
            if ((fnow > enow) == (fnow > -enow)) {
                Two_Sum(Q, enow, Qnew, h[hindex]);
                enow = e[++eindex];
            } else {
                Two_Sum(Q, fnow, Qnew, h[hindex]);
                fnow = f[++findex];
            }
            Q = Qnew;
            hindex++;
        }
    }
    while (eindex < elen) {
        Two_Sum(Q, enow, Qnew, h[hindex]);
        enow = e[++eindex];
        Q = Qnew;
        hindex++;
    }
    while (findex < flen) {
        Two_Sum(Q, fnow, Qnew, h[hindex]);
        fnow = f[++findex];
        Q = Qnew;
        hindex++;
    }
    h[hindex] = Q;
    return hindex + 1;
}

/*****************************************************************************/
/*                                                                           */
/*  fast_expansion_sum_zeroelim()   Sum two expansions, eliminating zero     */
/*                                  components from the output expansion.    */
/*                                                                           */
/*  Sets h = e + f.  See the long version of my paper for details.           */
/*                                                                           */
/*  If round-to-even is used (as with IEEE 754), maintains the strongly      */
/*  nonoverlapping property.  (That is, if e is strongly nonoverlapping, h   */
/*  will be also.)  Does NOT maintain the nonoverlapping or nonadjacent      */
/*  properties.                                                              */
/*                                                                           */
/*****************************************************************************/

int fast_expansion_sum_zeroelim(elen, e, flen, f, h) /* h cannot be e or f. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q;
    INEXACT REAL Qnew;
    INEXACT REAL hh;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    int eindex, findex, hindex;
    REAL enow, fnow;

    enow = e[0];
    fnow = f[0];
    eindex = findex = 0;
    if ((fnow > enow) == (fnow > -enow)) {
        Q = enow;
        enow = e[++eindex];
    } else {
        Q = fnow;
        fnow = f[++findex];
    }
    hindex = 0;
    if ((eindex < elen) && (findex < flen)) {
        if ((fnow > enow) == (fnow > -enow)) {
            Fast_Two_Sum(enow, Q, Qnew, hh);
            enow = e[++eindex];
        } else {
            Fast_Two_Sum(fnow, Q, Qnew, hh);
            fnow = f[++findex];
        }
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
        while ((eindex < elen) && (findex < flen)) {
            if ((fnow > enow) == (fnow > -enow)) {
                Two_Sum(Q, enow, Qnew, hh);
                enow = e[++eindex];
            } else {
                Two_Sum(Q, fnow, Qnew, hh);
                fnow = f[++findex];
            }
            Q = Qnew;
            if (hh != 0.0) {
                h[hindex++] = hh;
            }
        }
    }
    while (eindex < elen) {
        Two_Sum(Q, enow, Qnew, hh);
        enow = e[++eindex];
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    while (findex < flen) {
        Two_Sum(Q, fnow, Qnew, hh);
        fnow = f[++findex];
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  linear_expansion_sum()   Sum two expansions.                             */
/*                                                                           */
/*  Sets h = e + f.  See either version of my paper for details.             */
/*                                                                           */
/*  Maintains the nonoverlapping property.  (That is, if e is                */
/*  nonoverlapping, h will be also.)                                         */
/*                                                                           */
/*****************************************************************************/

int linear_expansion_sum(elen, e, flen, f, h) /* h cannot be e or f. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q, q;
    INEXACT REAL Qnew;
    INEXACT REAL R;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    int eindex, findex, hindex;
    REAL enow, fnow;
    REAL g0;

    enow = e[0];
    fnow = f[0];
    eindex = findex = 0;
    if ((fnow > enow) == (fnow > -enow)) {
        g0 = enow;
        enow = e[++eindex];
    } else {
        g0 = fnow;
        fnow = f[++findex];
    }
    if ((eindex < elen) && ((findex >= flen) || ((fnow > enow) == (fnow > -enow)))) {
        Fast_Two_Sum(enow, g0, Qnew, q);
        enow = e[++eindex];
    } else {
        Fast_Two_Sum(fnow, g0, Qnew, q);
        fnow = f[++findex];
    }
    Q = Qnew;
    for (hindex = 0; hindex < elen + flen - 2; hindex++) {
        if ((eindex < elen) && ((findex >= flen) || ((fnow > enow) == (fnow > -enow)))) {
            Fast_Two_Sum(enow, q, R, h[hindex]);
            enow = e[++eindex];
        } else {
            Fast_Two_Sum(fnow, q, R, h[hindex]);
            fnow = f[++findex];
        }
        Two_Sum(Q, R, Qnew, q);
        Q = Qnew;
    }
    h[hindex] = q;
    h[hindex + 1] = Q;
    return hindex + 2;
}

/*****************************************************************************/
/*                                                                           */
/*  linear_expansion_sum_zeroelim()   Sum two expansions, eliminating zero   */
/*                                    components from the output expansion.  */
/*                                                                           */
/*  Sets h = e + f.  See either version of my paper for details.             */
/*                                                                           */
/*  Maintains the nonoverlapping property.  (That is, if e is                */
/*  nonoverlapping, h will be also.)                                         */
/*                                                                           */
/*****************************************************************************/

int linear_expansion_sum_zeroelim(elen, e, flen, f, h) /* h cannot be e or f. */
    int elen;
REAL* e;
int flen;
REAL* f;
REAL* h;
{
    REAL Q, q, hh;
    INEXACT REAL Qnew;
    INEXACT REAL R;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    int eindex, findex, hindex;
    int count;
    REAL enow, fnow;
    REAL g0;

    enow = e[0];
    fnow = f[0];
    eindex = findex = 0;
    hindex = 0;
    if ((fnow > enow) == (fnow > -enow)) {
        g0 = enow;
        enow = e[++eindex];
    } else {
        g0 = fnow;
        fnow = f[++findex];
    }
    if ((eindex < elen) && ((findex >= flen) || ((fnow > enow) == (fnow > -enow)))) {
        Fast_Two_Sum(enow, g0, Qnew, q);
        enow = e[++eindex];
    } else {
        Fast_Two_Sum(fnow, g0, Qnew, q);
        fnow = f[++findex];
    }
    Q = Qnew;
    for (count = 2; count < elen + flen; count++) {
        if ((eindex < elen) && ((findex >= flen) || ((fnow > enow) == (fnow > -enow)))) {
            Fast_Two_Sum(enow, q, R, hh);
            enow = e[++eindex];
        } else {
            Fast_Two_Sum(fnow, q, R, hh);
            fnow = f[++findex];
        }
        Two_Sum(Q, R, Qnew, q);
        Q = Qnew;
        if (hh != 0) {
            h[hindex++] = hh;
        }
    }
    if (q != 0) {
        h[hindex++] = q;
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  scale_expansion()   Multiply an expansion by a scalar.                   */
/*                                                                           */
/*  Sets h = be.  See either version of my paper for details.                */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int scale_expansion(elen, e, b, h) /* e and h cannot be the same. */
    int elen;
REAL* e;
REAL b;
REAL* h;
{
    INEXACT REAL Q;
    INEXACT REAL sum;
    INEXACT REAL product1;
    REAL product0;
    int eindex, hindex;
    REAL enow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;

    Split(b, bhi, blo);
    Two_Product_Presplit(e[0], b, bhi, blo, Q, h[0]);
    hindex = 1;
    for (eindex = 1; eindex < elen; eindex++) {
        enow = e[eindex];
        Two_Product_Presplit(enow, b, bhi, blo, product1, product0);
        Two_Sum(Q, product0, sum, h[hindex]);
        hindex++;
        Two_Sum(product1, sum, Q, h[hindex]);
        hindex++;
    }
    h[hindex] = Q;
    return elen + elen;
}

/*****************************************************************************/
/*                                                                           */
/*  scale_expansion_zeroelim()   Multiply an expansion by a scalar,          */
/*                               eliminating zero components from the        */
/*                               output expansion.                           */
/*                                                                           */
/*  Sets h = be.  See either version of my paper for details.                */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), maintains the strongly nonoverlapping and nonadjacent    */
/*  properties as well.  (That is, if e has one of these properties, so      */
/*  will h.)                                                                 */
/*                                                                           */
/*****************************************************************************/

int scale_expansion_zeroelim(elen, e, b, h) /* e and h cannot be the same. */
    int elen;
REAL* e;
REAL b;
REAL* h;
{
    INEXACT REAL Q, sum;
    REAL hh;
    INEXACT REAL product1;
    REAL product0;
    int eindex, hindex;
    REAL enow;
    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;

    Split(b, bhi, blo);
    Two_Product_Presplit(e[0], b, bhi, blo, Q, hh);
    hindex = 0;
    if (hh != 0) {
        h[hindex++] = hh;
    }
    for (eindex = 1; eindex < elen; eindex++) {
        enow = e[eindex];
        Two_Product_Presplit(enow, b, bhi, blo, product1, product0);
        Two_Sum(Q, product0, sum, hh);
        if (hh != 0) {
            h[hindex++] = hh;
        }
        Fast_Two_Sum(product1, sum, Q, hh);
        if (hh != 0) {
            h[hindex++] = hh;
        }
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

/*****************************************************************************/
/*                                                                           */
/*  compress()   Compress an expansion.                                      */
/*                                                                           */
/*  See the long version of my paper for details.                            */
/*                                                                           */
/*  Maintains the nonoverlapping property.  If round-to-even is used (as     */
/*  with IEEE 754), then any nonoverlapping expansion is converted to a      */
/*  nonadjacent expansion.                                                   */
/*                                                                           */
/*****************************************************************************/

//int compress(elen, e, h) /* e and h may be the same. */
//    int elen;
//REAL* e;
//REAL* h;
//{
//    REAL Q, q;
//    INEXACT REAL Qnew;
//    int eindex, hindex;
//    INEXACT REAL bvirt;
//    REAL enow, hnow;
//    int top, bottom;
//
//    bottom = elen - 1;
//    Q = e[bottom];
//    for (eindex = elen - 2; eindex >= 0; eindex--) {
//        enow = e[eindex];
//        Fast_Two_Sum(Q, enow, Qnew, q);
//        if (q != 0) {
//            h[bottom--] = Qnew;
//            Q = q;
//        } else {
//            Q = Qnew;
//        }
//    }
//    top = 0;
//    for (hindex = bottom + 1; hindex < elen; hindex++) {
//        hnow = h[hindex];
//        Fast_Two_Sum(hnow, Q, Qnew, q);
//        if (q != 0) {
//            h[top++] = q;
//        }
//        Q = Qnew;
//    }
//    h[top] = Q;
//    return top + 1;
//}

/*****************************************************************************/
/*                                                                           */
/*  estimate()   Produce a one-word estimate of an expansion's value.        */
/*                                                                           */
/*  See either version of my paper for details.                              */
/*                                                                           */
/*****************************************************************************/

REAL estimate(elen, e) int elen;
REAL* e;
{
    REAL Q;
    int eindex;

    Q = e[0];
    for (eindex = 1; eindex < elen; eindex++) {
        Q += e[eindex];
    }
    return Q;
}

/*****************************************************************************/
/*                                                                           */
/*  orient2dfast()   Approximate 2D orientation test.  Nonrobust.            */
/*  orient2dexact()   Exact 2D orientation test.  Robust.                    */
/*  orient2dslow()   Another exact 2D orientation test.  Robust.             */
/*  orient2d()   Adaptive exact 2D orientation test.  Robust.                */
/*                                                                           */
/*               Return a positive value if the points pa, pb, and pc occur  */
/*               in counterclockwise order; a negative value if they occur   */
/*               in clockwise order; and zero if they are collinear.  The    */
/*               result is also a rough approximation of twice the signed    */
/*               area of the triangle defined by the three points.           */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In orient2d() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, orient2d() is usually quite */
/*  fast, but will run more slowly when the input points are collinear or    */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL orient2dfast(pa, pb, pc)
    REAL* pa;
REAL* pb;
REAL* pc;
{
    REAL acx, bcx, acy, bcy;

    acx = pa[0] - pc[0];
    bcx = pb[0] - pc[0];
    acy = pa[1] - pc[1];
    bcy = pb[1] - pc[1];
    return acx * bcy - acy * bcx;
}

REAL orient2dexact(pa, pb, pc)
    REAL* pa;
REAL* pb;
REAL* pc;
{
    INEXACT REAL axby1, axcy1, bxcy1, bxay1, cxay1, cxby1;
    REAL axby0, axcy0, bxcy0, bxay0, cxay0, cxby0;
    REAL aterms[4], bterms[4], cterms[4];
    INEXACT REAL aterms3, bterms3, cterms3;
    REAL v[8], w[12];
    int vlength, wlength;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    Two_Product(pa[0], pb[1], axby1, axby0);
    Two_Product(pa[0], pc[1], axcy1, axcy0);
    Two_Two_Diff(axby1, axby0, axcy1, axcy0,
        aterms3, aterms[2], aterms[1], aterms[0]);
    aterms[3] = aterms3;

    Two_Product(pb[0], pc[1], bxcy1, bxcy0);
    Two_Product(pb[0], pa[1], bxay1, bxay0);
    Two_Two_Diff(bxcy1, bxcy0, bxay1, bxay0,
        bterms3, bterms[2], bterms[1], bterms[0]);
    bterms[3] = bterms3;

    Two_Product(pc[0], pa[1], cxay1, cxay0);
    Two_Product(pc[0], pb[1], cxby1, cxby0);
    Two_Two_Diff(cxay1, cxay0, cxby1, cxby0,
        cterms3, cterms[2], cterms[1], cterms[0]);
    cterms[3] = cterms3;

    vlength = fast_expansion_sum_zeroelim(4, aterms, 4, bterms, v);
    wlength = fast_expansion_sum_zeroelim(vlength, v, 4, cterms, w);

    return w[wlength - 1];
}

REAL orient2dslow(pa, pb, pc)
    REAL* pa;
REAL* pb;
REAL* pc;
{
    INEXACT REAL acx, acy, bcx, bcy;
    REAL acxtail, acytail;
    REAL bcxtail, bcytail;
    REAL negate, negatetail;
    REAL axby[8], bxay[8];
    INEXACT REAL axby7, bxay7;
    REAL deter[16];
    int deterlen;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j, _k, _l, _m, _n;
    REAL _0, _1, _2;

    Two_Diff(pa[0], pc[0], acx, acxtail);
    Two_Diff(pa[1], pc[1], acy, acytail);
    Two_Diff(pb[0], pc[0], bcx, bcxtail);
    Two_Diff(pb[1], pc[1], bcy, bcytail);

    Two_Two_Product(acx, acxtail, bcy, bcytail,
        axby7, axby[6], axby[5], axby[4],
        axby[3], axby[2], axby[1], axby[0]);
    axby[7] = axby7;
    negate = -acy;
    negatetail = -acytail;
    Two_Two_Product(bcx, bcxtail, negate, negatetail,
        bxay7, bxay[6], bxay[5], bxay[4],
        bxay[3], bxay[2], bxay[1], bxay[0]);
    bxay[7] = bxay7;

    deterlen = fast_expansion_sum_zeroelim(8, axby, 8, bxay, deter);

    return deter[deterlen - 1];
}

REAL orient2dadapt(pa, pb, pc, detsum)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL detsum;
{
    INEXACT REAL acx, acy, bcx, bcy;
    REAL acxtail, acytail, bcxtail, bcytail;
    INEXACT REAL detleft, detright;
    REAL detlefttail, detrighttail;
    REAL det, errbound;
    REAL B[4], C1[8], C2[12], D[16];
    INEXACT REAL B3;
    int C1length, C2length, Dlength;
    REAL u[4];
    INEXACT REAL u3;
    INEXACT REAL s1, t1;
    REAL s0, t0;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    acx = (REAL)(pa[0] - pc[0]);
    bcx = (REAL)(pb[0] - pc[0]);
    acy = (REAL)(pa[1] - pc[1]);
    bcy = (REAL)(pb[1] - pc[1]);

    Two_Product(acx, bcy, detleft, detlefttail);
    Two_Product(acy, bcx, detright, detrighttail);

    Two_Two_Diff(detleft, detlefttail, detright, detrighttail,
        B3, B[2], B[1], B[0]);
    B[3] = B3;

    det = estimate(4, B);
    errbound = ccwerrboundB * detsum;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    Two_Diff_Tail(pa[0], pc[0], acx, acxtail);
    Two_Diff_Tail(pb[0], pc[0], bcx, bcxtail);
    Two_Diff_Tail(pa[1], pc[1], acy, acytail);
    Two_Diff_Tail(pb[1], pc[1], bcy, bcytail);

    if ((acxtail == 0.0) && (acytail == 0.0)
        && (bcxtail == 0.0) && (bcytail == 0.0)) {
        return det;
    }

    errbound = ccwerrboundC * detsum + resulterrbound * Absolute(det);
    det += (acx * bcytail + bcy * acxtail)
        - (acy * bcxtail + bcx * acytail);
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    Two_Product(acxtail, bcy, s1, s0);
    Two_Product(acytail, bcx, t1, t0);
    Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
    u[3] = u3;
    C1length = fast_expansion_sum_zeroelim(4, B, 4, u, C1);

    Two_Product(acx, bcytail, s1, s0);
    Two_Product(acy, bcxtail, t1, t0);
    Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
    u[3] = u3;
    C2length = fast_expansion_sum_zeroelim(C1length, C1, 4, u, C2);

    Two_Product(acxtail, bcytail, s1, s0);
    Two_Product(acytail, bcxtail, t1, t0);
    Two_Two_Diff(s1, s0, t1, t0, u3, u[2], u[1], u[0]);
    u[3] = u3;
    Dlength = fast_expansion_sum_zeroelim(C2length, C2, 4, u, D);

    return (D[Dlength - 1]);
}

REAL orient2d(pa, pb, pc)
    REAL* pa;
REAL* pb;
REAL* pc;
{
    REAL detleft, detright, det;
    REAL detsum, errbound;

    detleft = (pa[0] - pc[0]) * (pb[1] - pc[1]);
    detright = (pa[1] - pc[1]) * (pb[0] - pc[0]);
    det = detleft - detright;

    if (detleft > 0.0) {
        if (detright <= 0.0) {
            return det;
        } else {
            detsum = detleft + detright;
        }
    } else if (detleft < 0.0) {
        if (detright >= 0.0) {
            return det;
        } else {
            detsum = -detleft - detright;
        }
    } else {
        return det;
    }

    errbound = ccwerrboundA * detsum;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    return orient2dadapt(pa, pb, pc, detsum);
}

/*****************************************************************************/
/*                                                                           */
/*  orient3dfast()   Approximate 3D orientation test.  Nonrobust.            */
/*  orient3dexact()   Exact 3D orientation test.  Robust.                    */
/*  orient3dslow()   Another exact 3D orientation test.  Robust.             */
/*  orient3d()   Adaptive exact 3D orientation test.  Robust.                */
/*                                                                           */
/*               Return a positive value if the point pd lies below the      */
/*               plane passing through pa, pb, and pc; "below" is defined so */
/*               that pa, pb, and pc appear in counterclockwise order when   */
/*               viewed from above the plane.  Returns a negative value if   */
/*               pd lies above the plane.  Returns zero if the points are    */
/*               coplanar.  The result is also a rough approximation of six  */
/*               times the signed volume of the tetrahedron defined by the   */
/*               four points.                                                */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In orient3d() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, orient3d() is usually quite */
/*  fast, but will run more slowly when the input points are coplanar or     */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL orient3dfast(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    REAL adx, bdx, cdx;
    REAL ady, bdy, cdy;
    REAL adz, bdz, cdz;

    adx = pa[0] - pd[0];
    bdx = pb[0] - pd[0];
    cdx = pc[0] - pd[0];
    ady = pa[1] - pd[1];
    bdy = pb[1] - pd[1];
    cdy = pc[1] - pd[1];
    adz = pa[2] - pd[2];
    bdz = pb[2] - pd[2];
    cdz = pc[2] - pd[2];

    return adx * (bdy * cdz - bdz * cdy)
        + bdx * (cdy * adz - cdz * ady)
        + cdx * (ady * bdz - adz * bdy);
}

REAL orient3dexact(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    INEXACT REAL axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
    INEXACT REAL bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
    REAL axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
    REAL bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
    REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
    REAL temp8[8];
    int templen;
    REAL abc[12], bcd[12], cda[12], dab[12];
    int abclen, bcdlen, cdalen, dablen;
    REAL adet[24], bdet[24], cdet[24], ddet[24];
    int alen, blen, clen, dlen;
    REAL abdet[48], cddet[48];
    int ablen, cdlen;
    REAL deter[96];
    int deterlen;
    int i;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    Two_Product(pa[0], pb[1], axby1, axby0);
    Two_Product(pb[0], pa[1], bxay1, bxay0);
    Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

    Two_Product(pb[0], pc[1], bxcy1, bxcy0);
    Two_Product(pc[0], pb[1], cxby1, cxby0);
    Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

    Two_Product(pc[0], pd[1], cxdy1, cxdy0);
    Two_Product(pd[0], pc[1], dxcy1, dxcy0);
    Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

    Two_Product(pd[0], pa[1], dxay1, dxay0);
    Two_Product(pa[0], pd[1], axdy1, axdy0);
    Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

    Two_Product(pa[0], pc[1], axcy1, axcy0);
    Two_Product(pc[0], pa[1], cxay1, cxay0);
    Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

    Two_Product(pb[0], pd[1], bxdy1, bxdy0);
    Two_Product(pd[0], pb[1], dxby1, dxby0);
    Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

    templen = fast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
    cdalen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
    templen = fast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
    dablen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);
    for (i = 0; i < 4; i++) {
        bd[i] = -bd[i];
        ac[i] = -ac[i];
    }
    templen = fast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
    abclen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
    templen = fast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
    bcdlen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

    alen = scale_expansion_zeroelim(bcdlen, bcd, pa[2], adet);
    blen = scale_expansion_zeroelim(cdalen, cda, -pb[2], bdet);
    clen = scale_expansion_zeroelim(dablen, dab, pc[2], cdet);
    dlen = scale_expansion_zeroelim(abclen, abc, -pd[2], ddet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

    return deter[deterlen - 1];
}

REAL orient3dslow(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    INEXACT REAL adx, ady, adz, bdx, bdy, bdz, cdx, cdy, cdz;
    REAL adxtail, adytail, adztail;
    REAL bdxtail, bdytail, bdztail;
    REAL cdxtail, cdytail, cdztail;
    REAL negate, negatetail;
    INEXACT REAL axby7, bxcy7, axcy7, bxay7, cxby7, cxay7;
    REAL axby[8], bxcy[8], axcy[8], bxay[8], cxby[8], cxay[8];
    REAL temp16[16], temp32[32], temp32t[32];
    int temp16len, temp32len, temp32tlen;
    REAL adet[64], bdet[64], cdet[64];
    int alen, blen, clen;
    REAL abdet[128];
    int ablen;
    REAL deter[192];
    int deterlen;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j, _k, _l, _m, _n;
    REAL _0, _1, _2;

    Two_Diff(pa[0], pd[0], adx, adxtail);
    Two_Diff(pa[1], pd[1], ady, adytail);
    Two_Diff(pa[2], pd[2], adz, adztail);
    Two_Diff(pb[0], pd[0], bdx, bdxtail);
    Two_Diff(pb[1], pd[1], bdy, bdytail);
    Two_Diff(pb[2], pd[2], bdz, bdztail);
    Two_Diff(pc[0], pd[0], cdx, cdxtail);
    Two_Diff(pc[1], pd[1], cdy, cdytail);
    Two_Diff(pc[2], pd[2], cdz, cdztail);

    Two_Two_Product(adx, adxtail, bdy, bdytail,
        axby7, axby[6], axby[5], axby[4],
        axby[3], axby[2], axby[1], axby[0]);
    axby[7] = axby7;
    negate = -ady;
    negatetail = -adytail;
    Two_Two_Product(bdx, bdxtail, negate, negatetail,
        bxay7, bxay[6], bxay[5], bxay[4],
        bxay[3], bxay[2], bxay[1], bxay[0]);
    bxay[7] = bxay7;
    Two_Two_Product(bdx, bdxtail, cdy, cdytail,
        bxcy7, bxcy[6], bxcy[5], bxcy[4],
        bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
    bxcy[7] = bxcy7;
    negate = -bdy;
    negatetail = -bdytail;
    Two_Two_Product(cdx, cdxtail, negate, negatetail,
        cxby7, cxby[6], cxby[5], cxby[4],
        cxby[3], cxby[2], cxby[1], cxby[0]);
    cxby[7] = cxby7;
    Two_Two_Product(cdx, cdxtail, ady, adytail,
        cxay7, cxay[6], cxay[5], cxay[4],
        cxay[3], cxay[2], cxay[1], cxay[0]);
    cxay[7] = cxay7;
    negate = -cdy;
    negatetail = -cdytail;
    Two_Two_Product(adx, adxtail, negate, negatetail,
        axcy7, axcy[6], axcy[5], axcy[4],
        axcy[3], axcy[2], axcy[1], axcy[0]);
    axcy[7] = axcy7;

    temp16len = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, temp16);
    temp32len = scale_expansion_zeroelim(temp16len, temp16, adz, temp32);
    temp32tlen = scale_expansion_zeroelim(temp16len, temp16, adztail, temp32t);
    alen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
        adet);

    temp16len = fast_expansion_sum_zeroelim(8, cxay, 8, axcy, temp16);
    temp32len = scale_expansion_zeroelim(temp16len, temp16, bdz, temp32);
    temp32tlen = scale_expansion_zeroelim(temp16len, temp16, bdztail, temp32t);
    blen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
        bdet);

    temp16len = fast_expansion_sum_zeroelim(8, axby, 8, bxay, temp16);
    temp32len = scale_expansion_zeroelim(temp16len, temp16, cdz, temp32);
    temp32tlen = scale_expansion_zeroelim(temp16len, temp16, cdztail, temp32t);
    clen = fast_expansion_sum_zeroelim(temp32len, temp32, temp32tlen, temp32t,
        cdet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, deter);

    return deter[deterlen - 1];
}

REAL orient3dadapt(pa, pb, pc, pd, permanent)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL permanent;
{
    INEXACT REAL adx, bdx, cdx, ady, bdy, cdy, adz, bdz, cdz;
    REAL det, errbound;

    INEXACT REAL bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
    REAL bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
    REAL bc[4], ca[4], ab[4];
    INEXACT REAL bc3, ca3, ab3;
    REAL adet[8], bdet[8], cdet[8];
    int alen, blen, clen;
    REAL abdet[16];
    int ablen;
    REAL *finnow, *finother, *finswap;
    REAL fin1[192], fin2[192];
    int finlength;

    REAL adxtail, bdxtail, cdxtail;
    REAL adytail, bdytail, cdytail;
    REAL adztail, bdztail, cdztail;
    INEXACT REAL at_blarge, at_clarge;
    INEXACT REAL bt_clarge, bt_alarge;
    INEXACT REAL ct_alarge, ct_blarge;
    REAL at_b[4], at_c[4], bt_c[4], bt_a[4], ct_a[4], ct_b[4];
    int at_blen, at_clen, bt_clen, bt_alen, ct_alen, ct_blen;
    INEXACT REAL bdxt_cdy1, cdxt_bdy1, cdxt_ady1;
    INEXACT REAL adxt_cdy1, adxt_bdy1, bdxt_ady1;
    REAL bdxt_cdy0, cdxt_bdy0, cdxt_ady0;
    REAL adxt_cdy0, adxt_bdy0, bdxt_ady0;
    INEXACT REAL bdyt_cdx1, cdyt_bdx1, cdyt_adx1;
    INEXACT REAL adyt_cdx1, adyt_bdx1, bdyt_adx1;
    REAL bdyt_cdx0, cdyt_bdx0, cdyt_adx0;
    REAL adyt_cdx0, adyt_bdx0, bdyt_adx0;
    REAL bct[8], cat[8], abt[8];
    int bctlen, catlen, abtlen;
    INEXACT REAL bdxt_cdyt1, cdxt_bdyt1, cdxt_adyt1;
    INEXACT REAL adxt_cdyt1, adxt_bdyt1, bdxt_adyt1;
    REAL bdxt_cdyt0, cdxt_bdyt0, cdxt_adyt0;
    REAL adxt_cdyt0, adxt_bdyt0, bdxt_adyt0;
    REAL u[4], v[12], w[16];
    INEXACT REAL u3;
    int vlength, wlength;
    REAL negate;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j, _k;
    REAL _0;

    adx = (REAL)(pa[0] - pd[0]);
    bdx = (REAL)(pb[0] - pd[0]);
    cdx = (REAL)(pc[0] - pd[0]);
    ady = (REAL)(pa[1] - pd[1]);
    bdy = (REAL)(pb[1] - pd[1]);
    cdy = (REAL)(pc[1] - pd[1]);
    adz = (REAL)(pa[2] - pd[2]);
    bdz = (REAL)(pb[2] - pd[2]);
    cdz = (REAL)(pc[2] - pd[2]);

    Two_Product(bdx, cdy, bdxcdy1, bdxcdy0);
    Two_Product(cdx, bdy, cdxbdy1, cdxbdy0);
    Two_Two_Diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
    bc[3] = bc3;
    alen = scale_expansion_zeroelim(4, bc, adz, adet);

    Two_Product(cdx, ady, cdxady1, cdxady0);
    Two_Product(adx, cdy, adxcdy1, adxcdy0);
    Two_Two_Diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
    ca[3] = ca3;
    blen = scale_expansion_zeroelim(4, ca, bdz, bdet);

    Two_Product(adx, bdy, adxbdy1, adxbdy0);
    Two_Product(bdx, ady, bdxady1, bdxady0);
    Two_Two_Diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
    ab[3] = ab3;
    clen = scale_expansion_zeroelim(4, ab, cdz, cdet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

    det = estimate(finlength, fin1);
    errbound = o3derrboundB * permanent;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    Two_Diff_Tail(pa[0], pd[0], adx, adxtail);
    Two_Diff_Tail(pb[0], pd[0], bdx, bdxtail);
    Two_Diff_Tail(pc[0], pd[0], cdx, cdxtail);
    Two_Diff_Tail(pa[1], pd[1], ady, adytail);
    Two_Diff_Tail(pb[1], pd[1], bdy, bdytail);
    Two_Diff_Tail(pc[1], pd[1], cdy, cdytail);
    Two_Diff_Tail(pa[2], pd[2], adz, adztail);
    Two_Diff_Tail(pb[2], pd[2], bdz, bdztail);
    Two_Diff_Tail(pc[2], pd[2], cdz, cdztail);

    if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
        && (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)
        && (adztail == 0.0) && (bdztail == 0.0) && (cdztail == 0.0)) {
        return det;
    }

    errbound = o3derrboundC * permanent + resulterrbound * Absolute(det);
    det += (adz * ((bdx * cdytail + cdy * bdxtail) - (bdy * cdxtail + cdx * bdytail))
               + adztail * (bdx * cdy - bdy * cdx))
        + (bdz * ((cdx * adytail + ady * cdxtail) - (cdy * adxtail + adx * cdytail))
            + bdztail * (cdx * ady - cdy * adx))
        + (cdz * ((adx * bdytail + bdy * adxtail) - (ady * bdxtail + bdx * adytail))
            + cdztail * (adx * bdy - ady * bdx));
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    finnow = fin1;
    finother = fin2;

    if (adxtail == 0.0) {
        if (adytail == 0.0) {
            at_b[0] = 0.0;
            at_blen = 1;
            at_c[0] = 0.0;
            at_clen = 1;
        } else {
            negate = -adytail;
            Two_Product(negate, bdx, at_blarge, at_b[0]);
            at_b[1] = at_blarge;
            at_blen = 2;
            Two_Product(adytail, cdx, at_clarge, at_c[0]);
            at_c[1] = at_clarge;
            at_clen = 2;
        }
    } else {
        if (adytail == 0.0) {
            Two_Product(adxtail, bdy, at_blarge, at_b[0]);
            at_b[1] = at_blarge;
            at_blen = 2;
            negate = -adxtail;
            Two_Product(negate, cdy, at_clarge, at_c[0]);
            at_c[1] = at_clarge;
            at_clen = 2;
        } else {
            Two_Product(adxtail, bdy, adxt_bdy1, adxt_bdy0);
            Two_Product(adytail, bdx, adyt_bdx1, adyt_bdx0);
            Two_Two_Diff(adxt_bdy1, adxt_bdy0, adyt_bdx1, adyt_bdx0,
                at_blarge, at_b[2], at_b[1], at_b[0]);
            at_b[3] = at_blarge;
            at_blen = 4;
            Two_Product(adytail, cdx, adyt_cdx1, adyt_cdx0);
            Two_Product(adxtail, cdy, adxt_cdy1, adxt_cdy0);
            Two_Two_Diff(adyt_cdx1, adyt_cdx0, adxt_cdy1, adxt_cdy0,
                at_clarge, at_c[2], at_c[1], at_c[0]);
            at_c[3] = at_clarge;
            at_clen = 4;
        }
    }
    if (bdxtail == 0.0) {
        if (bdytail == 0.0) {
            bt_c[0] = 0.0;
            bt_clen = 1;
            bt_a[0] = 0.0;
            bt_alen = 1;
        } else {
            negate = -bdytail;
            Two_Product(negate, cdx, bt_clarge, bt_c[0]);
            bt_c[1] = bt_clarge;
            bt_clen = 2;
            Two_Product(bdytail, adx, bt_alarge, bt_a[0]);
            bt_a[1] = bt_alarge;
            bt_alen = 2;
        }
    } else {
        if (bdytail == 0.0) {
            Two_Product(bdxtail, cdy, bt_clarge, bt_c[0]);
            bt_c[1] = bt_clarge;
            bt_clen = 2;
            negate = -bdxtail;
            Two_Product(negate, ady, bt_alarge, bt_a[0]);
            bt_a[1] = bt_alarge;
            bt_alen = 2;
        } else {
            Two_Product(bdxtail, cdy, bdxt_cdy1, bdxt_cdy0);
            Two_Product(bdytail, cdx, bdyt_cdx1, bdyt_cdx0);
            Two_Two_Diff(bdxt_cdy1, bdxt_cdy0, bdyt_cdx1, bdyt_cdx0,
                bt_clarge, bt_c[2], bt_c[1], bt_c[0]);
            bt_c[3] = bt_clarge;
            bt_clen = 4;
            Two_Product(bdytail, adx, bdyt_adx1, bdyt_adx0);
            Two_Product(bdxtail, ady, bdxt_ady1, bdxt_ady0);
            Two_Two_Diff(bdyt_adx1, bdyt_adx0, bdxt_ady1, bdxt_ady0,
                bt_alarge, bt_a[2], bt_a[1], bt_a[0]);
            bt_a[3] = bt_alarge;
            bt_alen = 4;
        }
    }
    if (cdxtail == 0.0) {
        if (cdytail == 0.0) {
            ct_a[0] = 0.0;
            ct_alen = 1;
            ct_b[0] = 0.0;
            ct_blen = 1;
        } else {
            negate = -cdytail;
            Two_Product(negate, adx, ct_alarge, ct_a[0]);
            ct_a[1] = ct_alarge;
            ct_alen = 2;
            Two_Product(cdytail, bdx, ct_blarge, ct_b[0]);
            ct_b[1] = ct_blarge;
            ct_blen = 2;
        }
    } else {
        if (cdytail == 0.0) {
            Two_Product(cdxtail, ady, ct_alarge, ct_a[0]);
            ct_a[1] = ct_alarge;
            ct_alen = 2;
            negate = -cdxtail;
            Two_Product(negate, bdy, ct_blarge, ct_b[0]);
            ct_b[1] = ct_blarge;
            ct_blen = 2;
        } else {
            Two_Product(cdxtail, ady, cdxt_ady1, cdxt_ady0);
            Two_Product(cdytail, adx, cdyt_adx1, cdyt_adx0);
            Two_Two_Diff(cdxt_ady1, cdxt_ady0, cdyt_adx1, cdyt_adx0,
                ct_alarge, ct_a[2], ct_a[1], ct_a[0]);
            ct_a[3] = ct_alarge;
            ct_alen = 4;
            Two_Product(cdytail, bdx, cdyt_bdx1, cdyt_bdx0);
            Two_Product(cdxtail, bdy, cdxt_bdy1, cdxt_bdy0);
            Two_Two_Diff(cdyt_bdx1, cdyt_bdx0, cdxt_bdy1, cdxt_bdy0,
                ct_blarge, ct_b[2], ct_b[1], ct_b[0]);
            ct_b[3] = ct_blarge;
            ct_blen = 4;
        }
    }

    bctlen = fast_expansion_sum_zeroelim(bt_clen, bt_c, ct_blen, ct_b, bct);
    wlength = scale_expansion_zeroelim(bctlen, bct, adz, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
        finother);
    finswap = finnow;
    finnow = finother;
    finother = finswap;

    catlen = fast_expansion_sum_zeroelim(ct_alen, ct_a, at_clen, at_c, cat);
    wlength = scale_expansion_zeroelim(catlen, cat, bdz, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
        finother);
    finswap = finnow;
    finnow = finother;
    finother = finswap;

    abtlen = fast_expansion_sum_zeroelim(at_blen, at_b, bt_alen, bt_a, abt);
    wlength = scale_expansion_zeroelim(abtlen, abt, cdz, w);
    finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
        finother);
    finswap = finnow;
    finnow = finother;
    finother = finswap;

    if (adztail != 0.0) {
        vlength = scale_expansion_zeroelim(4, bc, adztail, v);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (bdztail != 0.0) {
        vlength = scale_expansion_zeroelim(4, ca, bdztail, v);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (cdztail != 0.0) {
        vlength = scale_expansion_zeroelim(4, ab, cdztail, v);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, vlength, v,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }

    if (adxtail != 0.0) {
        if (bdytail != 0.0) {
            Two_Product(adxtail, bdytail, adxt_bdyt1, adxt_bdyt0);
            Two_One_Product(adxt_bdyt1, adxt_bdyt0, cdz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (cdztail != 0.0) {
                Two_One_Product(adxt_bdyt1, adxt_bdyt0, cdztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
        if (cdytail != 0.0) {
            negate = -adxtail;
            Two_Product(negate, cdytail, adxt_cdyt1, adxt_cdyt0);
            Two_One_Product(adxt_cdyt1, adxt_cdyt0, bdz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (bdztail != 0.0) {
                Two_One_Product(adxt_cdyt1, adxt_cdyt0, bdztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
    }
    if (bdxtail != 0.0) {
        if (cdytail != 0.0) {
            Two_Product(bdxtail, cdytail, bdxt_cdyt1, bdxt_cdyt0);
            Two_One_Product(bdxt_cdyt1, bdxt_cdyt0, adz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (adztail != 0.0) {
                Two_One_Product(bdxt_cdyt1, bdxt_cdyt0, adztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
        if (adytail != 0.0) {
            negate = -bdxtail;
            Two_Product(negate, adytail, bdxt_adyt1, bdxt_adyt0);
            Two_One_Product(bdxt_adyt1, bdxt_adyt0, cdz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (cdztail != 0.0) {
                Two_One_Product(bdxt_adyt1, bdxt_adyt0, cdztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
    }
    if (cdxtail != 0.0) {
        if (adytail != 0.0) {
            Two_Product(cdxtail, adytail, cdxt_adyt1, cdxt_adyt0);
            Two_One_Product(cdxt_adyt1, cdxt_adyt0, bdz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (bdztail != 0.0) {
                Two_One_Product(cdxt_adyt1, cdxt_adyt0, bdztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
        if (bdytail != 0.0) {
            negate = -cdxtail;
            Two_Product(negate, bdytail, cdxt_bdyt1, cdxt_bdyt0);
            Two_One_Product(cdxt_bdyt1, cdxt_bdyt0, adz, u3, u[2], u[1], u[0]);
            u[3] = u3;
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (adztail != 0.0) {
                Two_One_Product(cdxt_bdyt1, cdxt_bdyt0, adztail, u3, u[2], u[1], u[0]);
                u[3] = u3;
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, 4, u,
                    finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
        }
    }

    if (adztail != 0.0) {
        wlength = scale_expansion_zeroelim(bctlen, bct, adztail, w);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (bdztail != 0.0) {
        wlength = scale_expansion_zeroelim(catlen, cat, bdztail, w);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (cdztail != 0.0) {
        wlength = scale_expansion_zeroelim(abtlen, abt, cdztail, w);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, wlength, w,
            finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }

    return finnow[finlength - 1];
}
 
REAL orient3d(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    REAL adx, bdx, cdx, ady, bdy, cdy, adz, bdz, cdz;
    REAL bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
    REAL det;
    REAL permanent, errbound;

    adx = pa[0] - pd[0];
    bdx = pb[0] - pd[0];
    cdx = pc[0] - pd[0];
    ady = pa[1] - pd[1];
    bdy = pb[1] - pd[1];
    cdy = pc[1] - pd[1];
    adz = pa[2] - pd[2];
    bdz = pb[2] - pd[2];
    cdz = pc[2] - pd[2];

    bdxcdy = bdx * cdy;
    cdxbdy = cdx * bdy;

    cdxady = cdx * ady;
    adxcdy = adx * cdy;

    adxbdy = adx * bdy;
    bdxady = bdx * ady;

    det = adz * (bdxcdy - cdxbdy)
        + bdz * (cdxady - adxcdy)
        + cdz * (adxbdy - bdxady);

    permanent = (Absolute(bdxcdy) + Absolute(cdxbdy)) * Absolute(adz)
        + (Absolute(cdxady) + Absolute(adxcdy)) * Absolute(bdz)
        + (Absolute(adxbdy) + Absolute(bdxady)) * Absolute(cdz);
    errbound = o3derrboundA * permanent;
    if ((det > errbound) || (-det > errbound)) {
        return det;
    }

    return orient3dadapt(pa, pb, pc, pd, permanent);
}

#ifdef _WIN32
#pragma warning(pop) // #pragma warning(disable: 4131)
#endif // _WIN32

#if 0
/*****************************************************************************/
/*                                                                           */
/*  incirclefast()   Approximate 2D incircle test.  Nonrobust.               */
/*  incircleexact()   Exact 2D incircle test.  Robust.                       */
/*  incircleslow()   Another exact 2D incircle test.  Robust.                */
/*  incircle()   Adaptive exact 2D incircle test.  Robust.                   */
/*                                                                           */
/*               Return a positive value if the point pd lies inside the     */
/*               circle passing through pa, pb, and pc; a negative value if  */
/*               it lies outside; and zero if the four points are cocircular.*/
/*               The points pa, pb, and pc must be in counterclockwise       */
/*               order, or the sign of the result will be reversed.          */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In incircle() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, incircle() is usually quite */
/*  fast, but will run more slowly when the input points are cocircular or   */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL incirclefast(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    REAL adx, ady, bdx, bdy, cdx, cdy;
    REAL abdet, bcdet, cadet;
    REAL alift, blift, clift;

    adx = pa[0] - pd[0];
    ady = pa[1] - pd[1];
    bdx = pb[0] - pd[0];
    bdy = pb[1] - pd[1];
    cdx = pc[0] - pd[0];
    cdy = pc[1] - pd[1];

    abdet = adx * bdy - bdx * ady;
    bcdet = bdx * cdy - cdx * bdy;
    cadet = cdx * ady - adx * cdy;
    alift = adx * adx + ady * ady;
    blift = bdx * bdx + bdy * bdy;
    clift = cdx * cdx + cdy * cdy;

    return alift * bcdet + blift * cadet + clift * abdet;
}

REAL incircleexact(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    INEXACT REAL axby1, bxcy1, cxdy1, dxay1, axcy1, bxdy1;
    INEXACT REAL bxay1, cxby1, dxcy1, axdy1, cxay1, dxby1;
    REAL axby0, bxcy0, cxdy0, dxay0, axcy0, bxdy0;
    REAL bxay0, cxby0, dxcy0, axdy0, cxay0, dxby0;
    REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
    REAL temp8[8];
    int templen;
    REAL abc[12], bcd[12], cda[12], dab[12];
    int abclen, bcdlen, cdalen, dablen;
    REAL det24x[24], det24y[24], det48x[48], det48y[48];
    int xlen, ylen;
    REAL adet[96], bdet[96], cdet[96], ddet[96];
    int alen, blen, clen, dlen;
    REAL abdet[192], cddet[192];
    int ablen, cdlen;
    REAL deter[384];
    int deterlen;
    int i;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    Two_Product(pa[0], pb[1], axby1, axby0);
    Two_Product(pb[0], pa[1], bxay1, bxay0);
    Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

    Two_Product(pb[0], pc[1], bxcy1, bxcy0);
    Two_Product(pc[0], pb[1], cxby1, cxby0);
    Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

    Two_Product(pc[0], pd[1], cxdy1, cxdy0);
    Two_Product(pd[0], pc[1], dxcy1, dxcy0);
    Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

    Two_Product(pd[0], pa[1], dxay1, dxay0);
    Two_Product(pa[0], pd[1], axdy1, axdy0);
    Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

    Two_Product(pa[0], pc[1], axcy1, axcy0);
    Two_Product(pc[0], pa[1], cxay1, cxay0);
    Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

    Two_Product(pb[0], pd[1], bxdy1, bxdy0);
    Two_Product(pd[0], pb[1], dxby1, dxby0);
    Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

    templen = fast_expansion_sum_zeroelim(4, cd, 4, da, temp8);
    cdalen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, cda);
    templen = fast_expansion_sum_zeroelim(4, da, 4, ab, temp8);
    dablen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, dab);
    for (i = 0; i < 4; i++) {
        bd[i] = -bd[i];
        ac[i] = -ac[i];
    }
    templen = fast_expansion_sum_zeroelim(4, ab, 4, bc, temp8);
    abclen = fast_expansion_sum_zeroelim(templen, temp8, 4, ac, abc);
    templen = fast_expansion_sum_zeroelim(4, bc, 4, cd, temp8);
    bcdlen = fast_expansion_sum_zeroelim(templen, temp8, 4, bd, bcd);

    xlen = scale_expansion_zeroelim(bcdlen, bcd, pa[0], det24x);
    xlen = scale_expansion_zeroelim(xlen, det24x, pa[0], det48x);
    ylen = scale_expansion_zeroelim(bcdlen, bcd, pa[1], det24y);
    ylen = scale_expansion_zeroelim(ylen, det24y, pa[1], det48y);
    alen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, adet);

    xlen = scale_expansion_zeroelim(cdalen, cda, pb[0], det24x);
    xlen = scale_expansion_zeroelim(xlen, det24x, -pb[0], det48x);
    ylen = scale_expansion_zeroelim(cdalen, cda, pb[1], det24y);
    ylen = scale_expansion_zeroelim(ylen, det24y, -pb[1], det48y);
    blen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, bdet);

    xlen = scale_expansion_zeroelim(dablen, dab, pc[0], det24x);
    xlen = scale_expansion_zeroelim(xlen, det24x, pc[0], det48x);
    ylen = scale_expansion_zeroelim(dablen, dab, pc[1], det24y);
    ylen = scale_expansion_zeroelim(ylen, det24y, pc[1], det48y);
    clen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, cdet);

    xlen = scale_expansion_zeroelim(abclen, abc, pd[0], det24x);
    xlen = scale_expansion_zeroelim(xlen, det24x, -pd[0], det48x);
    ylen = scale_expansion_zeroelim(abclen, abc, pd[1], det24y);
    ylen = scale_expansion_zeroelim(ylen, det24y, -pd[1], det48y);
    dlen = fast_expansion_sum_zeroelim(xlen, det48x, ylen, det48y, ddet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

    return deter[deterlen - 1];
}

REAL incircleslow(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    INEXACT REAL adx, bdx, cdx, ady, bdy, cdy;
    REAL adxtail, bdxtail, cdxtail;
    REAL adytail, bdytail, cdytail;
    REAL negate, negatetail;
    INEXACT REAL axby7, bxcy7, axcy7, bxay7, cxby7, cxay7;
    REAL axby[8], bxcy[8], axcy[8], bxay[8], cxby[8], cxay[8];
    REAL temp16[16];
    int temp16len;
    REAL detx[32], detxx[64], detxt[32], detxxt[64], detxtxt[64];
    int xlen, xxlen, xtlen, xxtlen, xtxtlen;
    REAL x1[128], x2[192];
    int x1len, x2len;
    REAL dety[32], detyy[64], detyt[32], detyyt[64], detytyt[64];
    int ylen, yylen, ytlen, yytlen, ytytlen;
    REAL y1[128], y2[192];
    int y1len, y2len;
    REAL adet[384], bdet[384], cdet[384], abdet[768], deter[1152];
    int alen, blen, clen, ablen, deterlen;
    int i;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j, _k, _l, _m, _n;
    REAL _0, _1, _2;

    Two_Diff(pa[0], pd[0], adx, adxtail);
    Two_Diff(pa[1], pd[1], ady, adytail);
    Two_Diff(pb[0], pd[0], bdx, bdxtail);
    Two_Diff(pb[1], pd[1], bdy, bdytail);
    Two_Diff(pc[0], pd[0], cdx, cdxtail);
    Two_Diff(pc[1], pd[1], cdy, cdytail);

    Two_Two_Product(adx, adxtail, bdy, bdytail,
        axby7, axby[6], axby[5], axby[4],
        axby[3], axby[2], axby[1], axby[0]);
    axby[7] = axby7;
    negate = -ady;
    negatetail = -adytail;
    Two_Two_Product(bdx, bdxtail, negate, negatetail,
        bxay7, bxay[6], bxay[5], bxay[4],
        bxay[3], bxay[2], bxay[1], bxay[0]);
    bxay[7] = bxay7;
    Two_Two_Product(bdx, bdxtail, cdy, cdytail,
        bxcy7, bxcy[6], bxcy[5], bxcy[4],
        bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
    bxcy[7] = bxcy7;
    negate = -bdy;
    negatetail = -bdytail;
    Two_Two_Product(cdx, cdxtail, negate, negatetail,
        cxby7, cxby[6], cxby[5], cxby[4],
        cxby[3], cxby[2], cxby[1], cxby[0]);
    cxby[7] = cxby7;
    Two_Two_Product(cdx, cdxtail, ady, adytail,
        cxay7, cxay[6], cxay[5], cxay[4],
        cxay[3], cxay[2], cxay[1], cxay[0]);
    cxay[7] = cxay7;
    negate = -cdy;
    negatetail = -cdytail;
    Two_Two_Product(adx, adxtail, negate, negatetail,
        axcy7, axcy[6], axcy[5], axcy[4],
        axcy[3], axcy[2], axcy[1], axcy[0]);
    axcy[7] = axcy7;

    temp16len = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, temp16);

    xlen = scale_expansion_zeroelim(temp16len, temp16, adx, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, adx, detxx);
    xtlen = scale_expansion_zeroelim(temp16len, temp16, adxtail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, adx, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, adxtail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

    ylen = scale_expansion_zeroelim(temp16len, temp16, ady, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, ady, detyy);
    ytlen = scale_expansion_zeroelim(temp16len, temp16, adytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, ady, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, adytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

    alen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, adet);

    temp16len = fast_expansion_sum_zeroelim(8, cxay, 8, axcy, temp16);

    xlen = scale_expansion_zeroelim(temp16len, temp16, bdx, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, bdx, detxx);
    xtlen = scale_expansion_zeroelim(temp16len, temp16, bdxtail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, bdx, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, bdxtail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

    ylen = scale_expansion_zeroelim(temp16len, temp16, bdy, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, bdy, detyy);
    ytlen = scale_expansion_zeroelim(temp16len, temp16, bdytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, bdy, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, bdytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

    blen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, bdet);

    temp16len = fast_expansion_sum_zeroelim(8, axby, 8, bxay, temp16);

    xlen = scale_expansion_zeroelim(temp16len, temp16, cdx, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, cdx, detxx);
    xtlen = scale_expansion_zeroelim(temp16len, temp16, cdxtail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, cdx, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, cdxtail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);

    ylen = scale_expansion_zeroelim(temp16len, temp16, cdy, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, cdy, detyy);
    ytlen = scale_expansion_zeroelim(temp16len, temp16, cdytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, cdy, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, cdytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);

    clen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, cdet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, deter);

    return deter[deterlen - 1];
}

REAL incircleadapt(pa, pb, pc, pd, permanent)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL permanent;
{
    INEXACT REAL adx, bdx, cdx, ady, bdy, cdy;
    REAL det, errbound;

    INEXACT REAL bdxcdy1, cdxbdy1, cdxady1, adxcdy1, adxbdy1, bdxady1;
    REAL bdxcdy0, cdxbdy0, cdxady0, adxcdy0, adxbdy0, bdxady0;
    REAL bc[4], ca[4], ab[4];
    INEXACT REAL bc3, ca3, ab3;
    REAL axbc[8], axxbc[16], aybc[8], ayybc[16], adet[32];
    int axbclen, axxbclen, aybclen, ayybclen, alen;
    REAL bxca[8], bxxca[16], byca[8], byyca[16], bdet[32];
    int bxcalen, bxxcalen, bycalen, byycalen, blen;
    REAL cxab[8], cxxab[16], cyab[8], cyyab[16], cdet[32];
    int cxablen, cxxablen, cyablen, cyyablen, clen;
    REAL abdet[64];
    int ablen;
    REAL fin1[1152], fin2[1152];
    REAL *finnow, *finother, *finswap;
    int finlength;

    REAL adxtail, bdxtail, cdxtail, adytail, bdytail, cdytail;
    INEXACT REAL adxadx1, adyady1, bdxbdx1, bdybdy1, cdxcdx1, cdycdy1;
    REAL adxadx0, adyady0, bdxbdx0, bdybdy0, cdxcdx0, cdycdy0;
    REAL aa[4], bb[4], cc[4];
    INEXACT REAL aa3, bb3, cc3;
    INEXACT REAL ti1, tj1;
    REAL ti0, tj0;
    REAL u[4], v[4];
    INEXACT REAL u3, v3;
    REAL temp8[8], temp16a[16], temp16b[16], temp16c[16];
    REAL temp32a[32], temp32b[32], temp48[48], temp64[64];
    int temp8len, temp16alen, temp16blen, temp16clen;
    int temp32alen, temp32blen, temp48len, temp64len;
    REAL axtbb[8], axtcc[8], aytbb[8], aytcc[8];
    int axtbblen, axtcclen, aytbblen, aytcclen;
    REAL bxtaa[8], bxtcc[8], bytaa[8], bytcc[8];
    int bxtaalen, bxtcclen, bytaalen, bytcclen;
    REAL cxtaa[8], cxtbb[8], cytaa[8], cytbb[8];
    int cxtaalen, cxtbblen, cytaalen, cytbblen;
    REAL axtbc[8], aytbc[8], bxtca[8], bytca[8], cxtab[8], cytab[8];
    int axtbclen, aytbclen, bxtcalen, bytcalen, cxtablen, cytablen;
    REAL axtbct[16], aytbct[16], bxtcat[16], bytcat[16], cxtabt[16], cytabt[16];
    int axtbctlen, aytbctlen, bxtcatlen, bytcatlen, cxtabtlen, cytabtlen;
    REAL axtbctt[8], aytbctt[8], bxtcatt[8];
    REAL bytcatt[8], cxtabtt[8], cytabtt[8];
    int axtbcttlen, aytbcttlen, bxtcattlen, bytcattlen, cxtabttlen, cytabttlen;
    REAL abt[8], bct[8], cat[8];
    int abtlen, bctlen, catlen;
    REAL abtt[4], bctt[4], catt[4];
    int abttlen, bcttlen, cattlen;
    INEXACT REAL abtt3, bctt3, catt3;
    REAL negate;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    adx = (REAL)(pa[0] - pd[0]);
    bdx = (REAL)(pb[0] - pd[0]);
    cdx = (REAL)(pc[0] - pd[0]);
    ady = (REAL)(pa[1] - pd[1]);
    bdy = (REAL)(pb[1] - pd[1]);
    cdy = (REAL)(pc[1] - pd[1]);

    Two_Product(bdx, cdy, bdxcdy1, bdxcdy0);
    Two_Product(cdx, bdy, cdxbdy1, cdxbdy0);
    Two_Two_Diff(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc3, bc[2], bc[1], bc[0]);
    bc[3] = bc3;
    axbclen = scale_expansion_zeroelim(4, bc, adx, axbc);
    axxbclen = scale_expansion_zeroelim(axbclen, axbc, adx, axxbc);
    aybclen = scale_expansion_zeroelim(4, bc, ady, aybc);
    ayybclen = scale_expansion_zeroelim(aybclen, aybc, ady, ayybc);
    alen = fast_expansion_sum_zeroelim(axxbclen, axxbc, ayybclen, ayybc, adet);

    Two_Product(cdx, ady, cdxady1, cdxady0);
    Two_Product(adx, cdy, adxcdy1, adxcdy0);
    Two_Two_Diff(cdxady1, cdxady0, adxcdy1, adxcdy0, ca3, ca[2], ca[1], ca[0]);
    ca[3] = ca3;
    bxcalen = scale_expansion_zeroelim(4, ca, bdx, bxca);
    bxxcalen = scale_expansion_zeroelim(bxcalen, bxca, bdx, bxxca);
    bycalen = scale_expansion_zeroelim(4, ca, bdy, byca);
    byycalen = scale_expansion_zeroelim(bycalen, byca, bdy, byyca);
    blen = fast_expansion_sum_zeroelim(bxxcalen, bxxca, byycalen, byyca, bdet);

    Two_Product(adx, bdy, adxbdy1, adxbdy0);
    Two_Product(bdx, ady, bdxady1, bdxady0);
    Two_Two_Diff(adxbdy1, adxbdy0, bdxady1, bdxady0, ab3, ab[2], ab[1], ab[0]);
    ab[3] = ab3;
    cxablen = scale_expansion_zeroelim(4, ab, cdx, cxab);
    cxxablen = scale_expansion_zeroelim(cxablen, cxab, cdx, cxxab);
    cyablen = scale_expansion_zeroelim(4, ab, cdy, cyab);
    cyyablen = scale_expansion_zeroelim(cyablen, cyab, cdy, cyyab);
    clen = fast_expansion_sum_zeroelim(cxxablen, cxxab, cyyablen, cyyab, cdet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

    det = estimate(finlength, fin1);
    errbound = iccerrboundB * permanent;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    Two_Diff_Tail(pa[0], pd[0], adx, adxtail);
    Two_Diff_Tail(pa[1], pd[1], ady, adytail);
    Two_Diff_Tail(pb[0], pd[0], bdx, bdxtail);
    Two_Diff_Tail(pb[1], pd[1], bdy, bdytail);
    Two_Diff_Tail(pc[0], pd[0], cdx, cdxtail);
    Two_Diff_Tail(pc[1], pd[1], cdy, cdytail);
    if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0)
        && (adytail == 0.0) && (bdytail == 0.0) && (cdytail == 0.0)) {
        return det;
    }

    errbound = iccerrboundC * permanent + resulterrbound * Absolute(det);
    det += ((adx * adx + ady * ady) * ((bdx * cdytail + cdy * bdxtail) - (bdy * cdxtail + cdx * bdytail))
               + 2.0 * (adx * adxtail + ady * adytail) * (bdx * cdy - bdy * cdx))
        + ((bdx * bdx + bdy * bdy) * ((cdx * adytail + ady * cdxtail) - (cdy * adxtail + adx * cdytail))
            + 2.0 * (bdx * bdxtail + bdy * bdytail) * (cdx * ady - cdy * adx))
        + ((cdx * cdx + cdy * cdy) * ((adx * bdytail + bdy * adxtail) - (ady * bdxtail + bdx * adytail))
            + 2.0 * (cdx * cdxtail + cdy * cdytail) * (adx * bdy - ady * bdx));
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    finnow = fin1;
    finother = fin2;

    if ((bdxtail != 0.0) || (bdytail != 0.0)
        || (cdxtail != 0.0) || (cdytail != 0.0)) {
        Square(adx, adxadx1, adxadx0);
        Square(ady, adyady1, adyady0);
        Two_Two_Sum(adxadx1, adxadx0, adyady1, adyady0, aa3, aa[2], aa[1], aa[0]);
        aa[3] = aa3;
    }
    if ((cdxtail != 0.0) || (cdytail != 0.0)
        || (adxtail != 0.0) || (adytail != 0.0)) {
        Square(bdx, bdxbdx1, bdxbdx0);
        Square(bdy, bdybdy1, bdybdy0);
        Two_Two_Sum(bdxbdx1, bdxbdx0, bdybdy1, bdybdy0, bb3, bb[2], bb[1], bb[0]);
        bb[3] = bb3;
    }
    if ((adxtail != 0.0) || (adytail != 0.0)
        || (bdxtail != 0.0) || (bdytail != 0.0)) {
        Square(cdx, cdxcdx1, cdxcdx0);
        Square(cdy, cdycdy1, cdycdy0);
        Two_Two_Sum(cdxcdx1, cdxcdx0, cdycdy1, cdycdy0, cc3, cc[2], cc[1], cc[0]);
        cc[3] = cc3;
    }

    if (adxtail != 0.0) {
        axtbclen = scale_expansion_zeroelim(4, bc, adxtail, axtbc);
        temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, 2.0 * adx,
            temp16a);

        axtcclen = scale_expansion_zeroelim(4, cc, adxtail, axtcc);
        temp16blen = scale_expansion_zeroelim(axtcclen, axtcc, bdy, temp16b);

        axtbblen = scale_expansion_zeroelim(4, bb, adxtail, axtbb);
        temp16clen = scale_expansion_zeroelim(axtbblen, axtbb, -cdy, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (adytail != 0.0) {
        aytbclen = scale_expansion_zeroelim(4, bc, adytail, aytbc);
        temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, 2.0 * ady,
            temp16a);

        aytbblen = scale_expansion_zeroelim(4, bb, adytail, aytbb);
        temp16blen = scale_expansion_zeroelim(aytbblen, aytbb, cdx, temp16b);

        aytcclen = scale_expansion_zeroelim(4, cc, adytail, aytcc);
        temp16clen = scale_expansion_zeroelim(aytcclen, aytcc, -bdx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (bdxtail != 0.0) {
        bxtcalen = scale_expansion_zeroelim(4, ca, bdxtail, bxtca);
        temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, 2.0 * bdx,
            temp16a);

        bxtaalen = scale_expansion_zeroelim(4, aa, bdxtail, bxtaa);
        temp16blen = scale_expansion_zeroelim(bxtaalen, bxtaa, cdy, temp16b);

        bxtcclen = scale_expansion_zeroelim(4, cc, bdxtail, bxtcc);
        temp16clen = scale_expansion_zeroelim(bxtcclen, bxtcc, -ady, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (bdytail != 0.0) {
        bytcalen = scale_expansion_zeroelim(4, ca, bdytail, bytca);
        temp16alen = scale_expansion_zeroelim(bytcalen, bytca, 2.0 * bdy,
            temp16a);

        bytcclen = scale_expansion_zeroelim(4, cc, bdytail, bytcc);
        temp16blen = scale_expansion_zeroelim(bytcclen, bytcc, adx, temp16b);

        bytaalen = scale_expansion_zeroelim(4, aa, bdytail, bytaa);
        temp16clen = scale_expansion_zeroelim(bytaalen, bytaa, -cdx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (cdxtail != 0.0) {
        cxtablen = scale_expansion_zeroelim(4, ab, cdxtail, cxtab);
        temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, 2.0 * cdx,
            temp16a);

        cxtbblen = scale_expansion_zeroelim(4, bb, cdxtail, cxtbb);
        temp16blen = scale_expansion_zeroelim(cxtbblen, cxtbb, ady, temp16b);

        cxtaalen = scale_expansion_zeroelim(4, aa, cdxtail, cxtaa);
        temp16clen = scale_expansion_zeroelim(cxtaalen, cxtaa, -bdy, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }
    if (cdytail != 0.0) {
        cytablen = scale_expansion_zeroelim(4, ab, cdytail, cytab);
        temp16alen = scale_expansion_zeroelim(cytablen, cytab, 2.0 * cdy,
            temp16a);

        cytaalen = scale_expansion_zeroelim(4, aa, cdytail, cytaa);
        temp16blen = scale_expansion_zeroelim(cytaalen, cytaa, bdx, temp16b);

        cytbblen = scale_expansion_zeroelim(4, bb, cdytail, cytbb);
        temp16clen = scale_expansion_zeroelim(cytbblen, cytbb, -adx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
            temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c,
            temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
            temp48, finother);
        finswap = finnow;
        finnow = finother;
        finother = finswap;
    }

    if ((adxtail != 0.0) || (adytail != 0.0)) {
        if ((bdxtail != 0.0) || (bdytail != 0.0)
            || (cdxtail != 0.0) || (cdytail != 0.0)) {
            Two_Product(bdxtail, cdy, ti1, ti0);
            Two_Product(bdx, cdytail, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
            u[3] = u3;
            negate = -bdy;
            Two_Product(cdxtail, negate, ti1, ti0);
            negate = -bdytail;
            Two_Product(cdx, negate, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
            v[3] = v3;
            bctlen = fast_expansion_sum_zeroelim(4, u, 4, v, bct);

            Two_Product(bdxtail, cdytail, ti1, ti0);
            Two_Product(cdxtail, bdytail, tj1, tj0);
            Two_Two_Diff(ti1, ti0, tj1, tj0, bctt3, bctt[2], bctt[1], bctt[0]);
            bctt[3] = bctt3;
            bcttlen = 4;
        } else {
            bct[0] = 0.0;
            bctlen = 1;
            bctt[0] = 0.0;
            bcttlen = 1;
        }

        if (adxtail != 0.0) {
            temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, adxtail, temp16a);
            axtbctlen = scale_expansion_zeroelim(bctlen, bct, adxtail, axtbct);
            temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, 2.0 * adx,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (bdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, cc, adxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
            if (cdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, bb, -adxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }

            temp32alen = scale_expansion_zeroelim(axtbctlen, axtbct, adxtail,
                temp32a);
            axtbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adxtail, axtbctt);
            temp16alen = scale_expansion_zeroelim(axtbcttlen, axtbctt, 2.0 * adx,
                temp16a);
            temp16blen = scale_expansion_zeroelim(axtbcttlen, axtbctt, adxtail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
        if (adytail != 0.0) {
            temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, adytail, temp16a);
            aytbctlen = scale_expansion_zeroelim(bctlen, bct, adytail, aytbct);
            temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, 2.0 * ady,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;

            temp32alen = scale_expansion_zeroelim(aytbctlen, aytbct, adytail,
                temp32a);
            aytbcttlen = scale_expansion_zeroelim(bcttlen, bctt, adytail, aytbctt);
            temp16alen = scale_expansion_zeroelim(aytbcttlen, aytbctt, 2.0 * ady,
                temp16a);
            temp16blen = scale_expansion_zeroelim(aytbcttlen, aytbctt, adytail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
    }
    if ((bdxtail != 0.0) || (bdytail != 0.0)) {
        if ((cdxtail != 0.0) || (cdytail != 0.0)
            || (adxtail != 0.0) || (adytail != 0.0)) {
            Two_Product(cdxtail, ady, ti1, ti0);
            Two_Product(cdx, adytail, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
            u[3] = u3;
            negate = -cdy;
            Two_Product(adxtail, negate, ti1, ti0);
            negate = -cdytail;
            Two_Product(adx, negate, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
            v[3] = v3;
            catlen = fast_expansion_sum_zeroelim(4, u, 4, v, cat);

            Two_Product(cdxtail, adytail, ti1, ti0);
            Two_Product(adxtail, cdytail, tj1, tj0);
            Two_Two_Diff(ti1, ti0, tj1, tj0, catt3, catt[2], catt[1], catt[0]);
            catt[3] = catt3;
            cattlen = 4;
        } else {
            cat[0] = 0.0;
            catlen = 1;
            catt[0] = 0.0;
            cattlen = 1;
        }

        if (bdxtail != 0.0) {
            temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, bdxtail, temp16a);
            bxtcatlen = scale_expansion_zeroelim(catlen, cat, bdxtail, bxtcat);
            temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, 2.0 * bdx,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (cdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, aa, bdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
            if (adytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, cc, -bdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }

            temp32alen = scale_expansion_zeroelim(bxtcatlen, bxtcat, bdxtail,
                temp32a);
            bxtcattlen = scale_expansion_zeroelim(cattlen, catt, bdxtail, bxtcatt);
            temp16alen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, 2.0 * bdx,
                temp16a);
            temp16blen = scale_expansion_zeroelim(bxtcattlen, bxtcatt, bdxtail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
        if (bdytail != 0.0) {
            temp16alen = scale_expansion_zeroelim(bytcalen, bytca, bdytail, temp16a);
            bytcatlen = scale_expansion_zeroelim(catlen, cat, bdytail, bytcat);
            temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, 2.0 * bdy,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;

            temp32alen = scale_expansion_zeroelim(bytcatlen, bytcat, bdytail,
                temp32a);
            bytcattlen = scale_expansion_zeroelim(cattlen, catt, bdytail, bytcatt);
            temp16alen = scale_expansion_zeroelim(bytcattlen, bytcatt, 2.0 * bdy,
                temp16a);
            temp16blen = scale_expansion_zeroelim(bytcattlen, bytcatt, bdytail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
    }
    if ((cdxtail != 0.0) || (cdytail != 0.0)) {
        if ((adxtail != 0.0) || (adytail != 0.0)
            || (bdxtail != 0.0) || (bdytail != 0.0)) {
            Two_Product(adxtail, bdy, ti1, ti0);
            Two_Product(adx, bdytail, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, u3, u[2], u[1], u[0]);
            u[3] = u3;
            negate = -ady;
            Two_Product(bdxtail, negate, ti1, ti0);
            negate = -adytail;
            Two_Product(bdx, negate, tj1, tj0);
            Two_Two_Sum(ti1, ti0, tj1, tj0, v3, v[2], v[1], v[0]);
            v[3] = v3;
            abtlen = fast_expansion_sum_zeroelim(4, u, 4, v, abt);

            Two_Product(adxtail, bdytail, ti1, ti0);
            Two_Product(bdxtail, adytail, tj1, tj0);
            Two_Two_Diff(ti1, ti0, tj1, tj0, abtt3, abtt[2], abtt[1], abtt[0]);
            abtt[3] = abtt3;
            abttlen = 4;
        } else {
            abt[0] = 0.0;
            abtlen = 1;
            abtt[0] = 0.0;
            abttlen = 1;
        }

        if (cdxtail != 0.0) {
            temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, cdxtail, temp16a);
            cxtabtlen = scale_expansion_zeroelim(abtlen, abt, cdxtail, cxtabt);
            temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, 2.0 * cdx,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
            if (adytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, bb, cdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }
            if (bdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, aa, -cdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail,
                    temp16a);
                finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp16alen,
                    temp16a, finother);
                finswap = finnow;
                finnow = finother;
                finother = finswap;
            }

            temp32alen = scale_expansion_zeroelim(cxtabtlen, cxtabt, cdxtail,
                temp32a);
            cxtabttlen = scale_expansion_zeroelim(abttlen, abtt, cdxtail, cxtabtt);
            temp16alen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, 2.0 * cdx,
                temp16a);
            temp16blen = scale_expansion_zeroelim(cxtabttlen, cxtabtt, cdxtail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
        if (cdytail != 0.0) {
            temp16alen = scale_expansion_zeroelim(cytablen, cytab, cdytail, temp16a);
            cytabtlen = scale_expansion_zeroelim(abtlen, abt, cdytail, cytabt);
            temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, 2.0 * cdy,
                temp32a);
            temp48len = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp32alen, temp32a, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len,
                temp48, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;

            temp32alen = scale_expansion_zeroelim(cytabtlen, cytabt, cdytail,
                temp32a);
            cytabttlen = scale_expansion_zeroelim(abttlen, abtt, cdytail, cytabtt);
            temp16alen = scale_expansion_zeroelim(cytabttlen, cytabtt, 2.0 * cdy,
                temp16a);
            temp16blen = scale_expansion_zeroelim(cytabttlen, cytabtt, cdytail,
                temp16b);
            temp32blen = fast_expansion_sum_zeroelim(temp16alen, temp16a,
                temp16blen, temp16b, temp32b);
            temp64len = fast_expansion_sum_zeroelim(temp32alen, temp32a,
                temp32blen, temp32b, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len,
                temp64, finother);
            finswap = finnow;
            finnow = finother;
            finother = finswap;
        }
    }

    return finnow[finlength - 1];
}

REAL incircle(pa, pb, pc, pd)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
{
    REAL adx, bdx, cdx, ady, bdy, cdy;
    REAL bdxcdy, cdxbdy, cdxady, adxcdy, adxbdy, bdxady;
    REAL alift, blift, clift;
    REAL det;
    REAL permanent, errbound;

    adx = pa[0] - pd[0];
    bdx = pb[0] - pd[0];
    cdx = pc[0] - pd[0];
    ady = pa[1] - pd[1];
    bdy = pb[1] - pd[1];
    cdy = pc[1] - pd[1];

    bdxcdy = bdx * cdy;
    cdxbdy = cdx * bdy;
    alift = adx * adx + ady * ady;

    cdxady = cdx * ady;
    adxcdy = adx * cdy;
    blift = bdx * bdx + bdy * bdy;

    adxbdy = adx * bdy;
    bdxady = bdx * ady;
    clift = cdx * cdx + cdy * cdy;

    det = alift * (bdxcdy - cdxbdy)
        + blift * (cdxady - adxcdy)
        + clift * (adxbdy - bdxady);

    permanent = (Absolute(bdxcdy) + Absolute(cdxbdy)) * alift
        + (Absolute(cdxady) + Absolute(adxcdy)) * blift
        + (Absolute(adxbdy) + Absolute(bdxady)) * clift;
    errbound = iccerrboundA * permanent;
    if ((det > errbound) || (-det > errbound)) {
        return det;
    }

    return incircleadapt(pa, pb, pc, pd, permanent);
}

/*****************************************************************************/
/*                                                                           */
/*  inspherefast()   Approximate 3D insphere test.  Nonrobust.               */
/*  insphereexact()   Exact 3D insphere test.  Robust.                       */
/*  insphereslow()   Another exact 3D insphere test.  Robust.                */
/*  insphere()   Adaptive exact 3D insphere test.  Robust.                   */
/*                                                                           */
/*               Return a positive value if the point pe lies inside the     */
/*               sphere passing through pa, pb, pc, and pd; a negative value */
/*               if it lies outside; and zero if the five points are         */
/*               cospherical.  The points pa, pb, pc, and pd must be ordered */
/*               so that they have a positive orientation (as defined by     */
/*               orient3d()), or the sign of the result will be reversed.    */
/*                                                                           */
/*  Only the first and last routine should be used; the middle two are for   */
/*  timings.                                                                 */
/*                                                                           */
/*  The last three use exact arithmetic to ensure a correct answer.  The     */
/*  result returned is the determinant of a matrix.  In insphere() only,     */
/*  this determinant is computed adaptively, in the sense that exact         */
/*  arithmetic is used only to the degree it is needed to ensure that the    */
/*  returned value has the correct sign.  Hence, insphere() is usually quite */
/*  fast, but will run more slowly when the input points are cospherical or  */
/*  nearly so.                                                               */
/*                                                                           */
/*****************************************************************************/

REAL inspherefast(pa, pb, pc, pd, pe)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL* pe;
{
    REAL aex, bex, cex, dex;
    REAL aey, bey, cey, dey;
    REAL aez, bez, cez, dez;
    REAL alift, blift, clift, dlift;
    REAL ab, bc, cd, da, ac, bd;
    REAL abc, bcd, cda, dab;

    aex = pa[0] - pe[0];
    bex = pb[0] - pe[0];
    cex = pc[0] - pe[0];
    dex = pd[0] - pe[0];
    aey = pa[1] - pe[1];
    bey = pb[1] - pe[1];
    cey = pc[1] - pe[1];
    dey = pd[1] - pe[1];
    aez = pa[2] - pe[2];
    bez = pb[2] - pe[2];
    cez = pc[2] - pe[2];
    dez = pd[2] - pe[2];

    ab = aex * bey - bex * aey;
    bc = bex * cey - cex * bey;
    cd = cex * dey - dex * cey;
    da = dex * aey - aex * dey;

    ac = aex * cey - cex * aey;
    bd = bex * dey - dex * bey;

    abc = aez * bc - bez * ac + cez * ab;
    bcd = bez * cd - cez * bd + dez * bc;
    cda = cez * da + dez * ac + aez * cd;
    dab = dez * ab + aez * bd + bez * da;

    alift = aex * aex + aey * aey + aez * aez;
    blift = bex * bex + bey * bey + bez * bez;
    clift = cex * cex + cey * cey + cez * cez;
    dlift = dex * dex + dey * dey + dez * dez;

    return (dlift * abc - clift * dab) + (blift * cda - alift * bcd);
}

REAL insphereexact(pa, pb, pc, pd, pe)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL* pe;
{
    INEXACT REAL axby1, bxcy1, cxdy1, dxey1, exay1;
    INEXACT REAL bxay1, cxby1, dxcy1, exdy1, axey1;
    INEXACT REAL axcy1, bxdy1, cxey1, dxay1, exby1;
    INEXACT REAL cxay1, dxby1, excy1, axdy1, bxey1;
    REAL axby0, bxcy0, cxdy0, dxey0, exay0;
    REAL bxay0, cxby0, dxcy0, exdy0, axey0;
    REAL axcy0, bxdy0, cxey0, dxay0, exby0;
    REAL cxay0, dxby0, excy0, axdy0, bxey0;
    REAL ab[4], bc[4], cd[4], de[4], ea[4];
    REAL ac[4], bd[4], ce[4], da[4], eb[4];
    REAL temp8a[8], temp8b[8], temp16[16];
    int temp8alen, temp8blen, temp16len;
    REAL abc[24], bcd[24], cde[24], dea[24], eab[24];
    REAL abd[24], bce[24], cda[24], deb[24], eac[24];
    int abclen, bcdlen, cdelen, dealen, eablen;
    int abdlen, bcelen, cdalen, deblen, eaclen;
    REAL temp48a[48], temp48b[48];
    int temp48alen, temp48blen;
    REAL abcd[96], bcde[96], cdea[96], deab[96], eabc[96];
    int abcdlen, bcdelen, cdealen, deablen, eabclen;
    REAL temp192[192];
    REAL det384x[384], det384y[384], det384z[384];
    int xlen, ylen, zlen;
    REAL detxy[768];
    int xylen;
    REAL adet[1152], bdet[1152], cdet[1152], ddet[1152], edet[1152];
    int alen, blen, clen, dlen, elen;
    REAL abdet[2304], cddet[2304], cdedet[3456];
    int ablen, cdlen;
    REAL deter[5760];
    int deterlen;
    int i;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    Two_Product(pa[0], pb[1], axby1, axby0);
    Two_Product(pb[0], pa[1], bxay1, bxay0);
    Two_Two_Diff(axby1, axby0, bxay1, bxay0, ab[3], ab[2], ab[1], ab[0]);

    Two_Product(pb[0], pc[1], bxcy1, bxcy0);
    Two_Product(pc[0], pb[1], cxby1, cxby0);
    Two_Two_Diff(bxcy1, bxcy0, cxby1, cxby0, bc[3], bc[2], bc[1], bc[0]);

    Two_Product(pc[0], pd[1], cxdy1, cxdy0);
    Two_Product(pd[0], pc[1], dxcy1, dxcy0);
    Two_Two_Diff(cxdy1, cxdy0, dxcy1, dxcy0, cd[3], cd[2], cd[1], cd[0]);

    Two_Product(pd[0], pe[1], dxey1, dxey0);
    Two_Product(pe[0], pd[1], exdy1, exdy0);
    Two_Two_Diff(dxey1, dxey0, exdy1, exdy0, de[3], de[2], de[1], de[0]);

    Two_Product(pe[0], pa[1], exay1, exay0);
    Two_Product(pa[0], pe[1], axey1, axey0);
    Two_Two_Diff(exay1, exay0, axey1, axey0, ea[3], ea[2], ea[1], ea[0]);

    Two_Product(pa[0], pc[1], axcy1, axcy0);
    Two_Product(pc[0], pa[1], cxay1, cxay0);
    Two_Two_Diff(axcy1, axcy0, cxay1, cxay0, ac[3], ac[2], ac[1], ac[0]);

    Two_Product(pb[0], pd[1], bxdy1, bxdy0);
    Two_Product(pd[0], pb[1], dxby1, dxby0);
    Two_Two_Diff(bxdy1, bxdy0, dxby1, dxby0, bd[3], bd[2], bd[1], bd[0]);

    Two_Product(pc[0], pe[1], cxey1, cxey0);
    Two_Product(pe[0], pc[1], excy1, excy0);
    Two_Two_Diff(cxey1, cxey0, excy1, excy0, ce[3], ce[2], ce[1], ce[0]);

    Two_Product(pd[0], pa[1], dxay1, dxay0);
    Two_Product(pa[0], pd[1], axdy1, axdy0);
    Two_Two_Diff(dxay1, dxay0, axdy1, axdy0, da[3], da[2], da[1], da[0]);

    Two_Product(pe[0], pb[1], exby1, exby0);
    Two_Product(pb[0], pe[1], bxey1, bxey0);
    Two_Two_Diff(exby1, exby0, bxey1, bxey0, eb[3], eb[2], eb[1], eb[0]);

    temp8alen = scale_expansion_zeroelim(4, bc, pa[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, ac, -pb[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, ab, pc[2], temp8a);
    abclen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        abc);

    temp8alen = scale_expansion_zeroelim(4, cd, pb[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, bd, -pc[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, bc, pd[2], temp8a);
    bcdlen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        bcd);

    temp8alen = scale_expansion_zeroelim(4, de, pc[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, ce, -pd[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, cd, pe[2], temp8a);
    cdelen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        cde);

    temp8alen = scale_expansion_zeroelim(4, ea, pd[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, da, -pe[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, de, pa[2], temp8a);
    dealen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        dea);

    temp8alen = scale_expansion_zeroelim(4, ab, pe[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, eb, -pa[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, ea, pb[2], temp8a);
    eablen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        eab);

    temp8alen = scale_expansion_zeroelim(4, bd, pa[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, da, pb[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, ab, pd[2], temp8a);
    abdlen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        abd);

    temp8alen = scale_expansion_zeroelim(4, ce, pb[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, eb, pc[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, bc, pe[2], temp8a);
    bcelen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        bce);

    temp8alen = scale_expansion_zeroelim(4, da, pc[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, ac, pd[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, cd, pa[2], temp8a);
    cdalen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        cda);

    temp8alen = scale_expansion_zeroelim(4, eb, pd[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, bd, pe[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, de, pb[2], temp8a);
    deblen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        deb);

    temp8alen = scale_expansion_zeroelim(4, ac, pe[2], temp8a);
    temp8blen = scale_expansion_zeroelim(4, ce, pa[2], temp8b);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp8blen, temp8b,
        temp16);
    temp8alen = scale_expansion_zeroelim(4, ea, pc[2], temp8a);
    eaclen = fast_expansion_sum_zeroelim(temp8alen, temp8a, temp16len, temp16,
        eac);

    temp48alen = fast_expansion_sum_zeroelim(cdelen, cde, bcelen, bce, temp48a);
    temp48blen = fast_expansion_sum_zeroelim(deblen, deb, bcdlen, bcd, temp48b);
    for (i = 0; i < temp48blen; i++) {
        temp48b[i] = -temp48b[i];
    }
    bcdelen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
        temp48blen, temp48b, bcde);
    xlen = scale_expansion_zeroelim(bcdelen, bcde, pa[0], temp192);
    xlen = scale_expansion_zeroelim(xlen, temp192, pa[0], det384x);
    ylen = scale_expansion_zeroelim(bcdelen, bcde, pa[1], temp192);
    ylen = scale_expansion_zeroelim(ylen, temp192, pa[1], det384y);
    zlen = scale_expansion_zeroelim(bcdelen, bcde, pa[2], temp192);
    zlen = scale_expansion_zeroelim(zlen, temp192, pa[2], det384z);
    xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
    alen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, adet);

    temp48alen = fast_expansion_sum_zeroelim(dealen, dea, cdalen, cda, temp48a);
    temp48blen = fast_expansion_sum_zeroelim(eaclen, eac, cdelen, cde, temp48b);
    for (i = 0; i < temp48blen; i++) {
        temp48b[i] = -temp48b[i];
    }
    cdealen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
        temp48blen, temp48b, cdea);
    xlen = scale_expansion_zeroelim(cdealen, cdea, pb[0], temp192);
    xlen = scale_expansion_zeroelim(xlen, temp192, pb[0], det384x);
    ylen = scale_expansion_zeroelim(cdealen, cdea, pb[1], temp192);
    ylen = scale_expansion_zeroelim(ylen, temp192, pb[1], det384y);
    zlen = scale_expansion_zeroelim(cdealen, cdea, pb[2], temp192);
    zlen = scale_expansion_zeroelim(zlen, temp192, pb[2], det384z);
    xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
    blen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, bdet);

    temp48alen = fast_expansion_sum_zeroelim(eablen, eab, deblen, deb, temp48a);
    temp48blen = fast_expansion_sum_zeroelim(abdlen, abd, dealen, dea, temp48b);
    for (i = 0; i < temp48blen; i++) {
        temp48b[i] = -temp48b[i];
    }
    deablen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
        temp48blen, temp48b, deab);
    xlen = scale_expansion_zeroelim(deablen, deab, pc[0], temp192);
    xlen = scale_expansion_zeroelim(xlen, temp192, pc[0], det384x);
    ylen = scale_expansion_zeroelim(deablen, deab, pc[1], temp192);
    ylen = scale_expansion_zeroelim(ylen, temp192, pc[1], det384y);
    zlen = scale_expansion_zeroelim(deablen, deab, pc[2], temp192);
    zlen = scale_expansion_zeroelim(zlen, temp192, pc[2], det384z);
    xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
    clen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, cdet);

    temp48alen = fast_expansion_sum_zeroelim(abclen, abc, eaclen, eac, temp48a);
    temp48blen = fast_expansion_sum_zeroelim(bcelen, bce, eablen, eab, temp48b);
    for (i = 0; i < temp48blen; i++) {
        temp48b[i] = -temp48b[i];
    }
    eabclen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
        temp48blen, temp48b, eabc);
    xlen = scale_expansion_zeroelim(eabclen, eabc, pd[0], temp192);
    xlen = scale_expansion_zeroelim(xlen, temp192, pd[0], det384x);
    ylen = scale_expansion_zeroelim(eabclen, eabc, pd[1], temp192);
    ylen = scale_expansion_zeroelim(ylen, temp192, pd[1], det384y);
    zlen = scale_expansion_zeroelim(eabclen, eabc, pd[2], temp192);
    zlen = scale_expansion_zeroelim(zlen, temp192, pd[2], det384z);
    xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
    dlen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, ddet);

    temp48alen = fast_expansion_sum_zeroelim(bcdlen, bcd, abdlen, abd, temp48a);
    temp48blen = fast_expansion_sum_zeroelim(cdalen, cda, abclen, abc, temp48b);
    for (i = 0; i < temp48blen; i++) {
        temp48b[i] = -temp48b[i];
    }
    abcdlen = fast_expansion_sum_zeroelim(temp48alen, temp48a,
        temp48blen, temp48b, abcd);
    xlen = scale_expansion_zeroelim(abcdlen, abcd, pe[0], temp192);
    xlen = scale_expansion_zeroelim(xlen, temp192, pe[0], det384x);
    ylen = scale_expansion_zeroelim(abcdlen, abcd, pe[1], temp192);
    ylen = scale_expansion_zeroelim(ylen, temp192, pe[1], det384y);
    zlen = scale_expansion_zeroelim(abcdlen, abcd, pe[2], temp192);
    zlen = scale_expansion_zeroelim(zlen, temp192, pe[2], det384z);
    xylen = fast_expansion_sum_zeroelim(xlen, det384x, ylen, det384y, detxy);
    elen = fast_expansion_sum_zeroelim(xylen, detxy, zlen, det384z, edet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
    cdelen = fast_expansion_sum_zeroelim(cdlen, cddet, elen, edet, cdedet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdelen, cdedet, deter);

    return deter[deterlen - 1];
}

REAL insphereslow(pa, pb, pc, pd, pe)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL* pe;
{
    INEXACT REAL aex, bex, cex, dex, aey, bey, cey, dey, aez, bez, cez, dez;
    REAL aextail, bextail, cextail, dextail;
    REAL aeytail, beytail, ceytail, deytail;
    REAL aeztail, beztail, ceztail, deztail;
    REAL negate, negatetail;
    INEXACT REAL axby7, bxcy7, cxdy7, dxay7, axcy7, bxdy7;
    INEXACT REAL bxay7, cxby7, dxcy7, axdy7, cxay7, dxby7;
    REAL axby[8], bxcy[8], cxdy[8], dxay[8], axcy[8], bxdy[8];
    REAL bxay[8], cxby[8], dxcy[8], axdy[8], cxay[8], dxby[8];
    REAL ab[16], bc[16], cd[16], da[16], ac[16], bd[16];
    int ablen, bclen, cdlen, dalen, aclen, bdlen;
    REAL temp32a[32], temp32b[32], temp64a[64], temp64b[64], temp64c[64];
    int temp32alen, temp32blen, temp64alen, temp64blen, temp64clen;
    REAL temp128[128], temp192[192];
    int temp128len, temp192len;
    REAL detx[384], detxx[768], detxt[384], detxxt[768], detxtxt[768];
    int xlen, xxlen, xtlen, xxtlen, xtxtlen;
    REAL x1[1536], x2[2304];
    int x1len, x2len;
    REAL dety[384], detyy[768], detyt[384], detyyt[768], detytyt[768];
    int ylen, yylen, ytlen, yytlen, ytytlen;
    REAL y1[1536], y2[2304];
    int y1len, y2len;
    REAL detz[384], detzz[768], detzt[384], detzzt[768], detztzt[768];
    int zlen, zzlen, ztlen, zztlen, ztztlen;
    REAL z1[1536], z2[2304];
    int z1len, z2len;
    REAL detxy[4608];
    int xylen;
    REAL adet[6912], bdet[6912], cdet[6912], ddet[6912];
    int alen, blen, clen, dlen;
    REAL abdet[13824], cddet[13824], deter[27648];
    int deterlen;
    int i;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL a0hi, a0lo, a1hi, a1lo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j, _k, _l, _m, _n;
    REAL _0, _1, _2;

    Two_Diff(pa[0], pe[0], aex, aextail);
    Two_Diff(pa[1], pe[1], aey, aeytail);
    Two_Diff(pa[2], pe[2], aez, aeztail);
    Two_Diff(pb[0], pe[0], bex, bextail);
    Two_Diff(pb[1], pe[1], bey, beytail);
    Two_Diff(pb[2], pe[2], bez, beztail);
    Two_Diff(pc[0], pe[0], cex, cextail);
    Two_Diff(pc[1], pe[1], cey, ceytail);
    Two_Diff(pc[2], pe[2], cez, ceztail);
    Two_Diff(pd[0], pe[0], dex, dextail);
    Two_Diff(pd[1], pe[1], dey, deytail);
    Two_Diff(pd[2], pe[2], dez, deztail);

    Two_Two_Product(aex, aextail, bey, beytail,
        axby7, axby[6], axby[5], axby[4],
        axby[3], axby[2], axby[1], axby[0]);
    axby[7] = axby7;
    negate = -aey;
    negatetail = -aeytail;
    Two_Two_Product(bex, bextail, negate, negatetail,
        bxay7, bxay[6], bxay[5], bxay[4],
        bxay[3], bxay[2], bxay[1], bxay[0]);
    bxay[7] = bxay7;
    ablen = fast_expansion_sum_zeroelim(8, axby, 8, bxay, ab);
    Two_Two_Product(bex, bextail, cey, ceytail,
        bxcy7, bxcy[6], bxcy[5], bxcy[4],
        bxcy[3], bxcy[2], bxcy[1], bxcy[0]);
    bxcy[7] = bxcy7;
    negate = -bey;
    negatetail = -beytail;
    Two_Two_Product(cex, cextail, negate, negatetail,
        cxby7, cxby[6], cxby[5], cxby[4],
        cxby[3], cxby[2], cxby[1], cxby[0]);
    cxby[7] = cxby7;
    bclen = fast_expansion_sum_zeroelim(8, bxcy, 8, cxby, bc);
    Two_Two_Product(cex, cextail, dey, deytail,
        cxdy7, cxdy[6], cxdy[5], cxdy[4],
        cxdy[3], cxdy[2], cxdy[1], cxdy[0]);
    cxdy[7] = cxdy7;
    negate = -cey;
    negatetail = -ceytail;
    Two_Two_Product(dex, dextail, negate, negatetail,
        dxcy7, dxcy[6], dxcy[5], dxcy[4],
        dxcy[3], dxcy[2], dxcy[1], dxcy[0]);
    dxcy[7] = dxcy7;
    cdlen = fast_expansion_sum_zeroelim(8, cxdy, 8, dxcy, cd);
    Two_Two_Product(dex, dextail, aey, aeytail,
        dxay7, dxay[6], dxay[5], dxay[4],
        dxay[3], dxay[2], dxay[1], dxay[0]);
    dxay[7] = dxay7;
    negate = -dey;
    negatetail = -deytail;
    Two_Two_Product(aex, aextail, negate, negatetail,
        axdy7, axdy[6], axdy[5], axdy[4],
        axdy[3], axdy[2], axdy[1], axdy[0]);
    axdy[7] = axdy7;
    dalen = fast_expansion_sum_zeroelim(8, dxay, 8, axdy, da);
    Two_Two_Product(aex, aextail, cey, ceytail,
        axcy7, axcy[6], axcy[5], axcy[4],
        axcy[3], axcy[2], axcy[1], axcy[0]);
    axcy[7] = axcy7;
    negate = -aey;
    negatetail = -aeytail;
    Two_Two_Product(cex, cextail, negate, negatetail,
        cxay7, cxay[6], cxay[5], cxay[4],
        cxay[3], cxay[2], cxay[1], cxay[0]);
    cxay[7] = cxay7;
    aclen = fast_expansion_sum_zeroelim(8, axcy, 8, cxay, ac);
    Two_Two_Product(bex, bextail, dey, deytail,
        bxdy7, bxdy[6], bxdy[5], bxdy[4],
        bxdy[3], bxdy[2], bxdy[1], bxdy[0]);
    bxdy[7] = bxdy7;
    negate = -bey;
    negatetail = -beytail;
    Two_Two_Product(dex, dextail, negate, negatetail,
        dxby7, dxby[6], dxby[5], dxby[4],
        dxby[3], dxby[2], dxby[1], dxby[0]);
    dxby[7] = dxby7;
    bdlen = fast_expansion_sum_zeroelim(8, bxdy, 8, dxby, bd);

    temp32alen = scale_expansion_zeroelim(cdlen, cd, -bez, temp32a);
    temp32blen = scale_expansion_zeroelim(cdlen, cd, -beztail, temp32b);
    temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64a);
    temp32alen = scale_expansion_zeroelim(bdlen, bd, cez, temp32a);
    temp32blen = scale_expansion_zeroelim(bdlen, bd, ceztail, temp32b);
    temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64b);
    temp32alen = scale_expansion_zeroelim(bclen, bc, -dez, temp32a);
    temp32blen = scale_expansion_zeroelim(bclen, bc, -deztail, temp32b);
    temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64c);
    temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
        temp64blen, temp64b, temp128);
    temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
        temp128len, temp128, temp192);
    xlen = scale_expansion_zeroelim(temp192len, temp192, aex, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, aex, detxx);
    xtlen = scale_expansion_zeroelim(temp192len, temp192, aextail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, aex, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, aextail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
    ylen = scale_expansion_zeroelim(temp192len, temp192, aey, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, aey, detyy);
    ytlen = scale_expansion_zeroelim(temp192len, temp192, aeytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, aey, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, aeytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
    zlen = scale_expansion_zeroelim(temp192len, temp192, aez, detz);
    zzlen = scale_expansion_zeroelim(zlen, detz, aez, detzz);
    ztlen = scale_expansion_zeroelim(temp192len, temp192, aeztail, detzt);
    zztlen = scale_expansion_zeroelim(ztlen, detzt, aez, detzzt);
    for (i = 0; i < zztlen; i++) {
        detzzt[i] *= 2.0;
    }
    ztztlen = scale_expansion_zeroelim(ztlen, detzt, aeztail, detztzt);
    z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
    z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
    xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
    alen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, adet);

    temp32alen = scale_expansion_zeroelim(dalen, da, cez, temp32a);
    temp32blen = scale_expansion_zeroelim(dalen, da, ceztail, temp32b);
    temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64a);
    temp32alen = scale_expansion_zeroelim(aclen, ac, dez, temp32a);
    temp32blen = scale_expansion_zeroelim(aclen, ac, deztail, temp32b);
    temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64b);
    temp32alen = scale_expansion_zeroelim(cdlen, cd, aez, temp32a);
    temp32blen = scale_expansion_zeroelim(cdlen, cd, aeztail, temp32b);
    temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64c);
    temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
        temp64blen, temp64b, temp128);
    temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
        temp128len, temp128, temp192);
    xlen = scale_expansion_zeroelim(temp192len, temp192, bex, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, bex, detxx);
    xtlen = scale_expansion_zeroelim(temp192len, temp192, bextail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, bex, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, bextail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
    ylen = scale_expansion_zeroelim(temp192len, temp192, bey, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, bey, detyy);
    ytlen = scale_expansion_zeroelim(temp192len, temp192, beytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, bey, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, beytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
    zlen = scale_expansion_zeroelim(temp192len, temp192, bez, detz);
    zzlen = scale_expansion_zeroelim(zlen, detz, bez, detzz);
    ztlen = scale_expansion_zeroelim(temp192len, temp192, beztail, detzt);
    zztlen = scale_expansion_zeroelim(ztlen, detzt, bez, detzzt);
    for (i = 0; i < zztlen; i++) {
        detzzt[i] *= 2.0;
    }
    ztztlen = scale_expansion_zeroelim(ztlen, detzt, beztail, detztzt);
    z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
    z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
    xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
    blen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, bdet);

    temp32alen = scale_expansion_zeroelim(ablen, ab, -dez, temp32a);
    temp32blen = scale_expansion_zeroelim(ablen, ab, -deztail, temp32b);
    temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64a);
    temp32alen = scale_expansion_zeroelim(bdlen, bd, -aez, temp32a);
    temp32blen = scale_expansion_zeroelim(bdlen, bd, -aeztail, temp32b);
    temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64b);
    temp32alen = scale_expansion_zeroelim(dalen, da, -bez, temp32a);
    temp32blen = scale_expansion_zeroelim(dalen, da, -beztail, temp32b);
    temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64c);
    temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
        temp64blen, temp64b, temp128);
    temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
        temp128len, temp128, temp192);
    xlen = scale_expansion_zeroelim(temp192len, temp192, cex, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, cex, detxx);
    xtlen = scale_expansion_zeroelim(temp192len, temp192, cextail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, cex, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, cextail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
    ylen = scale_expansion_zeroelim(temp192len, temp192, cey, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, cey, detyy);
    ytlen = scale_expansion_zeroelim(temp192len, temp192, ceytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, cey, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, ceytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
    zlen = scale_expansion_zeroelim(temp192len, temp192, cez, detz);
    zzlen = scale_expansion_zeroelim(zlen, detz, cez, detzz);
    ztlen = scale_expansion_zeroelim(temp192len, temp192, ceztail, detzt);
    zztlen = scale_expansion_zeroelim(ztlen, detzt, cez, detzzt);
    for (i = 0; i < zztlen; i++) {
        detzzt[i] *= 2.0;
    }
    ztztlen = scale_expansion_zeroelim(ztlen, detzt, ceztail, detztzt);
    z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
    z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
    xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
    clen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, cdet);

    temp32alen = scale_expansion_zeroelim(bclen, bc, aez, temp32a);
    temp32blen = scale_expansion_zeroelim(bclen, bc, aeztail, temp32b);
    temp64alen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64a);
    temp32alen = scale_expansion_zeroelim(aclen, ac, -bez, temp32a);
    temp32blen = scale_expansion_zeroelim(aclen, ac, -beztail, temp32b);
    temp64blen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64b);
    temp32alen = scale_expansion_zeroelim(ablen, ab, cez, temp32a);
    temp32blen = scale_expansion_zeroelim(ablen, ab, ceztail, temp32b);
    temp64clen = fast_expansion_sum_zeroelim(temp32alen, temp32a,
        temp32blen, temp32b, temp64c);
    temp128len = fast_expansion_sum_zeroelim(temp64alen, temp64a,
        temp64blen, temp64b, temp128);
    temp192len = fast_expansion_sum_zeroelim(temp64clen, temp64c,
        temp128len, temp128, temp192);
    xlen = scale_expansion_zeroelim(temp192len, temp192, dex, detx);
    xxlen = scale_expansion_zeroelim(xlen, detx, dex, detxx);
    xtlen = scale_expansion_zeroelim(temp192len, temp192, dextail, detxt);
    xxtlen = scale_expansion_zeroelim(xtlen, detxt, dex, detxxt);
    for (i = 0; i < xxtlen; i++) {
        detxxt[i] *= 2.0;
    }
    xtxtlen = scale_expansion_zeroelim(xtlen, detxt, dextail, detxtxt);
    x1len = fast_expansion_sum_zeroelim(xxlen, detxx, xxtlen, detxxt, x1);
    x2len = fast_expansion_sum_zeroelim(x1len, x1, xtxtlen, detxtxt, x2);
    ylen = scale_expansion_zeroelim(temp192len, temp192, dey, dety);
    yylen = scale_expansion_zeroelim(ylen, dety, dey, detyy);
    ytlen = scale_expansion_zeroelim(temp192len, temp192, deytail, detyt);
    yytlen = scale_expansion_zeroelim(ytlen, detyt, dey, detyyt);
    for (i = 0; i < yytlen; i++) {
        detyyt[i] *= 2.0;
    }
    ytytlen = scale_expansion_zeroelim(ytlen, detyt, deytail, detytyt);
    y1len = fast_expansion_sum_zeroelim(yylen, detyy, yytlen, detyyt, y1);
    y2len = fast_expansion_sum_zeroelim(y1len, y1, ytytlen, detytyt, y2);
    zlen = scale_expansion_zeroelim(temp192len, temp192, dez, detz);
    zzlen = scale_expansion_zeroelim(zlen, detz, dez, detzz);
    ztlen = scale_expansion_zeroelim(temp192len, temp192, deztail, detzt);
    zztlen = scale_expansion_zeroelim(ztlen, detzt, dez, detzzt);
    for (i = 0; i < zztlen; i++) {
        detzzt[i] *= 2.0;
    }
    ztztlen = scale_expansion_zeroelim(ztlen, detzt, deztail, detztzt);
    z1len = fast_expansion_sum_zeroelim(zzlen, detzz, zztlen, detzzt, z1);
    z2len = fast_expansion_sum_zeroelim(z1len, z1, ztztlen, detztzt, z2);
    xylen = fast_expansion_sum_zeroelim(x2len, x2, y2len, y2, detxy);
    dlen = fast_expansion_sum_zeroelim(z2len, z2, xylen, detxy, ddet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
    deterlen = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, deter);

    return deter[deterlen - 1];
}

REAL insphereadapt(pa, pb, pc, pd, pe, permanent)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL* pe;
REAL permanent;
{
    INEXACT REAL aex, bex, cex, dex, aey, bey, cey, dey, aez, bez, cez, dez;
    REAL det, errbound;

    INEXACT REAL aexbey1, bexaey1, bexcey1, cexbey1;
    INEXACT REAL cexdey1, dexcey1, dexaey1, aexdey1;
    INEXACT REAL aexcey1, cexaey1, bexdey1, dexbey1;
    REAL aexbey0, bexaey0, bexcey0, cexbey0;
    REAL cexdey0, dexcey0, dexaey0, aexdey0;
    REAL aexcey0, cexaey0, bexdey0, dexbey0;
    REAL ab[4], bc[4], cd[4], da[4], ac[4], bd[4];
    INEXACT REAL ab3, bc3, cd3, da3, ac3, bd3;
    REAL abeps, bceps, cdeps, daeps, aceps, bdeps;
    REAL temp8a[8], temp8b[8], temp8c[8], temp16[16], temp24[24], temp48[48];
    int temp8alen, temp8blen, temp8clen, temp16len, temp24len, temp48len;
    REAL xdet[96], ydet[96], zdet[96], xydet[192];
    int xlen, ylen, zlen, xylen;
    REAL adet[288], bdet[288], cdet[288], ddet[288];
    int alen, blen, clen, dlen;
    REAL abdet[576], cddet[576];
    int ablen, cdlen;
    REAL fin1[1152];
    int finlength;

    REAL aextail, bextail, cextail, dextail;
    REAL aeytail, beytail, ceytail, deytail;
    REAL aeztail, beztail, ceztail, deztail;

    INEXACT REAL bvirt;
    REAL avirt, bround, around;
    INEXACT REAL c;
    INEXACT REAL abig;
    REAL ahi, alo, bhi, blo;
    REAL err1, err2, err3;
    INEXACT REAL _i, _j;
    REAL _0;

    aex = (REAL)(pa[0] - pe[0]);
    bex = (REAL)(pb[0] - pe[0]);
    cex = (REAL)(pc[0] - pe[0]);
    dex = (REAL)(pd[0] - pe[0]);
    aey = (REAL)(pa[1] - pe[1]);
    bey = (REAL)(pb[1] - pe[1]);
    cey = (REAL)(pc[1] - pe[1]);
    dey = (REAL)(pd[1] - pe[1]);
    aez = (REAL)(pa[2] - pe[2]);
    bez = (REAL)(pb[2] - pe[2]);
    cez = (REAL)(pc[2] - pe[2]);
    dez = (REAL)(pd[2] - pe[2]);

    Two_Product(aex, bey, aexbey1, aexbey0);
    Two_Product(bex, aey, bexaey1, bexaey0);
    Two_Two_Diff(aexbey1, aexbey0, bexaey1, bexaey0, ab3, ab[2], ab[1], ab[0]);
    ab[3] = ab3;

    Two_Product(bex, cey, bexcey1, bexcey0);
    Two_Product(cex, bey, cexbey1, cexbey0);
    Two_Two_Diff(bexcey1, bexcey0, cexbey1, cexbey0, bc3, bc[2], bc[1], bc[0]);
    bc[3] = bc3;

    Two_Product(cex, dey, cexdey1, cexdey0);
    Two_Product(dex, cey, dexcey1, dexcey0);
    Two_Two_Diff(cexdey1, cexdey0, dexcey1, dexcey0, cd3, cd[2], cd[1], cd[0]);
    cd[3] = cd3;

    Two_Product(dex, aey, dexaey1, dexaey0);
    Two_Product(aex, dey, aexdey1, aexdey0);
    Two_Two_Diff(dexaey1, dexaey0, aexdey1, aexdey0, da3, da[2], da[1], da[0]);
    da[3] = da3;

    Two_Product(aex, cey, aexcey1, aexcey0);
    Two_Product(cex, aey, cexaey1, cexaey0);
    Two_Two_Diff(aexcey1, aexcey0, cexaey1, cexaey0, ac3, ac[2], ac[1], ac[0]);
    ac[3] = ac3;

    Two_Product(bex, dey, bexdey1, bexdey0);
    Two_Product(dex, bey, dexbey1, dexbey0);
    Two_Two_Diff(bexdey1, bexdey0, dexbey1, dexbey0, bd3, bd[2], bd[1], bd[0]);
    bd[3] = bd3;

    temp8alen = scale_expansion_zeroelim(4, cd, bez, temp8a);
    temp8blen = scale_expansion_zeroelim(4, bd, -cez, temp8b);
    temp8clen = scale_expansion_zeroelim(4, bc, dez, temp8c);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
        temp8blen, temp8b, temp16);
    temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
        temp16len, temp16, temp24);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, aex, temp48);
    xlen = scale_expansion_zeroelim(temp48len, temp48, -aex, xdet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, aey, temp48);
    ylen = scale_expansion_zeroelim(temp48len, temp48, -aey, ydet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, aez, temp48);
    zlen = scale_expansion_zeroelim(temp48len, temp48, -aez, zdet);
    xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
    alen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, adet);

    temp8alen = scale_expansion_zeroelim(4, da, cez, temp8a);
    temp8blen = scale_expansion_zeroelim(4, ac, dez, temp8b);
    temp8clen = scale_expansion_zeroelim(4, cd, aez, temp8c);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
        temp8blen, temp8b, temp16);
    temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
        temp16len, temp16, temp24);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, bex, temp48);
    xlen = scale_expansion_zeroelim(temp48len, temp48, bex, xdet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, bey, temp48);
    ylen = scale_expansion_zeroelim(temp48len, temp48, bey, ydet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, bez, temp48);
    zlen = scale_expansion_zeroelim(temp48len, temp48, bez, zdet);
    xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
    blen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, bdet);

    temp8alen = scale_expansion_zeroelim(4, ab, dez, temp8a);
    temp8blen = scale_expansion_zeroelim(4, bd, aez, temp8b);
    temp8clen = scale_expansion_zeroelim(4, da, bez, temp8c);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
        temp8blen, temp8b, temp16);
    temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
        temp16len, temp16, temp24);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, cex, temp48);
    xlen = scale_expansion_zeroelim(temp48len, temp48, -cex, xdet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, cey, temp48);
    ylen = scale_expansion_zeroelim(temp48len, temp48, -cey, ydet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, cez, temp48);
    zlen = scale_expansion_zeroelim(temp48len, temp48, -cez, zdet);
    xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
    clen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, cdet);

    temp8alen = scale_expansion_zeroelim(4, bc, aez, temp8a);
    temp8blen = scale_expansion_zeroelim(4, ac, -bez, temp8b);
    temp8clen = scale_expansion_zeroelim(4, ab, cez, temp8c);
    temp16len = fast_expansion_sum_zeroelim(temp8alen, temp8a,
        temp8blen, temp8b, temp16);
    temp24len = fast_expansion_sum_zeroelim(temp8clen, temp8c,
        temp16len, temp16, temp24);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, dex, temp48);
    xlen = scale_expansion_zeroelim(temp48len, temp48, dex, xdet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, dey, temp48);
    ylen = scale_expansion_zeroelim(temp48len, temp48, dey, ydet);
    temp48len = scale_expansion_zeroelim(temp24len, temp24, dez, temp48);
    zlen = scale_expansion_zeroelim(temp48len, temp48, dez, zdet);
    xylen = fast_expansion_sum_zeroelim(xlen, xdet, ylen, ydet, xydet);
    dlen = fast_expansion_sum_zeroelim(xylen, xydet, zlen, zdet, ddet);

    ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    cdlen = fast_expansion_sum_zeroelim(clen, cdet, dlen, ddet, cddet);
    finlength = fast_expansion_sum_zeroelim(ablen, abdet, cdlen, cddet, fin1);

    det = estimate(finlength, fin1);
    errbound = isperrboundB * permanent;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    Two_Diff_Tail(pa[0], pe[0], aex, aextail);
    Two_Diff_Tail(pa[1], pe[1], aey, aeytail);
    Two_Diff_Tail(pa[2], pe[2], aez, aeztail);
    Two_Diff_Tail(pb[0], pe[0], bex, bextail);
    Two_Diff_Tail(pb[1], pe[1], bey, beytail);
    Two_Diff_Tail(pb[2], pe[2], bez, beztail);
    Two_Diff_Tail(pc[0], pe[0], cex, cextail);
    Two_Diff_Tail(pc[1], pe[1], cey, ceytail);
    Two_Diff_Tail(pc[2], pe[2], cez, ceztail);
    Two_Diff_Tail(pd[0], pe[0], dex, dextail);
    Two_Diff_Tail(pd[1], pe[1], dey, deytail);
    Two_Diff_Tail(pd[2], pe[2], dez, deztail);
    if ((aextail == 0.0) && (aeytail == 0.0) && (aeztail == 0.0)
        && (bextail == 0.0) && (beytail == 0.0) && (beztail == 0.0)
        && (cextail == 0.0) && (ceytail == 0.0) && (ceztail == 0.0)
        && (dextail == 0.0) && (deytail == 0.0) && (deztail == 0.0)) {
        return det;
    }

    errbound = isperrboundC * permanent + resulterrbound * Absolute(det);
    abeps = (aex * beytail + bey * aextail)
        - (aey * bextail + bex * aeytail);
    bceps = (bex * ceytail + cey * bextail)
        - (bey * cextail + cex * beytail);
    cdeps = (cex * deytail + dey * cextail)
        - (cey * dextail + dex * ceytail);
    daeps = (dex * aeytail + aey * dextail)
        - (dey * aextail + aex * deytail);
    aceps = (aex * ceytail + cey * aextail)
        - (aey * cextail + cex * aeytail);
    bdeps = (bex * deytail + dey * bextail)
        - (bey * dextail + dex * beytail);
    det += (((bex * bex + bey * bey + bez * bez)
                    * ((cez * daeps + dez * aceps + aez * cdeps)
                        + (ceztail * da3 + deztail * ac3 + aeztail * cd3))
                + (dex * dex + dey * dey + dez * dez)
                    * ((aez * bceps - bez * aceps + cez * abeps)
                        + (aeztail * bc3 - beztail * ac3 + ceztail * ab3)))
               - ((aex * aex + aey * aey + aez * aez)
                       * ((bez * cdeps - cez * bdeps + dez * bceps)
                           + (beztail * cd3 - ceztail * bd3 + deztail * bc3))
                   + (cex * cex + cey * cey + cez * cez)
                       * ((dez * abeps + aez * bdeps + bez * daeps)
                           + (deztail * ab3 + aeztail * bd3 + beztail * da3))))
        + 2.0 * (((bex * bextail + bey * beytail + bez * beztail) * (cez * da3 + dez * ac3 + aez * cd3) + (dex * dextail + dey * deytail + dez * deztail) * (aez * bc3 - bez * ac3 + cez * ab3)) - ((aex * aextail + aey * aeytail + aez * aeztail) * (bez * cd3 - cez * bd3 + dez * bc3) + (cex * cextail + cey * ceytail + cez * ceztail) * (dez * ab3 + aez * bd3 + bez * da3)));
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    return insphereexact(pa, pb, pc, pd, pe);
}

REAL insphere(pa, pb, pc, pd, pe)
    REAL* pa;
REAL* pb;
REAL* pc;
REAL* pd;
REAL* pe;
{
    REAL aex, bex, cex, dex;
    REAL aey, bey, cey, dey;
    REAL aez, bez, cez, dez;
    REAL aexbey, bexaey, bexcey, cexbey, cexdey, dexcey, dexaey, aexdey;
    REAL aexcey, cexaey, bexdey, dexbey;
    REAL alift, blift, clift, dlift;
    REAL ab, bc, cd, da, ac, bd;
    REAL abc, bcd, cda, dab;
    REAL aezplus, bezplus, cezplus, dezplus;
    REAL aexbeyplus, bexaeyplus, bexceyplus, cexbeyplus;
    REAL cexdeyplus, dexceyplus, dexaeyplus, aexdeyplus;
    REAL aexceyplus, cexaeyplus, bexdeyplus, dexbeyplus;
    REAL det;
    REAL permanent, errbound;

    aex = pa[0] - pe[0];
    bex = pb[0] - pe[0];
    cex = pc[0] - pe[0];
    dex = pd[0] - pe[0];
    aey = pa[1] - pe[1];
    bey = pb[1] - pe[1];
    cey = pc[1] - pe[1];
    dey = pd[1] - pe[1];
    aez = pa[2] - pe[2];
    bez = pb[2] - pe[2];
    cez = pc[2] - pe[2];
    dez = pd[2] - pe[2];

    aexbey = aex * bey;
    bexaey = bex * aey;
    ab = aexbey - bexaey;
    bexcey = bex * cey;
    cexbey = cex * bey;
    bc = bexcey - cexbey;
    cexdey = cex * dey;
    dexcey = dex * cey;
    cd = cexdey - dexcey;
    dexaey = dex * aey;
    aexdey = aex * dey;
    da = dexaey - aexdey;

    aexcey = aex * cey;
    cexaey = cex * aey;
    ac = aexcey - cexaey;
    bexdey = bex * dey;
    dexbey = dex * bey;
    bd = bexdey - dexbey;

    abc = aez * bc - bez * ac + cez * ab;
    bcd = bez * cd - cez * bd + dez * bc;
    cda = cez * da + dez * ac + aez * cd;
    dab = dez * ab + aez * bd + bez * da;

    alift = aex * aex + aey * aey + aez * aez;
    blift = bex * bex + bey * bey + bez * bez;
    clift = cex * cex + cey * cey + cez * cez;
    dlift = dex * dex + dey * dey + dez * dez;

    det = (dlift * abc - clift * dab) + (blift * cda - alift * bcd);

    aezplus = Absolute(aez);
    bezplus = Absolute(bez);
    cezplus = Absolute(cez);
    dezplus = Absolute(dez);
    aexbeyplus = Absolute(aexbey);
    bexaeyplus = Absolute(bexaey);
    bexceyplus = Absolute(bexcey);
    cexbeyplus = Absolute(cexbey);
    cexdeyplus = Absolute(cexdey);
    dexceyplus = Absolute(dexcey);
    dexaeyplus = Absolute(dexaey);
    aexdeyplus = Absolute(aexdey);
    aexceyplus = Absolute(aexcey);
    cexaeyplus = Absolute(cexaey);
    bexdeyplus = Absolute(bexdey);
    dexbeyplus = Absolute(dexbey);
    permanent = ((cexdeyplus + dexceyplus) * bezplus
                    + (dexbeyplus + bexdeyplus) * cezplus
                    + (bexceyplus + cexbeyplus) * dezplus)
            * alift
        + ((dexaeyplus + aexdeyplus) * cezplus
              + (aexceyplus + cexaeyplus) * dezplus
              + (cexdeyplus + dexceyplus) * aezplus)
            * blift
        + ((aexbeyplus + bexaeyplus) * dezplus
              + (bexdeyplus + dexbeyplus) * aezplus
              + (dexaeyplus + aexdeyplus) * bezplus)
            * clift
        + ((bexceyplus + cexbeyplus) * aezplus
              + (cexaeyplus + aexceyplus) * bezplus
              + (aexbeyplus + bexaeyplus) * cezplus)
            * dlift;
    errbound = isperrboundA * permanent;
    if ((det > errbound) || (-det > errbound)) {
        return det;
    }

    return insphereadapt(pa, pb, pc, pd, pe, permanent);
}
#endif

#endif
