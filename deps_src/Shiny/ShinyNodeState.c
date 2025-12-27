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

#ifdef SLIC3R_PROFILE

#include "ShinyNodeState.h"
#include "ShinyNode.h"
#include "ShinyZone.h"

#include <malloc.h>

/*---------------------------------------------------------------------------*/

ShinyNodeState* ShinyNodeState_push(ShinyNodeState *a_top, ShinyNode *a_node) {
	ShinyZone *zone = a_node->zone;
	ShinyNodeState *self = (ShinyNodeState*) malloc(sizeof(ShinyNodeState));
	self->node = a_node;
	self->_prev = a_top;

	a_node->data.selfTicks.cur = a_node->_last.selfTicks;
	a_node->data.entryCount.cur = a_node->_last.entryCount;

	zone->data.selfTicks.cur += a_node->_last.selfTicks;
	zone->data.entryCount.cur += a_node->_last.entryCount;
	
	a_node->data.childTicks.cur = 0;
	a_node->_last.selfTicks = 0;
	a_node->_last.entryCount = 0;

	self->zoneUpdating = zone->_state != SHINY_ZONE_STATE_UPDATING;
	if (self->zoneUpdating) {
		zone->_state = SHINY_ZONE_STATE_UPDATING;
	} else {
		zone->data.childTicks.cur -= a_node->data.selfTicks.cur;
	}

	return self;
}

/*---------------------------------------------------------------------------*/

ShinyNodeState* ShinyNodeState_pop(ShinyNodeState *a_top) {
	ShinyNodeState *prev = a_top->_prev;
	free(a_top);
	return prev;
}

/*---------------------------------------------------------------------------*/

ShinyNode* ShinyNodeState_finishAndGetNext(ShinyNodeState *self, float a_damping) {
	ShinyNode *node = self->node;
	ShinyZone *zone = node->zone;

	if (self->zoneUpdating) {					
		zone->data.childTicks.cur += node->data.childTicks.cur;
		zone->_state = SHINY_ZONE_STATE_INITIALIZED;
	}

	ShinyData_computeAverage(&node->data, a_damping);

	if (!ShinyNode_isRoot(node))
		node->parent->data.childTicks.cur += node->data.selfTicks.cur + node->data.childTicks.cur;

	return node->nextSibling;
}


/*---------------------------------------------------------------------------*/

ShinyNode* ShinyNodeState_finishAndGetNextClean(ShinyNodeState *self) {
	ShinyNode *node = self->node;
	ShinyZone *zone = node->zone;

	if (self->zoneUpdating) {					
		zone->data.childTicks.cur += node->data.childTicks.cur;
		zone->_state = SHINY_ZONE_STATE_INITIALIZED;
	}

	ShinyData_copyAverage(&node->data);

	if (!ShinyNode_isRoot(node))
		node->parent->data.childTicks.cur += node->data.selfTicks.cur + node->data.childTicks.cur;

	return node->nextSibling;
}

#endif /* SLIC3R_PROFILE */
