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

#ifndef SHINY_ZONE_H
#define SHINY_ZONE_H

#include "ShinyData.h"
#include <memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

#define SHINY_ZONE_STATE_HIDDEN			0
#define SHINY_ZONE_STATE_INITIALIZED	1
#define SHINY_ZONE_STATE_UPDATING		2


/*---------------------------------------------------------------------------*/

typedef struct _ShinyZone {
	struct _ShinyZone* next;
	int _state;
	const char* name;
	ShinyData data;
} ShinyZone;


/*---------------------------------------------------------------------------*/

SHINY_INLINE void ShinyZone_init(ShinyZone *self, ShinyZone* a_prev) {
	self->_state = SHINY_ZONE_STATE_INITIALIZED;
	a_prev->next = self;
}

SHINY_INLINE void ShinyZone_uninit(ShinyZone *self) {
	self->_state = SHINY_ZONE_STATE_HIDDEN;
	self->next = NULL;
}

SHINY_API void ShinyZone_preUpdateChain(ShinyZone *first);
SHINY_API void ShinyZone_updateChain(ShinyZone *first, float a_damping);
SHINY_API void ShinyZone_updateChainClean(ShinyZone *first);

SHINY_API void ShinyZone_resetChain(ShinyZone *first);

SHINY_API ShinyZone* ShinyZone_sortChain(ShinyZone **first);

SHINY_INLINE float ShinyZone_compare(ShinyZone *a, ShinyZone *b) {
	return a->data.selfTicks.avg - b->data.selfTicks.avg;
}

SHINY_API void ShinyZone_clear(ShinyZone* self);

SHINY_API void ShinyZone_enumerateZones(const ShinyZone* a_zone, void (*a_func)(const ShinyZone*));

#if __cplusplus
} /* end of extern "C" */

template <class T>
void ShinyZone_enumerateZones(const ShinyZone* a_zone, T* a_this, void (T::*a_func)(const ShinyZone*)) {
	(a_this->*a_func)(a_zone);

	if (a_zone->next) ShinyZone_enumerateZones(a_zone->next, a_this, a_func);
}
#endif /* __cplusplus */

#endif /* SHINY_ZONE_H */
