/*
The MIT License

Copyright (c) 2007-2010 Aidin Abedi http://code.google.com/p/shinyprofiler/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef SHINY_PREREQS_H
#define SHINY_PREREQS_H


#include <stdint.h>

/*---------------------------------------------------------------------------*/

#ifndef FALSE
#define FALSE	0x0
#endif

#ifndef TRUE
#define TRUE	0x1
#endif

#ifndef NULL
#define NULL	0
#endif

#include "ShinyConfig.h"
#include "ShinyVersion.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

#define SHINY_PLATFORM_WIN32	0x1
#define SHINY_PLATFORM_POSIX	0x2

#if defined (_WIN32)
#   define SHINY_PLATFORM	SHINY_PLATFORM_WIN32

#else /* ASSUME: POSIX-compliant OS */
#   define SHINY_PLATFORM	SHINY_PLATFORM_POSIX
#endif


/*---------------------------------------------------------------------------*/

#define SHINY_COMPILER_MSVC		0x1
#define SHINY_COMPILER_GNUC		0x2
#define SHINY_COMPILER_OTHER	0x3

#if defined (_MSC_VER)
#   define SHINY_COMPILER	SHINY_COMPILER_MSVC

#elif defined (__GNUG__)
#   define SHINY_COMPILER	SHINY_COMPILER_GNUC

#else
#   define SHINY_COMPILER	SHINY_COMPILER_OTHER
#endif


/*---------------------------------------------------------------------------*/

#if SHINY_COMPILER == SHINY_COMPILER_GNUC
#include <sys/types.h>
#include <stdint.h>
#endif


/*---------------------------------------------------------------------------*/

struct _ShinyNode;
struct _ShinyZone;

typedef struct _ShinyNode* ShinyNodeCache;
typedef struct _ShinyNode* ShinyNodeTable;

/*---------------------------------------------------------------------------*/

#define SHINY_API

/*---------------------------------------------------------------------------*/

#if SHINY_COMPILER == SHINY_COMPILER_MSVC
#	define SHINY_INLINE		__inline
#	define SHINY_UNUSED
#elif SHINY_COMPILER == SHINY_COMPILER_GNUC
#	define SHINY_INLINE		inline
#	define SHINY_UNUSED		__attribute__((unused))
#elif SHINY_COMPILER == SHINY_COMPILER_OTHER
#	define SHINY_INLINE		inline
#	define SHINY_UNUSED
#endif


/*---------------------------------------------------------------------------*/

#if SHINY_COMPILER == SHINY_COMPILER_MSVC
	typedef int					int32_t;
	typedef unsigned int		uint32_t;

	typedef __int64				int64_t;
	typedef unsigned __int64	uint64_t;

/*
#elif defined(__CYGWIN__)
	typedef u_int32_t			uint32_t;
	typedef u_int64_t			uint64_t;
*/
#endif

	typedef uint64_t			shinytick_t;

#if __cplusplus
} /* end of extern "C" */
#endif

#endif /* SHINY_PREREQS_H */
