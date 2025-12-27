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

#ifndef SHINY_NODE_H
#define SHINY_NODE_H

#include "ShinyData.h"
#include "ShinyTools.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

typedef struct _ShinyNode {

	ShinyLastData _last;

	struct _ShinyZone* zone;
	struct _ShinyNode* parent;
	struct _ShinyNode* nextSibling;

	struct _ShinyNode* firstChild;
	struct _ShinyNode* lastChild;

	uint32_t childCount;
	uint32_t entryLevel;

	ShinyNodeCache* _cache;

	ShinyData data;

} ShinyNode;


/*---------------------------------------------------------------------------*/

extern ShinyNode _ShinyNode_dummy;


/*---------------------------------------------------------------------------*/

SHINY_INLINE void ShinyNode_addChild(ShinyNode* self,  ShinyNode* a_child) {
	if (self->childCount++) {
		self->lastChild->nextSibling = a_child;
		self->lastChild = a_child;

	} else {
		self->lastChild = a_child;
		self->firstChild = a_child;
	}
}

SHINY_INLINE void ShinyNode_init(ShinyNode* self, ShinyNode* a_parent, struct _ShinyZone* a_zone, ShinyNodeCache* a_cache) {
	/* NOTE: all member variables are assumed to be zero when allocated */

	self->zone = a_zone;
	self->parent = a_parent;

	self->entryLevel = a_parent->entryLevel + 1;
	ShinyNode_addChild(a_parent, self);

	self->_cache = a_cache;
}

SHINY_API void ShinyNode_updateTree(ShinyNode* self, float a_damping);
SHINY_API void ShinyNode_updateTreeClean(ShinyNode* self);

SHINY_INLINE void ShinyNode_destroy(ShinyNode* self) {
	*(self->_cache) = &_ShinyNode_dummy;
}

SHINY_INLINE void ShinyNode_appendTicks(ShinyNode* self, shinytick_t a_elapsedTicks) {
	self->_last.selfTicks += a_elapsedTicks;
}

SHINY_INLINE void ShinyNode_beginEntry(ShinyNode* self) {
	self->_last.entryCount++;
}

SHINY_INLINE int ShinyNode_isRoot(ShinyNode* self) {
	return (self->entryLevel == 0);
}

SHINY_INLINE int ShinyNode_isDummy(ShinyNode* self) {
	return (self == &_ShinyNode_dummy);
}

SHINY_INLINE int ShinyNode_isEqual(ShinyNode* self, const ShinyNode* a_parent, const struct _ShinyZone* a_zone) {
	return (self->parent == a_parent && self->zone == a_zone);
}

SHINY_API const ShinyNode* ShinyNode_findNextInTree(const ShinyNode* self);

SHINY_API void ShinyNode_clear(ShinyNode* self);

SHINY_API void ShinyNode_enumerateNodes(const ShinyNode* a_node, void (*a_func)(const ShinyNode*));

#if __cplusplus
} /* end of extern "C" */

template <class T>
void ShinyNode_enumerateNodes(const ShinyNode* a_node, T* a_this, void (T::*a_func)(const ShinyNode*)) {
	(a_this->*a_func)(a_node);

	if (a_node->firstChild) ShinyNode_enumerateNodes(a_node->firstChild, a_this, a_func);
	if (a_node->nextSibling) ShinyNode_enumerateNodes(a_node->nextSibling, a_this, a_func);
}
#endif /* __cplusplus */

#endif /* SHINY_NODE_H */
