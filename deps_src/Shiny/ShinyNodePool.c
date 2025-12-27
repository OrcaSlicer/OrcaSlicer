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

#include "ShinyNodePool.h"
#include "ShinyTools.h"

#include <memory.h>
#include <malloc.h>

/*---------------------------------------------------------------------------*/

ShinyNodePool* ShinyNodePool_create(uint32_t a_items) {
	ShinyNodePool* pPool = (ShinyNodePool*)
		malloc(sizeof(ShinyNodePool) + sizeof(ShinyNode) * (a_items - 1));

	pPool->nextPool = NULL;
	pPool->_nextItem = &pPool->_items[0];
	pPool->endOfItems = &pPool->_items[a_items];

	memset(&pPool->_items[0], 0, a_items * sizeof(ShinyNode));
	return pPool;
}


/*---------------------------------------------------------------------------*/

uint32_t ShinyNodePool_memoryUsageChain(ShinyNodePool *first) {
	uint32_t bytes = (uint32_t) ((char*) first->endOfItems - (char*) first);
	ShinyNodePool *pool = first->nextPool;

	while (pool) {
		bytes += (uint32_t) ((char*) pool->endOfItems - (char*) pool);
		pool = pool->nextPool;
	}

	return bytes;
}


/*---------------------------------------------------------------------------*/

void ShinyNodePool_destroy(ShinyNodePool *self) {
	ShinyNode* firstNode = ShinyNodePool_firstItem(self);
	ShinyNode* lastNode = self->_nextItem;

	while (firstNode != lastNode)
		ShinyNode_destroy(firstNode++);

	/* TODO: make this into a loop or a tail recursion */
	if (self->nextPool) ShinyNodePool_destroy(self->nextPool);
	free(self);
}

#endif /* SLIC3R_PROFILE */
