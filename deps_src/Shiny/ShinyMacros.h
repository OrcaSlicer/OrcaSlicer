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

#ifndef SHINY_MACROS_H
#define SHINY_MACROS_H

#ifdef SLIC3R_PROFILE

#include "ShinyManager.h"

/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_UPDATE()													\
	ShinyManager_update(&Shiny_instance)

#define PROFILE_SET_DAMPING(floatfrom0to1)									\
	Shiny_instance.damping = (floatfrom0to1);

#define PROFILE_GET_DAMPING()												\
	(Shiny_instance.damping)

#define PROFILE_OUTPUT(filename)											\
	ShinyManager_output(&Shiny_instance, (filename))

#define PROFILE_OUTPUT_STREAM(stream)										\
	ShinyManager_outputToStream(&Shiny_instance, (stream))

#ifdef __cplusplus
#define PROFILE_GET_TREE_STRING()											\
	ShinyManager_outputTreeToString(&Shiny_instance)

#define PROFILE_GET_FLAT_STRING()											\
	ShinyManager_outputFlatToString(&Shiny_instance)
#endif /* __cplusplus */

#define PROFILE_DESTROY()													\
	ShinyManager_destroy(&Shiny_instance)

#define PROFILE_CLEAR()														\
	ShinyManager_clear(&Shiny_instance)

#define PROFILE_SORT_ZONES()												\
	ShinyManager_sortZones(&Shiny_instance)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_GET_TOTAL_TICKS_CUR()										\
	ShinyData_totalTicksCur(&Shiny_instance.rootZone.data)

#define PROFILE_GET_TOTAL_TICKS()											\
	ShinyData_totalTicksAvg(&Shiny_instance.rootZone.data)

#define PROFILE_GET_PROFILED_TICKS_CUR()									\
	(Shiny_instance.rootZone.data.selfTicks.cur)

#define PROFILE_GET_PROFILED_TICKS()										\
	(Shiny_instance.rootZone.data.selfTicks.avg)

#define PROFILE_GET_UNPROFILED_TICKS_CUR()									\
	(Shiny_instance.rootZone.data.childTicks.cur)

#define PROFILE_GET_UNPROFILED_TICKS()										\
	(Shiny_instance.rootZone.data.childTicks.avg)

#define PROFILE_GET_SHARED_TOTAL_TICKS_CUR(name)							\
	ShinyData_totalTicksCur(&(_PROFILE_ID_ZONE_SHARED(name).data))

#define PROFILE_GET_SHARED_TOTAL_TICKS(name)								\
	ShinyData_totalTicksAvg(&(_PROFILE_ID_ZONE_SHARED(name).data))

#define PROFILE_GET_SHARED_SELF_TICKS_CUR(name)								\
	(_PROFILE_ID_ZONE_SHARED(name).data.selfTicks.cur)

#define PROFILE_GET_SHARED_SELF_TICKS(name)									\
	(_PROFILE_ID_ZONE_SHARED(name).data.selfTicks.avg)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_IS_SHARED_SELF_BELOW(name, floatfrom0to1)					\
	ShinyManager_isZoneSelfTimeBelow(										\
		&Shiny_instance, _PROFILE_ID_ZONE_SHARED(name), floatfrom0to1)

