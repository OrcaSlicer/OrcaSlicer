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

#ifndef SHINY_MANAGER_H
#define SHINY_MANAGER_H

#include "ShinyZone.h"
#include "ShinyNode.h"
#include "ShinyNodePool.h"
#include "ShinyTools.h"
#include "ShinyOutput.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

typedef struct {
#ifdef SHINY_HAS_ENABLED
	bool enabled;
#endif

	shinytick_t _lastTick;

	ShinyNode* _curNode;

	uint32_t _tableMask; /* = _tableSize - 1 */

	ShinyNodeTable* _nodeTable;

#ifdef SHINY_LOOKUP_RATE
	uint64_t _lookupCount;
	uint64_t _lookupSuccessCount;
#endif

	uint32_t _tableSize;

	uint32_t nodeCount;
	uint32_t zoneCount;

	ShinyZone* _lastZone;

	ShinyNodePool* _lastNodePool;
	ShinyNodePool* _firstNodePool;

	ShinyNode rootNode;
	ShinyZone rootZone;

	float damping;

	int _initialized;
	int _firstUpdate;
} ShinyManager;


/*---------------------------------------------------------------------------*/

extern ShinyNode* _ShinyManager_dummyNodeTable[];

extern ShinyManager Shiny_instance;


/*---------------------------------------------------------------------------*/

SHINY_INLINE void _ShinyManager_appendTicksToCurNode(ShinyManager *self) {
	shinytick_t curTick;
	ShinyGetTicks(&curTick);

	ShinyNode_appendTicks(self->_curNode, curTick - self->_lastTick);
	self->_lastTick = curTick;
}

SHINY_API ShinyNode* _ShinyManager_lookupNode(ShinyManager *self, ShinyNodeCache* a_cache, ShinyZone* a_zone);

SHINY_API void _ShinyManager_createNodeTable(ShinyManager *self, uint32_t a_count);
SHINY_API void _ShinyManager_resizeNodeTable(ShinyManager *self, uint32_t a_count);

SHINY_API void _ShinyManager_createNodePool(ShinyManager *self, uint32_t a_count);
SHINY_API void _ShinyManager_resizeNodePool(ShinyManager *self, uint32_t a_count);

SHINY_API ShinyNode* _ShinyManager_createNode(ShinyManager *self, ShinyNodeCache* a_cache, ShinyZone* a_pZone);
SHINY_API void _ShinyManager_insertNode(ShinyManager *self, ShinyNode* a_pNode);

SHINY_INLINE void _ShinyManager_init(ShinyManager *self) {
	self->_initialized = TRUE;

	self->rootNode._last.entryCount = 1;
	self->rootNode._last.selfTicks = 0;
	ShinyGetTicks(&self->_lastTick);
}

SHINY_INLINE void _ShinyManager_uninit(ShinyManager *self) {
	self->_initialized = FALSE;

	ShinyNode_clear(&self->rootNode);
	self->rootNode.parent = &self->rootNode;
	self->rootNode.zone = &self->rootZone;
}

#ifdef SHINY_LOOKUP_RATE
SHINY_INLINE void _ShinyManager_incLookup(ShinyManager *self) { self->_lookupCount++; }
SHINY_INLINE void _ShinyManager_incLookupSuccess(ShinyManager *self) { self->_lookupSuccessCount++; }
SHINY_INLINE float ShinyManager_lookupRate(const ShinyManager *self) { return ((float) self->_lookupSuccessCount) / ((float) self->_lookupCount); }

#else
SHINY_INLINE void _ShinyManager_incLookup(ShinyManager * self) { self = self; }
SHINY_INLINE void _ShinyManager_incLookupSuccess(ShinyManager *  self) { self = self; }
SHINY_INLINE float ShinyManager_lookupRate(const ShinyManager *  self) { self = self; return -1; }
#endif

SHINY_API void ShinyManager_resetZones(ShinyManager *self);
SHINY_API void ShinyManager_destroyNodes(ShinyManager *self);

SHINY_INLINE float ShinyManager_tableUsage(const ShinyManager *self)  {
	return ((float) self->nodeCount) / ((float) self->_tableSize);
}

SHINY_INLINE uint32_t ShinyManager_allocMemInBytes(const ShinyManager *self) {
	return self->_tableSize * sizeof(ShinyNode*)
		 + (self->_firstNodePool)? ShinyNodePool_memoryUsageChain(self->_firstNodePool) : 0;
}

