/*================================================================
** Copyright 2000, Clark Cooper
** All rights reserved.
**
** This is free software. You are permitted to copy, distribute, or modify
** it under the terms of the MIT/X license (contained in the COPYING file
** with this distribution.)
*/

#ifndef EXPATCONFIG_H
#define EXPATCONFIG_H

#include <memory.h>
#include <string.h>

#define XML_NS 1
#define XML_DTD 1
#define XML_CONTEXT_BYTES 1024

/* we will assume all Windows platforms are little endian */
#define BYTEORDER 1234

/* Windows has memmove() available. */
#define HAVE_MEMMOVE

#ifdef WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#undef WIN32_LEAN_AND_MEAN
#else
#endif

#endif /* ifndef EXPATCONFIG_H */
