/* mpfr.h -- Include file for mpfr.

Copyright 1999-2025 Free Software Foundation, Inc.
Contributed by the Pascaline and Caramba projects, INRIA.

This file is part of the GNU MPFR Library.

The GNU MPFR Library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3 of the License, or (at your
option) any later version.

The GNU MPFR Library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with the GNU MPFR Library; see the file COPYING.LESSER.
If not, see <https://www.gnu.org/licenses/>. */

#ifndef __MPFR_H
#define __MPFR_H

/* Define MPFR version number */
#define MPFR_VERSION_MAJOR 4
#define MPFR_VERSION_MINOR 2
#define MPFR_VERSION_PATCHLEVEL 2
#define MPFR_VERSION_STRING "4.2.2"

/* User macros:
   MPFR_USE_FILE:        Define it to make MPFR define functions dealing
                         with FILE* (auto-detect).
   MPFR_USE_INTMAX_T:    Define it to make MPFR define functions dealing
                         with intmax_t (auto-detect).
   MPFR_USE_VA_LIST:     Define it to make MPFR define functions dealing
                         with va_list (auto-detect).
   MPFR_USE_C99_FEATURE: Define it to 1 to make MPFR support C99-feature
                         (auto-detect), to 0 to bypass the detection.
   MPFR_USE_EXTENSION:   Define it to make MPFR use GCC extension to
                         reduce warnings.
   MPFR_USE_NO_MACRO:    Define it to make MPFR remove any overriding
                         function macro.
*/

/* Macros dealing with MPFR VERSION */
#define MPFR_VERSION_NUM(a,b,c) (((a) << 16L) | ((b) << 8) | (c))
#define MPFR_VERSION \
MPFR_VERSION_NUM(MPFR_VERSION_MAJOR,MPFR_VERSION_MINOR,MPFR_VERSION_PATCHLEVEL)

#ifndef MPFR_USE_MINI_GMP
#include <gmp.h>
#else
#include <mini-gmp.h>
#endif

/* Avoid some problems with macro expansion if the user defines macros
   with the same name as keywords. By convention, identifiers and macro
   names starting with mpfr_ are reserved by MPFR. */
typedef void            mpfr_void;
typedef int             mpfr_int;
typedef unsigned int    mpfr_uint;
typedef long            mpfr_long;
typedef unsigned long   mpfr_ulong;
typedef size_t          mpfr_size_t;

/* Global (possibly TLS) flags. Might also be used in an mpfr_t in the
   future (there would be room as mpfr_sign_t just needs 1 byte).
   TODO: The tests currently assume that the flags fits in an unsigned int;
   this should be cleaned up, e.g. by defining a function that outputs the
   flags as a string or by using the flags_out function (from tests/tests.c
   directly). */
typedef unsigned int    mpfr_flags_t;

/* Flags macros (in the public API) */
#define MPFR_FLAGS_UNDERFLOW 1
#define MPFR_FLAGS_OVERFLOW 2
#define MPFR_FLAGS_NAN 4
#define MPFR_FLAGS_INEXACT 8
#define MPFR_FLAGS_ERANGE 16
#define MPFR_FLAGS_DIVBY0 32
#define MPFR_FLAGS_ALL (MPFR_FLAGS_UNDERFLOW | \
                        MPFR_FLAGS_OVERFLOW  | \
                        MPFR_FLAGS_NAN       | \
                        MPFR_FLAGS_INEXACT   | \
                        MPFR_FLAGS_ERANGE    | \
                        MPFR_FLAGS_DIVBY0)

/* Definition of rounding modes (DON'T USE MPFR_RNDNA!).
   Warning! Changing the contents of this enum should be seen as an
   interface change since the old and the new types are not compatible
   (the integer type compatible with the enumerated type can even change,
   see ISO C99, 6.7.2.2#4), and in Makefile.am, AGE should be set to 0.

   MPFR_RNDU must appear just before MPFR_RNDD (see
   MPFR_IS_RNDUTEST_OR_RNDDNOTTEST in mpfr-impl.h).

   If you change the order of the rounding modes, please update the routines
   in texceptions.c which assume 0=RNDN, 1=RNDZ, 2=RNDU, 3=RNDD, 4=RNDA.
*/
typedef enum {
  MPFR_RNDN=0,  /* round to nearest, with ties to even */
  MPFR_RNDZ,    /* round toward zero */
  MPFR_RNDU,    /* round toward +Inf */
  MPFR_RNDD,    /* round toward -Inf */
  MPFR_RNDA,    /* round away from zero */
  MPFR_RNDF,    /* faithful rounding */
  MPFR_RNDNA=-1 /* round to nearest, with ties away from zero (mpfr_round) */
} mpfr_rnd_t;

/* kept for compatibility with MPFR 2.4.x and before */
#define GMP_RNDN MPFR_RNDN
#define GMP_RNDZ MPFR_RNDZ
#define GMP_RNDU MPFR_RNDU
#define GMP_RNDD MPFR_RNDD

/* The _MPFR_PREC_FORMAT and _MPFR_EXP_FORMAT values are automatically
   defined below. You MUST NOT force a value (this will break the ABI),
   possibly except for a very particular use, in which case you also need
   to rebuild the MPFR library with the chosen values; do not install this
   rebuilt library in a path that is searched by default, otherwise this
   will break applications that are dynamically linked with MPFR.

   Using non-default values is not guaranteed to work.

   Note: With the following default choices for _MPFR_PREC_FORMAT and
   _MPFR_EXP_FORMAT, mpfr_exp_t will be the same as [mp_exp_t] (at least
   up to GMP 6). */

/* Define precision: 1 (short), 2 (int) or 3 (long).
   DON'T FORCE A VALUE (see above). */
#ifndef _MPFR_PREC_FORMAT
# if __GMP_MP_SIZE_T_INT
#  define _MPFR_PREC_FORMAT 2
# else
#  define _MPFR_PREC_FORMAT 3
# endif
#endif

/* Define exponent: 1 (short), 2 (int), 3 (long) or 4 (intmax_t).
   DON'T FORCE A VALUE (see above). */
#ifndef _MPFR_EXP_FORMAT
# define _MPFR_EXP_FORMAT _MPFR_PREC_FORMAT
#endif

#if _MPFR_PREC_FORMAT > _MPFR_EXP_FORMAT
# error "mpfr_prec_t must not be larger than mpfr_exp_t"
#endif

/* Let's make mpfr_prec_t signed in order to avoid problems due to the
   usual arithmetic conversions when mixing mpfr_prec_t and mpfr_exp_t
   in an expression (for error analysis) if casts are forgotten.
   Note: mpfr_prec_t is currently limited to "long". This means that
   under MS Windows, the precisions are limited to about 2^31; however,
   these are already huge precisions, probably sufficient in practice
   on this platform. */
#if   _MPFR_PREC_FORMAT == 1
typedef short mpfr_prec_t;
typedef unsigned short mpfr_uprec_t;
#elif _MPFR_PREC_FORMAT == 2
typedef int   mpfr_prec_t;
typedef unsigned int   mpfr_uprec_t;
#elif _MPFR_PREC_FORMAT == 3
typedef long  mpfr_prec_t;
typedef unsigned long  mpfr_uprec_t;
#else
# error "Invalid MPFR Prec format"
#endif

