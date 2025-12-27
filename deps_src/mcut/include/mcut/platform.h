/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 * 
 * NOTE: This file is licensed under GPL-3.0-or-later (default). 
 * A commercial license can be purchased from Floyd M. Chitalu. 
 *  
 * License details:
 * 
 * (A)  GNU General Public License ("GPL"); a copy of which you should have 
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 * 
 * The commercial license options is for users that wish to use MCUT in 
 * their products for comercial purposes but do not wish to release their 
 * software products under the GPL license. 
 * 
 * Author(s)     : Floyd M. Chitalu
 */

/**
 * @file platform.h
 * @author Floyd M. Chitalu
 * @date 1 Jan 2021
 * @brief File containing platform-specific types and definitions.
 *
 * This header file defines platform specific directives and integral type 
 * declarations.
 * Platforms should define these so that MCUT users call MCUT commands
 * with the same calling conventions that the MCUT implementation expects.
 *
 * MCAPI_ATTR - Placed before the return type in function declarations.
 *              Useful for C++11 and GCC/Clang-style function attribute syntax.
 * MCAPI_CALL - Placed after the return type in function declarations.
 *              Useful for MSVC-style calling convention syntax.
 * MCAPI_PTR  - Placed between the '(' and '*' in function pointer types.
 *
 * Function declaration:  MCAPI_ATTR void MCAPI_CALL mcFunction(void);
 * Function pointer type: typedef void (MCAPI_PTR *PFN_mcFunction)(void);
 * 
 */

#ifndef MC_PLATFORM_H_
#define MC_PLATFORM_H_

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

/** @file */

#if defined(_WIN32)
    
#ifdef MCUT_SHARED_LIB

#if defined(MCUT_EXPORT_SHARED_LIB_SYMBOLS)
    //** Symbol visibilty */
    #define MCAPI_ATTR __declspec(dllexport)
#else
    //** Symbol visibilty */
    #define MCAPI_ATTR __declspec(dllimport)
#endif

#else // MCUT_SHARED_LIB

    //** Symbol visibilty */
#define MCAPI_ATTR

#endif // MCUT_SHARED_LIB

    //** Function calling convention */
    #define MCAPI_CALL __stdcall
    //** Function pointer-type declaration helper */
    #define MCAPI_PTR  MCAPI_CALL
#else // #if defined(_WIN32)
    
    //** Symbol visibilty */
    #define MCAPI_ATTR __attribute__((visibility("default")))
    //** Function calling convention */
    #define MCAPI_CALL
    //** Function pointer-type declaration helper */
    #define MCAPI_PTR
#endif // #if defined(_WIN32)

#include <stddef.h> // standard type definitions

#if !defined(MC_NO_STDINT_H)
    #if defined(_MSC_VER) && (_MSC_VER < 1600)
        typedef signed   __int8  int8_t;
        typedef unsigned __int8  uint8_t;
        typedef signed   __int16 int16_t;
        typedef unsigned __int16 uint16_t;
        typedef signed   __int32 int32_t;
        typedef unsigned __int32 uint32_t;
        typedef signed   __int64 int64_t;
        typedef unsigned __int64 uint64_t;
    #else
        #include <stdint.h> //  integer types
    #endif
#endif // !defined(MC_NO_STDINT_H)

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif