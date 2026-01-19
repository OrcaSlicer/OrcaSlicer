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

#include "ShinyNode.h"
#include "ShinyZone.h"
#include "ShinyNodeState.h"

#include <memory.h>

/*---------------------------------------------------------------------------*/

ShinyNode _ShinyNode_dummy = {
	/* _last = */ { 0, 0 },
	/* zone = */ NULL,
	/* parent = */ NULL,
	/* nextSibling = */ NULL,
	/* firstChild = */ NULL,
	/* lastChild = */ NULL
};


/*---------------------------------------------------------------------------*/

void ShinyNode_updateTree(ShinyNode* first, float a_damping) {
	ShinyNodeState *top = NULL;
	ShinyNode *node = first;

	for (;;) {
		do {
			top = ShinyNodeState_push(top, node);
			node = node->firstChild;
		} while (node);

		for (;;) {
			node = ShinyNodeState_finishAndGetNext(top, a_damping);
			top = ShinyNodeState_pop(top);

			if (node) break;
			else if (!top) return;
		}
	}
}


/*---------------------------------------------------------------------------*/

void ShinyNode_updateTreeClean(ShinyNode* first) {
	ShinyNodeState *top = NULL;
	ShinyNode *node = first;

	for (;;) {
		do {
			top = ShinyNodeState_push(top, node);
			node = node->firstChild;
		} while (node);

		for (;;) {
			node = ShinyNodeState_finishAndGetNextClean(top);
			top = ShinyNodeState_pop(top);

			if (node) break;
			else if (!top) return;
		}
	}
}


/*---------------------------------------------------------------------------*/

const ShinyNode* ShinyNode_findNextInTree(const ShinyNode* self) {
	if (self->firstChild) {
		return self->firstChild;

	} else if (self->nextSibling) {
		return self->nextSibling;

	} else {
		ShinyNode* pParent = self->parent;

		while (!ShinyNode_isRoot(pParent)) {
			if (pParent->nextSibling) return pParent->nextSibling;
			else pParent = pParent->parent;
		}

		return NULL;
	}
}


/*---------------------------------------------------------------------------*/

void ShinyNode_clear(ShinyNode* self) {
	memset(self, 0, sizeof(ShinyNode));
}


/*---------------------------------------------------------------------------*/

void ShinyNode_enumerateNodes(const ShinyNode* a_node, void (*a_func)(const ShinyNode*)) {
	a_func(a_node);

	if (a_node->firstChild) ShinyNode_enumerateNodes(a_node->firstChild, a_func);
	if (a_node->nextSibling) ShinyNode_enumerateNodes(a_node->nextSibling, a_func);
}

#endif /* SLIC3R_PROFILE */