/* Definition of precision limits without needing <limits.h> */
/* Note: The casts allows the expression to yield the wanted behavior
   for _MPFR_PREC_FORMAT == 1 (due to integer promotion rules). We
   also make sure that MPFR_PREC_MIN and MPFR_PREC_MAX have a signed
   integer type. The "- 256" allows more security, avoiding some
   integer overflows in extreme cases; ideally it should be useless. */
#define MPFR_PREC_MIN 1
#define MPFR_PREC_MAX ((mpfr_prec_t) ((((mpfr_uprec_t) -1) >> 1) - 256))

/* Definition of sign */
typedef int          mpfr_sign_t;

/* Definition of the exponent. _MPFR_EXP_FORMAT must be large enough
   so that mpfr_exp_t has at least 32 bits. */
#if   _MPFR_EXP_FORMAT == 1
typedef short mpfr_exp_t;
typedef unsigned short mpfr_uexp_t;
#elif _MPFR_EXP_FORMAT == 2
typedef int mpfr_exp_t;
typedef unsigned int mpfr_uexp_t;
#elif _MPFR_EXP_FORMAT == 3
typedef long mpfr_exp_t;
typedef unsigned long mpfr_uexp_t;
#elif _MPFR_EXP_FORMAT == 4
/* Note: in this case, intmax_t and uintmax_t must be defined before
   the inclusion of mpfr.h (we do not include <stdint.h> here due to
   potential issues with non-ISO implementations, on which there are
   alternative ways to define these types).
   In all known implementations, intmax_t has exactly 64 bits and is
   equivalent to long long when defined, but when long has 64 bits,
   it may be defined as long by <stdint.h> for better portability
   with old compilers, thus offers more flexibility than long long.
   This may change in the future.
   This _MPFR_EXP_FORMAT value is currently not supported since the
   MPFR code assumes that mpfr_exp_t fits in a long. Some examples
   of problematic code can be obtained with:
     grep -E 'mpfr_cmp_[su]i *\(.*__gmpfr_em' *.c
*/
typedef intmax_t mpfr_exp_t;
typedef uintmax_t mpfr_uexp_t;
#else
# error "Invalid MPFR Exp format"
#endif

/* Definition of the standard exponent limits */
#define MPFR_EMAX_DEFAULT ((mpfr_exp_t) (((mpfr_ulong) 1 << 30) - 1))
#define MPFR_EMIN_DEFAULT (-(MPFR_EMAX_DEFAULT))

/* DON'T USE THIS! (For MPFR-public macros only, see below.)
   The mpfr_sgn macro uses the fact that __MPFR_EXP_NAN and __MPFR_EXP_ZERO
   are the smallest values. For a n-bit type, EXP_MAX is 2^(n-1)-1,
   EXP_ZERO is 1-2^(n-1), EXP_NAN is 2-2^(n-1), EXP_INF is 3-2^(n-1).
   This may change in the future. MPFR code should not be based on these
   representations (but if this is absolutely needed, protect the code
   with a static assertion). */
#define __MPFR_EXP_MAX ((mpfr_exp_t) (((mpfr_uexp_t) -1) >> 1))
#define __MPFR_EXP_NAN  (1 - __MPFR_EXP_MAX)
#define __MPFR_EXP_ZERO (0 - __MPFR_EXP_MAX)
#define __MPFR_EXP_INF  (2 - __MPFR_EXP_MAX)

/* Definition of the main structure */
typedef struct {
  mpfr_prec_t  _mpfr_prec;
  mpfr_sign_t  _mpfr_sign;
  mpfr_exp_t   _mpfr_exp;
  mp_limb_t   *_mpfr_d;
} __mpfr_struct;

/* Compatibility with previous types of MPFR */
#ifndef mp_rnd_t
# define mp_rnd_t  mpfr_rnd_t
#endif
#ifndef mp_prec_t
# define mp_prec_t mpfr_prec_t
#endif

/*
   The represented number is
      _sign*(_d[k-1]/B+_d[k-2]/B^2+...+_d[0]/B^k)*2^_exp
   where k=ceil(_mp_prec/GMP_NUMB_BITS) and B=2^GMP_NUMB_BITS.

   For the msb (most significant bit) normalized representation, we must have
      _d[k-1]>=B/2, unless the number is singular.

   We must also have the last k*GMP_NUMB_BITS-_prec bits set to zero.
*/

typedef __mpfr_struct mpfr_t[1];
typedef __mpfr_struct *mpfr_ptr;
typedef const __mpfr_struct *mpfr_srcptr;

/* For those who need a direct and fast access to the sign field.
   However, it is not in the API, thus use it at your own risk: it
   might not be supported, or change name, in further versions!
   Unfortunately, it must be defined here (instead of MPFR's internal
   header file mpfr-impl.h) because it is used by some macros below.
*/
#define MPFR_SIGN(x) ((x)->_mpfr_sign)

/* Custom interface */
typedef enum {
  MPFR_NAN_KIND     = 0,
  MPFR_INF_KIND     = 1,
  MPFR_ZERO_KIND    = 2,
  MPFR_REGULAR_KIND = 3
} mpfr_kind_t;

/* Free cache policy */
typedef enum {
  MPFR_FREE_LOCAL_CACHE  = 1,  /* 1 << 0 */
  MPFR_FREE_GLOBAL_CACHE = 2   /* 1 << 1 */
} mpfr_free_cache_t;

/* GMP defines:
    + size_t:                Standard size_t
    + __GMP_NOTHROW          For C++: can't throw .
    + __GMP_EXTERN_INLINE    Attribute for inline function.
    + __GMP_DECLSPEC_EXPORT  compiling to go into a DLL
    + __GMP_DECLSPEC_IMPORT  compiling to go into a application
*/
/* Extra MPFR defines */
#define __MPFR_SENTINEL_ATTR
#if defined (__GNUC__)
# if __GNUC__ >= 4
#  undef __MPFR_SENTINEL_ATTR
#  define __MPFR_SENTINEL_ATTR __attribute__ ((__sentinel__))
# endif
#endif

/* If the user hasn't requested his/her preference
   and if the intention of support by the compiler is C99
   and if the compiler is known to support the C99 feature
   then we can auto-detect the C99 support as OK.
   __GNUC__ is used to detect GNU-C, ICC & CLANG compilers.
   Currently we need only variadic macros, and they are present
   since GCC >= 3. We don't test library version since we don't
   use any feature present in the library too (except intmax_t,
   but they use another detection).*/
#ifndef MPFR_USE_C99_FEATURE
# if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#  if defined (__GNUC__)
#   if __GNUC__ >= 3
#    define MPFR_USE_C99_FEATURE 1
#   endif
#  endif
# endif
# ifndef MPFR_USE_C99_FEATURE
#  define MPFR_USE_C99_FEATURE 0
# endif
#endif

/* Support for WINDOWS Dll:
   Check if we are inside a MPFR build, and if so export the functions.
   Otherwise does the same thing as GMP */
#if defined(__MPFR_WITHIN_MPFR) && __GMP_LIBGMP_DLL
# define __MPFR_DECLSPEC __GMP_DECLSPEC_EXPORT
#else
# ifndef __GMP_DECLSPEC
#  define __GMP_DECLSPEC
# endif
# define __MPFR_DECLSPEC __GMP_DECLSPEC
#endif