#define PROFILE_IS_SHARED_TOTAL_BELOW(name, floatfrom0to1)					\
	ShinyManager_isZoneTotalTimeBelow(										\
		&Shiny_instance, _PROFILE_ID_ZONE_SHARED(name), floatfrom0to1)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_END()														\
	ShinyManager_endCurNode(&Shiny_instance)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_BEGIN( name )												\
																			\
	static _PROFILE_ZONE_DEFINE(_PROFILE_ID_ZONE(name), #name);				\
	_PROFILE_ZONE_BEGIN(_PROFILE_ID_ZONE(name))


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#ifdef __cplusplus
#define PROFILE_BLOCK( name )												\
																			\
	_PROFILE_BLOCK_DEFINE(_PROFILE_ID_BLOCK());								\
	PROFILE_BEGIN(name)
#endif /* __cplusplus */

/*---------------------------------------------------------------------------*/
/* public preprocessors */

#ifdef __cplusplus
#define PROFILE_FUNC()														\
																			\
	_PROFILE_BLOCK_DEFINE(_PROFILE_ID_BLOCK());								\
	static _PROFILE_ZONE_DEFINE(_PROFILE_ID_ZONE_FUNC(), __FUNCTION__);		\
	_PROFILE_ZONE_BEGIN(_PROFILE_ID_ZONE_FUNC())
#endif /* __cplusplus */

/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_CODE( code )												\
																			\
	do {																	\
		static _PROFILE_ZONE_DEFINE(_PROFILE_ID_ZONE_CODE(), #code);		\
		_PROFILE_ZONE_BEGIN(_PROFILE_ID_ZONE_CODE());						\
		{ code; }															\
		PROFILE_END();														\
	} while(0)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_SHARED_EXTERN( name )										\
																			\
	_PROFILE_ZONE_DECLARE(extern, _PROFILE_ID_ZONE_SHARED(name))


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_SHARED_DEFINE( name )										\
																			\
	_PROFILE_ZONE_DEFINE(_PROFILE_ID_ZONE_SHARED(name), #name)


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#define PROFILE_SHARED_BEGIN( name )										\
																			\
	_PROFILE_ZONE_BEGIN(_PROFILE_ID_ZONE_SHARED(name))


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#ifdef __cplusplus
#define PROFILE_SHARED_BLOCK( name )										\
																			\
	_PROFILE_BLOCK_DEFINE(_PROFILE_ID_BLOCK());								\
	_PROFILE_ZONE_BEGIN(_PROFILE_ID_ZONE_SHARED(name))
#endif /* __cplusplus */


/*---------------------------------------------------------------------------*/
/* public preprocessors */

#ifdef SHINY_HAS_ENABLED
#define PROFILE_SET_ENABLED( boolean )										\
	Shiny_instance.enabled = boolean
#endif


/*---------------------------------------------------------------------------*/
/* internal preprocessors */

#define _PROFILE_ID_ZONE( name )			__ShinyZone_##name
#define _PROFILE_ID_ZONE_FUNC()				__ShinyZoneFunc
#define _PROFILE_ID_ZONE_CODE()				__ShinyZoneCode
#define _PROFILE_ID_ZONE_SHARED( name )		name##__ShinyZoneShared
#define _PROFILE_ID_BLOCK()					__ShinyBlock


/*---------------------------------------------------------------------------*/
/* internal preprocessor */

#define _PROFILE_ZONE_DEFINE( id, string )									\
																			\
	ShinyZone id = {														\
		NULL, SHINY_ZONE_STATE_HIDDEN, string,								\
		{ { 0, 0 }, { 0, 0 }, { 0, 0 } }									\
	}


/*---------------------------------------------------------------------------*/
/* internal preprocessor */

#define _PROFILE_ZONE_DECLARE( prefix, id )									\
																			\
	prefix ShinyZone id


/*---------------------------------------------------------------------------*/
/* internal preprocessor */

#define _PROFILE_BLOCK_DEFINE( id )											\
																			\
	ShinyEndNodeOnDestruction SHINY_UNUSED id


/*---------------------------------------------------------------------------*/
/* internal preprocessor */

#define _PROFILE_ZONE_BEGIN( id )											\
																			\
	do {																	\
		static ShinyNodeCache cache = &_ShinyNode_dummy;					\
		ShinyManager_lookupAndBeginNode(&Shiny_instance, &cache, &id);		\
	} while(0)

/*---------------------------------------------------------------------------*/

#else /* SLIC3R_PROFILE */

#define PROFILE_UPDATE()
#define PROFILE_SET_DAMPING(x)
#define PROFILE_GET_DAMPING()			0.0f
#define PROFILE_OUTPUT(x)
#define PROFILE_OUTPUT_STREAM(x)
#define PROFILE_CLEAR()
#define PROFILE_GET_TREE_STRING()		std::string()
#define PROFILE_GET_FLAT_STRING()		std::string()
#define PROFILE_DESTROY()
#define PROFILE_BEGIN(name)
#define PROFILE_BLOCK(name)
#define PROFILE_FUNC()
#define PROFILE_CODE(code)				do { code; } while (0)
#define PROFILE_SHARED_GLOBAL(name)
#define PROFILE_SHARED_MEMBER(name)
#define PROFILE_SHARED_DEFINE(name)
#define PROFILE_SHARED_BEGIN(name)
#define PROFILE_SHARED_BLOCK(name)
#define PROFILE_SET_ENABLED(boolean)

#endif /* SLIC3R_PROFILE */

#endif /* SHINY_MACROS_H */