SHINY_INLINE void ShinyManager_beginNode(ShinyManager *self, ShinyNode* a_node) {
	ShinyNode_beginEntry(a_node);

	_ShinyManager_appendTicksToCurNode(self);
	self->_curNode = a_node;
}

SHINY_INLINE void ShinyManager_lookupAndBeginNode(ShinyManager *self, ShinyNodeCache* a_cache, ShinyZone* a_zone) {
#ifdef SHINY_HAS_ENABLED
	if (!self->enabled) return;
#endif

	if (self->_curNode != (*a_cache)->parent)
		*a_cache = _ShinyManager_lookupNode(self, a_cache, a_zone);

	ShinyManager_beginNode(self, *a_cache);
}

SHINY_INLINE void ShinyManager_endCurNode(ShinyManager *self) {
#ifdef SHINY_HAS_ENABLED
	if (!self->enabled) return;
#endif

	_ShinyManager_appendTicksToCurNode(self);
	self->_curNode = self->_curNode->parent;
}

/**/

SHINY_API void ShinyManager_preLoad(ShinyManager *self);

SHINY_API void ShinyManager_updateClean(ShinyManager *self);
SHINY_API void ShinyManager_update(ShinyManager *self);

SHINY_API void ShinyManager_clear(ShinyManager *self);
SHINY_API void ShinyManager_destroy(ShinyManager *self);

SHINY_INLINE void ShinyManager_sortZones(ShinyManager *self) {
	if (self->rootZone.next)
		self->_lastZone = ShinyZone_sortChain(&self->rootZone.next);
}

SHINY_API const char* ShinyManager_getOutputErrorString(ShinyManager *self);

SHINY_API int ShinyManager_output(ShinyManager *self, const char *a_filename);
SHINY_API void ShinyManager_outputToStream(ShinyManager *self, FILE *stream);

#if __cplusplus
} /* end of extern "C" */

SHINY_INLINE std::string ShinyManager_outputTreeToString(ShinyManager *self) {
	const char* error = ShinyManager_getOutputErrorString(self);
	if (error) return error;
	else return ShinyNodesToString(&self->rootNode, self->nodeCount);
}

SHINY_INLINE std::string ShinyManager_outputFlatToString(ShinyManager *self) {
	const char* error = ShinyManager_getOutputErrorString(self);
	if (error) return error;

	ShinyManager_sortZones(self);
	return ShinyZonesToString(&self->rootZone, self->zoneCount);
}

extern "C" { /* end of c++ */
#endif

SHINY_INLINE int ShinyManager_isZoneSelfTimeBelow(ShinyManager *self, ShinyZone* a_zone, float a_percentage) {
	return a_percentage * (float) self->rootZone.data.childTicks.cur
		<= (float) a_zone->data.selfTicks.cur; 
}

SHINY_INLINE int ShinyManager_isZoneTotalTimeBelow(ShinyManager *self, ShinyZone* a_zone, float a_percentage) {
	return a_percentage * (float) self->rootZone.data.childTicks.cur
		<= (float) ShinyData_totalTicksCur(&a_zone->data); 
}

/**/

SHINY_INLINE void ShinyManager_enumerateNodes(ShinyManager *self, void (*a_func)(const ShinyNode*)) {
	ShinyNode_enumerateNodes(&self->rootNode, a_func);
}

SHINY_INLINE void ShinyManager_enumerateZones(ShinyManager *self, void (*a_func)(const ShinyZone*)) {
	ShinyZone_enumerateZones(&self->rootZone, a_func);
}

#if __cplusplus
} /* end of extern "C" */

template <class T> void ShinyManager_enumerateNodes(ShinyManager *self, T* a_this, void (T::*a_func)(const ShinyNode*)) {
	ShinyNode_enumerateNodes(&self->rootNode, a_this, a_func);
}

template <class T> void ShinyManager_enumerateZones(ShinyManager *self, T* a_this, void (T::*a_func)(const ShinyZone*)) {
	ShinyZone_enumerateZones(&self->rootZone, a_this, a_func);
}

extern "C" { /* end of c++ */
#endif


/*---------------------------------------------------------------------------*/

#if __cplusplus
} /* end of extern "C" */

class ShinyEndNodeOnDestruction {
public:

	SHINY_INLINE ~ShinyEndNodeOnDestruction() {
		ShinyManager_endCurNode(&Shiny_instance);
	}
};
#endif

#endif /* SHINY_MANAGER_H */