/* Use MPFR_DEPRECATED to mark MPFR functions, types or variables as
   deprecated. Code inspired by Apache Subversion's svn_types.h file.
   For compatibility with MSVC, MPFR_DEPRECATED must be put before
   __MPFR_DECLSPEC (not at the end of the function declaration as
   documented in the GCC manual); GCC does not seem to care.
   Moreover, in order to avoid a warning when testing such functions,
   do something like:
     +------------------------------------------
     |#ifndef _MPFR_NO_DEPRECATED_funcname
     |MPFR_DEPRECATED
     |#endif
     |__MPFR_DECLSPEC int mpfr_funcname (...);
     +------------------------------------------
   and in the corresponding test program:
     +------------------------------------------
     |#define _MPFR_NO_DEPRECATED_funcname
     |#include "mpfr-test.h"
     +------------------------------------------
*/
#if defined(__GNUC__) && \
  (__GNUC__ >= 4 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1))
# define MPFR_DEPRECATED __attribute__ ((__deprecated__))
#elif defined(_MSC_VER) && _MSC_VER >= 1300
# define MPFR_DEPRECATED __declspec(deprecated)
#else
# define MPFR_DEPRECATED
#endif
/* TODO: Also define MPFR_EXPERIMENTAL for experimental functions?
   See SVN_EXPERIMENTAL in Subversion 1.9+ as an example:
   __attribute__((__warning__("..."))) can be used with GCC 4.3.1+ but
   not __llvm__, and __declspec(deprecated("...")) can be used with
   MSC as above. */

/* ICC up to 19.1.3.304 at least declares itself as interoperable with
   GCC 4.9, but does not support the returns_nonnull attribute, thus
   outputs warning #1292. This minor issue has been reported in 2019-04:
   https://community.intel.com/t5/Intel-C-Compiler/Missing-support-for-returns-nonnull-attribute/td-p/1183013 */
#if defined(__GNUC__) && \
  (__GNUC__ >= 5 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9))
# define MPFR_RETURNS_NONNULL __attribute__ ((__returns_nonnull__))
#else
# define MPFR_RETURNS_NONNULL
#endif

/* Note: In order to be declared, some functions need a specific
   system header to be included *before* "mpfr.h". If the user
   forgets to include the header, the MPFR function prototype in
   the user object file is not correct. To avoid wrong results,
   we raise a linker error in that case by changing their internal
   name in the library (prefixed by __gmpfr instead of mpfr). See
   the lines of the form "#define mpfr_xxx __gmpfr_xxx" below. */

#if defined (__cplusplus)
extern "C" {
#endif

__MPFR_DECLSPEC MPFR_RETURNS_NONNULL const char * mpfr_get_version (void);
__MPFR_DECLSPEC MPFR_RETURNS_NONNULL const char * mpfr_get_patches (void);

__MPFR_DECLSPEC int mpfr_buildopt_tls_p          (void);
__MPFR_DECLSPEC int mpfr_buildopt_float128_p     (void);
__MPFR_DECLSPEC int mpfr_buildopt_decimal_p      (void);
__MPFR_DECLSPEC int mpfr_buildopt_gmpinternals_p (void);
__MPFR_DECLSPEC int mpfr_buildopt_sharedcache_p  (void);
__MPFR_DECLSPEC MPFR_RETURNS_NONNULL const char *
  mpfr_buildopt_tune_case (void);

__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emin     (void);
__MPFR_DECLSPEC int        mpfr_set_emin     (mpfr_exp_t);
__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emin_min (void);
__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emin_max (void);
__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emax     (void);
__MPFR_DECLSPEC int        mpfr_set_emax     (mpfr_exp_t);
__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emax_min (void);
__MPFR_DECLSPEC mpfr_exp_t mpfr_get_emax_max (void);

__MPFR_DECLSPEC void mpfr_set_default_rounding_mode (mpfr_rnd_t);
__MPFR_DECLSPEC mpfr_rnd_t mpfr_get_default_rounding_mode (void);
__MPFR_DECLSPEC const char * mpfr_print_rnd_mode (mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_clear_flags (void);
__MPFR_DECLSPEC void mpfr_clear_underflow (void);
__MPFR_DECLSPEC void mpfr_clear_overflow (void);
__MPFR_DECLSPEC void mpfr_clear_divby0 (void);
__MPFR_DECLSPEC void mpfr_clear_nanflag (void);
__MPFR_DECLSPEC void mpfr_clear_inexflag (void);
__MPFR_DECLSPEC void mpfr_clear_erangeflag (void);

__MPFR_DECLSPEC void mpfr_set_underflow (void);
__MPFR_DECLSPEC void mpfr_set_overflow (void);
__MPFR_DECLSPEC void mpfr_set_divby0 (void);
__MPFR_DECLSPEC void mpfr_set_nanflag (void);
__MPFR_DECLSPEC void mpfr_set_inexflag (void);
__MPFR_DECLSPEC void mpfr_set_erangeflag (void);

__MPFR_DECLSPEC int mpfr_underflow_p (void);
__MPFR_DECLSPEC int mpfr_overflow_p (void);
__MPFR_DECLSPEC int mpfr_divby0_p (void);
__MPFR_DECLSPEC int mpfr_nanflag_p (void);
__MPFR_DECLSPEC int mpfr_inexflag_p (void);
__MPFR_DECLSPEC int mpfr_erangeflag_p (void);

__MPFR_DECLSPEC void mpfr_flags_clear (mpfr_flags_t);
__MPFR_DECLSPEC void mpfr_flags_set (mpfr_flags_t);
__MPFR_DECLSPEC mpfr_flags_t mpfr_flags_test (mpfr_flags_t);
__MPFR_DECLSPEC mpfr_flags_t mpfr_flags_save (void);
__MPFR_DECLSPEC void mpfr_flags_restore (mpfr_flags_t,
                                         mpfr_flags_t);

__MPFR_DECLSPEC int mpfr_check_range (mpfr_ptr, int, mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_init2 (mpfr_ptr, mpfr_prec_t);
__MPFR_DECLSPEC void mpfr_init (mpfr_ptr);
__MPFR_DECLSPEC void mpfr_clear (mpfr_ptr);

__MPFR_DECLSPEC void
  mpfr_inits2 (mpfr_prec_t, mpfr_ptr, ...) __MPFR_SENTINEL_ATTR;
__MPFR_DECLSPEC void
  mpfr_inits (mpfr_ptr, ...) __MPFR_SENTINEL_ATTR;
__MPFR_DECLSPEC void
  mpfr_clears (mpfr_ptr, ...) __MPFR_SENTINEL_ATTR;

__MPFR_DECLSPEC int mpfr_prec_round (mpfr_ptr, mpfr_prec_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_can_round (mpfr_srcptr, mpfr_exp_t, mpfr_rnd_t,
                                    mpfr_rnd_t, mpfr_prec_t);
__MPFR_DECLSPEC mpfr_prec_t mpfr_min_prec (mpfr_srcptr);

__MPFR_DECLSPEC mpfr_exp_t mpfr_get_exp (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_set_exp (mpfr_ptr, mpfr_exp_t);
__MPFR_DECLSPEC mpfr_prec_t mpfr_get_prec (mpfr_srcptr);
__MPFR_DECLSPEC void mpfr_set_prec (mpfr_ptr, mpfr_prec_t);
__MPFR_DECLSPEC void mpfr_set_prec_raw (mpfr_ptr, mpfr_prec_t);
__MPFR_DECLSPEC void mpfr_set_default_prec (mpfr_prec_t);
__MPFR_DECLSPEC mpfr_prec_t mpfr_get_default_prec (void);

__MPFR_DECLSPEC int mpfr_set_d (mpfr_ptr, double, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_flt (mpfr_ptr, float, mpfr_rnd_t);
#ifdef MPFR_WANT_DECIMAL_FLOATS
/* _Decimal64 is not defined in C++,
   cf https://gcc.gnu.org/bugzilla/show_bug.cgi?id=51364 */
__MPFR_DECLSPEC int mpfr_set_decimal64 (mpfr_ptr, _Decimal64, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_decimal128 (mpfr_ptr, _Decimal128, mpfr_rnd_t);
#endif
__MPFR_DECLSPEC int mpfr_set_ld (mpfr_ptr, long double, mpfr_rnd_t);
#ifdef MPFR_WANT_FLOAT128
/* The user is free to define mpfr_float128 as another equivalent type,
   such as __float128 if this one is supported by the current compiler
   but _Float128 isn't. */
# ifndef mpfr_float128
#  define mpfr_float128 _Float128
# endif
__MPFR_DECLSPEC int mpfr_set_float128 (mpfr_ptr, mpfr_float128, mpfr_rnd_t);
__MPFR_DECLSPEC mpfr_float128 mpfr_get_float128 (mpfr_srcptr, mpfr_rnd_t);
#endif
__MPFR_DECLSPEC int mpfr_set_z (mpfr_ptr, mpz_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_z_2exp (mpfr_ptr, mpz_srcptr, mpfr_exp_t,
                                     mpfr_rnd_t);
__MPFR_DECLSPEC void mpfr_set_nan (mpfr_ptr);
__MPFR_DECLSPEC void mpfr_set_inf (mpfr_ptr, int);
__MPFR_DECLSPEC void mpfr_set_zero (mpfr_ptr, int);

#ifndef MPFR_USE_MINI_GMP
  /* mini-gmp does not provide mpf_t, we disable the following functions */
__MPFR_DECLSPEC int mpfr_set_f (mpfr_ptr, mpf_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cmp_f (mpfr_srcptr, mpf_srcptr);
__MPFR_DECLSPEC int mpfr_get_f (mpf_ptr, mpfr_srcptr, mpfr_rnd_t);
#endif
__MPFR_DECLSPEC int mpfr_set_si (mpfr_ptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_ui (mpfr_ptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_si_2exp (mpfr_ptr, long, mpfr_exp_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_ui_2exp (mpfr_ptr, unsigned long, mpfr_exp_t,
                                      mpfr_rnd_t);
#ifndef MPFR_USE_MINI_GMP
  /* mini-gmp does not provide mpq_t, we disable the following functions */
__MPFR_DECLSPEC int mpfr_set_q (mpfr_ptr, mpq_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_q (mpfr_ptr, mpfr_srcptr, mpq_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_q (mpfr_ptr, mpfr_srcptr, mpq_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_add_q (mpfr_ptr, mpfr_srcptr, mpq_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub_q (mpfr_ptr, mpfr_srcptr, mpq_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cmp_q (mpfr_srcptr, mpq_srcptr);
__MPFR_DECLSPEC void mpfr_get_q (mpq_ptr q, mpfr_srcptr f);
#endif
__MPFR_DECLSPEC int mpfr_set_str (mpfr_ptr, const char *, int, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_init_set_str (mpfr_ptr, const char *, int,
                                       mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set4 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t, int);
__MPFR_DECLSPEC int mpfr_abs (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_neg (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_signbit (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_setsign (mpfr_ptr, mpfr_srcptr, int, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_copysign (mpfr_ptr, mpfr_srcptr, mpfr_srcptr,
                                   mpfr_rnd_t);

__MPFR_DECLSPEC mpfr_exp_t mpfr_get_z_2exp (mpz_ptr, mpfr_srcptr);
__MPFR_DECLSPEC float mpfr_get_flt (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC double mpfr_get_d (mpfr_srcptr, mpfr_rnd_t);
#ifdef MPFR_WANT_DECIMAL_FLOATS
__MPFR_DECLSPEC _Decimal64 mpfr_get_decimal64 (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC _Decimal128 mpfr_get_decimal128 (mpfr_srcptr, mpfr_rnd_t);
#endif
__MPFR_DECLSPEC long double mpfr_get_ld (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC double mpfr_get_d1 (mpfr_srcptr);
__MPFR_DECLSPEC double mpfr_get_d_2exp (long*, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC long double mpfr_get_ld_2exp (long*, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_frexp (mpfr_exp_t*, mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC long mpfr_get_si (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC unsigned long mpfr_get_ui (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC size_t mpfr_get_str_ndigits (int, mpfr_prec_t);
__MPFR_DECLSPEC char * mpfr_get_str (char*, mpfr_exp_t*, int, size_t,
                                     mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_get_z (mpz_ptr z, mpfr_srcptr f, mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_free_str (char *);

__MPFR_DECLSPEC int mpfr_urandom (mpfr_ptr, gmp_randstate_t, mpfr_rnd_t);
#ifndef _MPFR_NO_DEPRECATED_GRANDOM /* for the test of this function */
MPFR_DEPRECATED
#endif
__MPFR_DECLSPEC int mpfr_grandom (mpfr_ptr, mpfr_ptr, gmp_randstate_t,
                                  mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_nrandom (mpfr_ptr, gmp_randstate_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_erandom (mpfr_ptr, gmp_randstate_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_urandomb (mpfr_ptr, gmp_randstate_t);

__MPFR_DECLSPEC void mpfr_nextabove (mpfr_ptr);
__MPFR_DECLSPEC void mpfr_nextbelow (mpfr_ptr);
__MPFR_DECLSPEC void mpfr_nexttoward (mpfr_ptr, mpfr_srcptr);

#ifndef MPFR_USE_MINI_GMP
__MPFR_DECLSPEC int mpfr_printf (const char*, ...);
__MPFR_DECLSPEC int mpfr_asprintf (char**, const char*, ...);
__MPFR_DECLSPEC int mpfr_sprintf (char*, const char*, ...);
__MPFR_DECLSPEC int mpfr_snprintf (char*, size_t, const char*, ...);
#endif

__MPFR_DECLSPEC int mpfr_pow (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_powr (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_pow_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_compound_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_pow_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_ui_pow_ui (mpfr_ptr, unsigned long, unsigned long,
                                    mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_ui_pow (mpfr_ptr, unsigned long, mpfr_srcptr,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_pow_z (mpfr_ptr, mpfr_srcptr, mpz_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_sqrt (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sqrt_ui (mpfr_ptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rec_sqrt (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_add (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_add_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_ui_sub (mpfr_ptr, unsigned long, mpfr_srcptr,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_ui_div (mpfr_ptr, unsigned long, mpfr_srcptr,
                                 mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_add_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_si_sub (mpfr_ptr, long, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_si_div (mpfr_ptr, long, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_add_d (mpfr_ptr, mpfr_srcptr, double, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub_d (mpfr_ptr, mpfr_srcptr, double, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_d_sub (mpfr_ptr, double, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_d (mpfr_ptr, mpfr_srcptr, double, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_d (mpfr_ptr, mpfr_srcptr, double, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_d_div (mpfr_ptr, double, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_sqr (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_const_pi (mpfr_ptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_const_log2 (mpfr_ptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_const_euler (mpfr_ptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_const_catalan (mpfr_ptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_agm (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_log (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log2 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log10 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log1p (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log2p1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log10p1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_log_ui (mpfr_ptr, unsigned long, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_exp (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_exp2 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_exp10 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_expm1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_exp2m1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_exp10m1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_eint (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_li2 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_cmp  (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_cmp3 (mpfr_srcptr, mpfr_srcptr, int);
__MPFR_DECLSPEC int mpfr_cmp_d (mpfr_srcptr, double);
__MPFR_DECLSPEC int mpfr_cmp_ld (mpfr_srcptr, long double);
__MPFR_DECLSPEC int mpfr_cmp_ui (mpfr_srcptr, unsigned long);
__MPFR_DECLSPEC int mpfr_cmp_si (mpfr_srcptr, long);
__MPFR_DECLSPEC int mpfr_cmp_ui_2exp (mpfr_srcptr, unsigned long, mpfr_exp_t);
__MPFR_DECLSPEC int mpfr_cmp_si_2exp (mpfr_srcptr, long, mpfr_exp_t);
__MPFR_DECLSPEC int mpfr_cmpabs (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_cmpabs_ui (mpfr_srcptr, unsigned long);
__MPFR_DECLSPEC void mpfr_reldiff (mpfr_ptr, mpfr_srcptr, mpfr_srcptr,
                                   mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_eq (mpfr_srcptr, mpfr_srcptr, unsigned long);
__MPFR_DECLSPEC int mpfr_sgn (mpfr_srcptr);

__MPFR_DECLSPEC int mpfr_mul_2exp (mpfr_ptr, mpfr_srcptr, unsigned long,
                                   mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_2exp (mpfr_ptr, mpfr_srcptr, unsigned long,
                                   mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_2ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                  mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_2ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                  mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_mul_2si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_2si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_rint (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_roundeven (mpfr_ptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_round (mpfr_ptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_trunc (mpfr_ptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_ceil (mpfr_ptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_floor (mpfr_ptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_rint_roundeven (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rint_round (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rint_trunc (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rint_ceil (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rint_floor (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_frac (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_modf (mpfr_ptr, mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_remquo (mpfr_ptr, long*, mpfr_srcptr, mpfr_srcptr,
                                 mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_remainder (mpfr_ptr, mpfr_srcptr, mpfr_srcptr,
                                    mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fmod (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fmod_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                  mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fmodquo (mpfr_ptr, long*, mpfr_srcptr, mpfr_srcptr,
                                  mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_fits_ulong_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_slong_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_uint_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_sint_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_ushort_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_sshort_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_uintmax_p (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fits_intmax_p (mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_extract (mpz_ptr, mpfr_srcptr, unsigned int);
__MPFR_DECLSPEC void mpfr_swap (mpfr_ptr, mpfr_ptr);
__MPFR_DECLSPEC void mpfr_dump (mpfr_srcptr);

__MPFR_DECLSPEC int mpfr_nan_p (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_inf_p (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_number_p (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_integer_p (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_zero_p (mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_regular_p (mpfr_srcptr);

__MPFR_DECLSPEC int mpfr_greater_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_greaterequal_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_less_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_lessequal_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_lessgreater_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_equal_p (mpfr_srcptr, mpfr_srcptr);
__MPFR_DECLSPEC int mpfr_unordered_p (mpfr_srcptr, mpfr_srcptr);

__MPFR_DECLSPEC int mpfr_atanh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_acosh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_asinh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cosh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sinh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_tanh (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sinh_cosh (mpfr_ptr, mpfr_ptr, mpfr_srcptr,
                                    mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_sech (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_csch (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_coth (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_acos (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_asin (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atan (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sin (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sin_cos (mpfr_ptr, mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cos (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_tan (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atan2 (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sec (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_csc (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cot (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_sinu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cosu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_tanu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_acosu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_asinu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atanu (mpfr_ptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atan2u (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_acospi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_asinpi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atanpi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_atan2pi (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_sinpi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cospi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_tanpi (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_hypot (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_erf (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_erfc (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cbrt (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
#ifndef _MPFR_NO_DEPRECATED_ROOT /* for the test of this function */
MPFR_DEPRECATED
#endif
__MPFR_DECLSPEC int mpfr_root (mpfr_ptr, mpfr_srcptr, unsigned long,
                               mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rootn_ui (mpfr_ptr, mpfr_srcptr, unsigned long,
                                   mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_rootn_si (mpfr_ptr, mpfr_srcptr, long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_gamma (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_gamma_inc (mpfr_ptr, mpfr_srcptr, mpfr_srcptr,
                                    mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_beta (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_lngamma (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_lgamma (mpfr_ptr, int *, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_digamma (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_zeta (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_zeta_ui (mpfr_ptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fac_ui (mpfr_ptr, unsigned long, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_j0 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_j1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_jn (mpfr_ptr, long, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_y0 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_y1 (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_yn (mpfr_ptr, long, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_ai (mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_min (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_max (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_dim (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_mul_z (mpfr_ptr, mpfr_srcptr, mpz_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_div_z (mpfr_ptr, mpfr_srcptr, mpz_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_add_z (mpfr_ptr, mpfr_srcptr, mpz_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sub_z (mpfr_ptr, mpfr_srcptr, mpz_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_z_sub (mpfr_ptr, mpz_srcptr, mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_cmp_z (mpfr_srcptr, mpz_srcptr);

__MPFR_DECLSPEC int mpfr_fma (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_srcptr,
                              mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fms (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_srcptr,
                              mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fmma (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_srcptr,
                               mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_fmms (mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_srcptr,
                               mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_sum (mpfr_ptr, const mpfr_ptr *, unsigned long,
                              mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_dot (mpfr_ptr, const mpfr_ptr *, const mpfr_ptr *,
                              unsigned long, mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_free_cache (void);
__MPFR_DECLSPEC void mpfr_free_cache2 (mpfr_free_cache_t);
__MPFR_DECLSPEC void mpfr_free_pool (void);
__MPFR_DECLSPEC int mpfr_mp_memory_cleanup (void);

__MPFR_DECLSPEC int mpfr_subnormalize (mpfr_ptr, int, mpfr_rnd_t);

__MPFR_DECLSPEC int mpfr_strtofr (mpfr_ptr, const char *, char **, int,
                                  mpfr_rnd_t);

__MPFR_DECLSPEC void mpfr_round_nearest_away_begin (mpfr_ptr);
__MPFR_DECLSPEC int mpfr_round_nearest_away_end (mpfr_ptr, int);

__MPFR_DECLSPEC size_t mpfr_custom_get_size (mpfr_prec_t);
__MPFR_DECLSPEC void mpfr_custom_init (void *, mpfr_prec_t);
__MPFR_DECLSPEC MPFR_RETURNS_NONNULL void *
  mpfr_custom_get_significand (mpfr_srcptr);
__MPFR_DECLSPEC mpfr_exp_t mpfr_custom_get_exp (mpfr_srcptr);
__MPFR_DECLSPEC void mpfr_custom_move (mpfr_ptr, void *);
__MPFR_DECLSPEC void mpfr_custom_init_set (mpfr_ptr, int, mpfr_exp_t,
                                           mpfr_prec_t, void *);
__MPFR_DECLSPEC int mpfr_custom_get_kind (mpfr_srcptr);

__MPFR_DECLSPEC int mpfr_total_order_p (mpfr_srcptr, mpfr_srcptr);

#if defined (__cplusplus)
}
#endif

/* Define MPFR_USE_EXTENSION to avoid "gcc -pedantic" warnings. */
#ifndef MPFR_EXTENSION
# if defined(MPFR_USE_EXTENSION)
#  define MPFR_EXTENSION __extension__
# else
#  define MPFR_EXTENSION
# endif
#endif

/* Warning! This macro doesn't work with K&R C (e.g., compare the "gcc -E"
   output with and without -traditional) and shouldn't be used internally.
   For public use only, but see the MPFR manual. */
#define MPFR_DECL_INIT(_x, _p)                                        \
  MPFR_EXTENSION mp_limb_t __gmpfr_local_tab_##_x[((_p)-1)/GMP_NUMB_BITS+1]; \
  MPFR_EXTENSION mpfr_t _x = {{(_p),1,__MPFR_EXP_NAN,__gmpfr_local_tab_##_x}}

#if MPFR_USE_C99_FEATURE
/* C99 & C11 version: functions with multiple inputs supported */
#define mpfr_round_nearest_away(func, rop, ...)                         \
  (mpfr_round_nearest_away_begin(rop),                                  \
   mpfr_round_nearest_away_end((rop), func((rop), __VA_ARGS__, MPFR_RNDN)))
#else
/* C90 version: function with one input supported */
#define mpfr_round_nearest_away(func, rop, op)                          \
  (mpfr_round_nearest_away_begin(rop),                                  \
   mpfr_round_nearest_away_end((rop), func((rop), (op), MPFR_RNDN)))
#endif

/* Fast access macros to replace function interface.
   If the user doesn't want to use the macro interface, let him make happy
   even if it produces faster and smaller code. */
#ifndef MPFR_USE_NO_MACRO

/* In the implementation of these macros, we need to make sure that the
   arguments are evaluated one time exactly and that type conversion is
   done as it would be with a function. Tests should be added to ensure
   that. */

/* Prevent x from being used as an lvalue.
   Thanks to Wojtek Lerch and Tim Rentsch for the idea. */
#define MPFR_VALUE_OF(x)  (0 ? (x) : (x))

/* The following macro converts the argument to mpfr_srcptr, as in type
   conversion for function parameters. But it will detect disallowed
   implicit conversions, e.g. when the argument has an integer type. */
#define MPFR_SRCPTR(x) ((mpfr_srcptr) (0 ? (x) : (mpfr_srcptr) (x)))
#define MPFR_GET_SIGN(_x) MPFR_VALUE_OF(MPFR_SIGN(MPFR_SRCPTR(_x)))

#define mpfr_nan_p(_x)      (MPFR_SRCPTR(_x)->_mpfr_exp == __MPFR_EXP_NAN)
#define mpfr_inf_p(_x)      (MPFR_SRCPTR(_x)->_mpfr_exp == __MPFR_EXP_INF)
#define mpfr_zero_p(_x)     (MPFR_SRCPTR(_x)->_mpfr_exp == __MPFR_EXP_ZERO)
#define mpfr_regular_p(_x)  (MPFR_SRCPTR(_x)->_mpfr_exp >  __MPFR_EXP_INF)

/* mpfr_sgn is documented as a macro, thus the following code is fine.
   But it would be safer to regard it as a function in some future
   MPFR version. */
#define mpfr_sgn(_x)                                               \
  ((_x)->_mpfr_exp < __MPFR_EXP_INF ?                              \
   (mpfr_nan_p (_x) ? mpfr_set_erangeflag () : (mpfr_void) 0), 0 : \
   MPFR_SIGN (_x))

#define mpfr_get_prec(_x) MPFR_VALUE_OF(MPFR_SRCPTR(_x)->_mpfr_prec)
#define mpfr_get_exp(_x)  MPFR_VALUE_OF(MPFR_SRCPTR(_x)->_mpfr_exp)
/* Note: Defining mpfr_get_exp() as a macro has the effect to disable
   the check that the argument is a pure FP number (done in the function);
   this increases the risk of undetected error and makes debugging more
   complex. Is it really worth in practice? (Potential FIXME) */

#define mpfr_round(a,b) mpfr_rint((a), (b), MPFR_RNDNA)
#define mpfr_trunc(a,b) mpfr_rint((a), (b), MPFR_RNDZ)
#define mpfr_ceil(a,b)  mpfr_rint((a), (b), MPFR_RNDU)
#define mpfr_floor(a,b) mpfr_rint((a), (b), MPFR_RNDD)

#define mpfr_cmp_ui(b,i) mpfr_cmp_ui_2exp((b),(i),0)
#define mpfr_cmp_si(b,i) mpfr_cmp_si_2exp((b),(i),0)
#if __GNUC__ > 2 || __GNUC_MINOR__ >= 95
#define mpfr_set(a,b,r)                         \
  __extension__ ({                              \
      mpfr_srcptr _p = (b);                     \
      mpfr_set4(a,_p,r,MPFR_SIGN(_p));          \
    })
#endif
#define mpfr_abs(a,b,r)  mpfr_set4(a,b,r,1)
#define mpfr_copysign(a,b,c,r) mpfr_set4(a,b,r,MPFR_GET_SIGN(c))
#define mpfr_setsign(a,b,s,r) mpfr_set4(a,b,r,(s) ? -1 : 1)
#define mpfr_signbit(x)  (MPFR_GET_SIGN(x) < 0)
#define mpfr_cmp(b, c)   mpfr_cmp3(b, c, 1)
#define mpfr_mul_2exp(y,x,n,r) mpfr_mul_2ui((y),(x),(n),(r))
#define mpfr_div_2exp(y,x,n,r) mpfr_div_2ui((y),(x),(n),(r))


/* When using GCC or ICC, optimize certain common comparisons and affectations.
   Added casts to improve robustness in case of undefined behavior and
   compiler extensions based on UB (in particular -fwrapv). MPFR doesn't
   use such extensions, but these macros will be used by 3rd-party code,
   where such extensions may be required.
   Moreover casts to unsigned long have been added to avoid warnings in
   programs that use MPFR and are compiled with -Wconversion; such casts
   are OK since if X is a constant expression, then (unsigned long) X is
   also a constant expression, so that the optimizations still work. The
   warnings are probably related to the following two bugs:
     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=4210
     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=38470 (possibly a variant)
   and the casts could be removed once these bugs are fixed.
   Casts shouldn't be used on the generic calls (to the ..._2exp functions),
   where implicit conversions are performed. Indeed, having at least one
   implicit conversion in the macro allows the compiler to emit diagnostics
   when normally expected, for instance in the following call:
     mpfr_set_ui (x, "foo", MPFR_RNDN);
   If this is not possible (for future macros), one of the tricks described on
   https://groups.google.com/g/comp.std.c/c/9Jl0giNILfg/m/e6-evyS9KukJ?pli=1
   could be used. */
#if defined (__GNUC__) && !defined(__cplusplus)
#if (__GNUC__ >= 2)

#undef mpfr_cmp_ui
/* We use the fact that mpfr_sgn on NaN sets the erange flag and returns 0.
   But warning! mpfr_sgn is specified as a macro in the API, thus the macro
   mustn't be used if side effects are possible, like here. */
#define mpfr_cmp_ui(_f,_u)                                      \
  (__builtin_constant_p (_u) && (mpfr_ulong) (_u) == 0 ?        \
   (mpfr_sgn) (_f) :                                            \
   mpfr_cmp_ui_2exp ((_f), (_u), 0))

#undef mpfr_cmp_si
#define mpfr_cmp_si(_f,_s)                                      \
  (__builtin_constant_p (_s) && (mpfr_long) (_s) >= 0 ?         \
   mpfr_cmp_ui ((_f), (mpfr_ulong) (mpfr_long) (_s)) :          \
   mpfr_cmp_si_2exp ((_f), (_s), 0))

#if __GNUC__ > 2 || __GNUC_MINOR__ >= 95
#undef mpfr_set_ui
#define mpfr_set_ui(_f,_u,_r)                                   \
  (__builtin_constant_p (_u) && (mpfr_ulong) (_u) == 0 ?        \
   __extension__ ({                                             \
       mpfr_ptr _p = (_f);                                      \
       _p->_mpfr_sign = 1;                                      \
       _p->_mpfr_exp = __MPFR_EXP_ZERO;                         \
       (mpfr_void) (_r); 0; }) :                                \
   mpfr_set_ui_2exp ((_f), (_u), 0, (_r)))
#endif

#undef mpfr_set_si
#define mpfr_set_si(_f,_s,_r)                                   \
  (__builtin_constant_p (_s) && (mpfr_long) (_s) >= 0 ?         \
   mpfr_set_ui ((_f), (mpfr_ulong) (mpfr_long) (_s), (_r)) :    \
   mpfr_set_si_2exp ((_f), (_s), 0, (_r)))

#if __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)
/* If the source is a constant number that is a power of 2,
   optimize the call */
#undef mpfr_mul_ui
#define mpfr_mul_ui(_f, _g, _u,_r)                              \
  (__builtin_constant_p (_u) && (mpfr_ulong) (_u) >= 1 &&       \
   ((mpfr_ulong) (_u) & ((mpfr_ulong) (_u) - 1)) == 0 ?         \
   mpfr_mul_2si((_f), (_g), __builtin_ctzl (_u), (_r)) :        \
   mpfr_mul_ui ((_f), (_g), (_u), (_r)))
#undef mpfr_div_ui
#define mpfr_div_ui(_f, _g, _u,_r)                              \
  (__builtin_constant_p (_u) && (mpfr_ulong) (_u) >= 1 &&       \
   ((mpfr_ulong) (_u) & ((mpfr_ulong) (_u) - 1)) == 0 ?         \
   mpfr_mul_2si((_f), (_g), - __builtin_ctzl (_u), (_r)) :      \
   mpfr_div_ui ((_f), (_g), (_u), (_r)))
#endif

/* If the source is a constant number that is non-negative,
   optimize the call */
#undef mpfr_mul_si
#define mpfr_mul_si(_f, _g, _s,_r)                                      \
  (__builtin_constant_p (_s) && (mpfr_long) (_s) >= 0 ?                 \
   mpfr_mul_ui ((_f), (_g), (mpfr_ulong) (mpfr_long) (_s), (_r)) :      \
   mpfr_mul_si ((_f), (_g), (_s), (_r)))
#undef mpfr_div_si
#define mpfr_div_si(_f, _g, _s,_r)                                      \
  (__builtin_constant_p (_s) && (mpfr_long) (_s) >= 0 ?                 \
   mpfr_div_ui ((_f), (_g), (mpfr_ulong) (mpfr_long) (_s), (_r)) :      \
   mpfr_div_si ((_f), (_g), (_s), (_r)))

#endif
#endif

/* Macro versions of the custom interface for fast access. */

/* The internal cast to mpfr_size_t will silent a warning with
   GCC's -Wsign-conversion that could occur with user code, as
   sizeof is of type size_t, which is unsigned. */
#define mpfr_custom_get_size(p)                                             \
  ((mpfr_size_t)                                                            \
   ((mpfr_size_t) (((mpfr_prec_t)(p) + GMP_NUMB_BITS - 1) / GMP_NUMB_BITS)  \
    * sizeof (mp_limb_t)))

#define mpfr_custom_init(m,p) ((void) (m), (void) (p))

#define mpfr_custom_get_significand(x) \
  ((mpfr_void *) MPFR_VALUE_OF(MPFR_SRCPTR(x)->_mpfr_d))

#define mpfr_custom_get_exp(x) MPFR_VALUE_OF(MPFR_SRCPTR(x)->_mpfr_exp)

#define mpfr_custom_move(x,m) (((mpfr_ptr) (x))->_mpfr_d = (mp_limb_t *) (m))

/* Note: the following macro is not usable in contexts where an expression
   is expected. */
#define mpfr_custom_init_set(x,k,e,p,m) do {                   \
  mpfr_ptr _x = (x);                                           \
  mpfr_exp_t _e = (e);                                         \
  mpfr_kind_t _t;                                              \
  mpfr_int _s, _k;                                             \
  _k = (k);                                                    \
  if (_k >= 0)  {                                              \
    _t = (mpfr_kind_t) _k;                                     \
    _s = 1;                                                    \
  } else {                                                     \
    _t = (mpfr_kind_t) - _k;                                   \
    _s = -1;                                                   \
  }                                                            \
  _e = _t == MPFR_REGULAR_KIND ? _e :                          \
    _t == MPFR_NAN_KIND ? __MPFR_EXP_NAN :                     \
    _t == MPFR_INF_KIND ? __MPFR_EXP_INF : __MPFR_EXP_ZERO;    \
  _x->_mpfr_prec = (p);                                        \
  _x->_mpfr_sign = _s;                                         \
  _x->_mpfr_exp  = _e;                                         \
  _x->_mpfr_d    = (mp_limb_t*) (m);                           \
 } while (0)

#if __GNUC__ > 2 || __GNUC_MINOR__ >= 95
#define mpfr_custom_get_kind(x)                                         \
  __extension__ ({                                                      \
    mpfr_srcptr _x = (x);                                               \
    _x->_mpfr_exp >  __MPFR_EXP_INF ?                                   \
      (mpfr_int) MPFR_REGULAR_KIND * MPFR_SIGN (_x)                     \
      : _x->_mpfr_exp == __MPFR_EXP_INF ?                               \
      (mpfr_int) MPFR_INF_KIND * MPFR_SIGN (_x)                         \
      : _x->_mpfr_exp == __MPFR_EXP_NAN ? (mpfr_int) MPFR_NAN_KIND      \
      : (mpfr_int) MPFR_ZERO_KIND * MPFR_SIGN (_x);                     \
  })
#else
#define mpfr_custom_get_kind(x) ((mpfr_custom_get_kind)(x))
#endif

/* End of the macro versions of the custom interface. */

#endif /* MPFR_USE_NO_MACRO */

/* These are defined to be macros */
#define mpfr_init_set_si(x, i, rnd) \
 ( mpfr_init(x), mpfr_set_si((x), (i), (rnd)) )
#define mpfr_init_set_ui(x, i, rnd) \
 ( mpfr_init(x), mpfr_set_ui((x), (i), (rnd)) )
#define mpfr_init_set_d(x, d, rnd) \
 ( mpfr_init(x), mpfr_set_d((x), (d), (rnd)) )
#define mpfr_init_set_ld(x, d, rnd) \
 ( mpfr_init(x), mpfr_set_ld((x), (d), (rnd)) )
#define mpfr_init_set_z(x, i, rnd) \
 ( mpfr_init(x), mpfr_set_z((x), (i), (rnd)) )
#ifndef MPFR_USE_MINI_GMP
#define mpfr_init_set_q(x, i, rnd) \
 ( mpfr_init(x), mpfr_set_q((x), (i), (rnd)) )
#define mpfr_init_set_f(x, y, rnd) \
 ( mpfr_init(x), mpfr_set_f((x), (y), (rnd)) )
#endif
#define mpfr_init_set(x, y, rnd) \
 ( mpfr_init(x), mpfr_set((x), (y), (rnd)) )

/* Compatibility layer -- obsolete functions and macros */
/* Note: it is not possible to output warnings, unless one defines
 * a deprecated variable and uses it, e.g.
 *   MPFR_DEPRECATED extern int mpfr_deprecated_feature;
 *   #define MPFR_EMIN_MIN ((void)mpfr_deprecated_feature,mpfr_get_emin_min())
 * (the cast to void avoids a warning because the left-hand operand
 * has no effect).
 */
#define mpfr_cmp_abs mpfr_cmpabs
#define mpfr_round_prec(x,r,p) mpfr_prec_round(x,p,r)
#define __gmp_default_rounding_mode (mpfr_get_default_rounding_mode())
#define __mpfr_emin (mpfr_get_emin())
#define __mpfr_emax (mpfr_get_emax())
#define __mpfr_default_fp_bit_precision (mpfr_get_default_fp_bit_precision())
#define MPFR_EMIN_MIN mpfr_get_emin_min()
#define MPFR_EMIN_MAX mpfr_get_emin_max()
#define MPFR_EMAX_MIN mpfr_get_emax_min()
#define MPFR_EMAX_MAX mpfr_get_emax_max()
#define mpfr_version (mpfr_get_version())
#ifndef mpz_set_fr
# define mpz_set_fr mpfr_get_z
#endif
#define mpfr_get_z_exp mpfr_get_z_2exp
#define mpfr_custom_get_mantissa mpfr_custom_get_significand

#endif /* __MPFR_H */


/* Check if <stdint.h> / <inttypes.h> is included or if the user
   explicitly wants intmax_t. Automatic detection is done by
   checking:
     - INTMAX_C and UINTMAX_C, but not if the compiler is a C++ one
       (as suggested by Patrick Pelissier) because the test does not
       work well in this case. See:
         https://sympa.inria.fr/sympa/arc/mpfr/2010-02/msg00025.html
       We do not check INTMAX_MAX and UINTMAX_MAX because under Solaris,
       these macros are always defined by <limits.h> (i.e. even when
       <stdint.h> and <inttypes.h> are not included).
     - _STDINT_H (defined by the glibc), _STDINT_H_ (defined under
       Mac OS X) and _STDINT (defined under MS Visual Studio), but
       this test may not work with all implementations.
       Portable software should not rely on these tests.
*/
#if (defined (INTMAX_C) && defined (UINTMAX_C) && !defined(__cplusplus)) || \
  defined (MPFR_USE_INTMAX_T) || \
  defined (_STDINT_H) || defined (_STDINT_H_) || defined (_STDINT) || \
  defined (_SYS_STDINT_H_) /* needed for FreeBSD */
# ifndef _MPFR_H_HAVE_INTMAX_T
# define _MPFR_H_HAVE_INTMAX_T 1

#if defined (__cplusplus)
extern "C" {
#endif

#define mpfr_set_sj __gmpfr_set_sj
#define mpfr_set_sj_2exp __gmpfr_set_sj_2exp
#define mpfr_set_uj __gmpfr_set_uj
#define mpfr_set_uj_2exp __gmpfr_set_uj_2exp
#define mpfr_get_sj __gmpfr_mpfr_get_sj
#define mpfr_get_uj __gmpfr_mpfr_get_uj
#define mpfr_pow_uj __gmpfr_mpfr_pow_uj
#define mpfr_pow_sj __gmpfr_mpfr_pow_sj
__MPFR_DECLSPEC int mpfr_set_sj (mpfr_ptr, intmax_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_sj_2exp (mpfr_ptr, intmax_t, intmax_t,
                                      mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_uj (mpfr_ptr, uintmax_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_set_uj_2exp (mpfr_ptr, uintmax_t, intmax_t,
                                      mpfr_rnd_t);
__MPFR_DECLSPEC intmax_t mpfr_get_sj (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC uintmax_t mpfr_get_uj (mpfr_srcptr, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_pow_uj (mpfr_ptr, mpfr_srcptr, uintmax_t, mpfr_rnd_t);
__MPFR_DECLSPEC int mpfr_pow_sj (mpfr_ptr, mpfr_srcptr, intmax_t, mpfr_rnd_t);
/* define mpfr_pown (defined in IEEE 754-2019) as an alias for mpfr_pow_sj.
   It is currently implemented as a macro, but this may change in the future
   (it could be implemented as an inline function); in case of change, update
   the manual. */
#define mpfr_pown mpfr_pow_sj

#if defined (__cplusplus)
}
#endif

# endif /* _MPFR_H_HAVE_INTMAX_T */
#endif


/* Check if <stdio.h> has been included or if the user wants FILE */
#if defined (_GMP_H_HAVE_FILE) || defined (MPFR_USE_FILE)
# ifndef _MPFR_H_HAVE_FILE
# define _MPFR_H_HAVE_FILE 1

#if defined (__cplusplus)
extern "C" {
#endif

#define mpfr_inp_str __gmpfr_inp_str
#define mpfr_out_str __gmpfr_out_str
__MPFR_DECLSPEC size_t mpfr_inp_str (mpfr_ptr, FILE*, int, mpfr_rnd_t);
__MPFR_DECLSPEC size_t mpfr_out_str (FILE*, int, size_t, mpfr_srcptr,
                                     mpfr_rnd_t);
#ifndef MPFR_USE_MINI_GMP
#define mpfr_fprintf __gmpfr_fprintf
__MPFR_DECLSPEC int mpfr_fprintf (FILE*, const char*, ...);
#endif
#define mpfr_fpif_export __gmpfr_fpif_export
#define mpfr_fpif_import __gmpfr_fpif_import
__MPFR_DECLSPEC int mpfr_fpif_export (FILE*, mpfr_ptr);
__MPFR_DECLSPEC int mpfr_fpif_import (mpfr_ptr, FILE*);

#if defined (__cplusplus)
}
#endif

# endif /* _MPFR_H_HAVE_FILE */
#endif


/* check if <stdarg.h> has been included or if the user wants va_list */
#if defined (_GMP_H_HAVE_VA_LIST) || defined (MPFR_USE_VA_LIST)
# ifndef _MPFR_H_HAVE_VA_LIST
# define _MPFR_H_HAVE_VA_LIST 1

#if defined (__cplusplus)
extern "C" {
#endif

#define mpfr_vprintf __gmpfr_vprintf
#define mpfr_vasprintf __gmpfr_vasprintf
#define mpfr_vsprintf __gmpfr_vsprintf
#define mpfr_vsnprintf __gmpfr_vsnprintf
__MPFR_DECLSPEC int mpfr_vprintf (const char*, va_list);
__MPFR_DECLSPEC int mpfr_vasprintf (char**, const char*, va_list);
__MPFR_DECLSPEC int mpfr_vsprintf (char*, const char*, va_list);
__MPFR_DECLSPEC int mpfr_vsnprintf (char*, size_t, const char*, va_list);

#if defined (__cplusplus)
}
#endif

# endif /* _MPFR_H_HAVE_VA_LIST */
#endif


/* check if <stdarg.h> has been included and if FILE is available
   (see above) */
#if defined (_MPFR_H_HAVE_VA_LIST) && defined (_MPFR_H_HAVE_FILE)
# ifndef _MPFR_H_HAVE_VA_LIST_FILE
# define _MPFR_H_HAVE_VA_LIST_FILE 1

#if defined (__cplusplus)
extern "C" {
#endif

#define mpfr_vfprintf __gmpfr_vfprintf
__MPFR_DECLSPEC int mpfr_vfprintf (FILE*, const char*, va_list);

#if defined (__cplusplus)
}
#endif

# endif /* _MPFR_H_HAVE_VA_LIST_FILE */
#endif
