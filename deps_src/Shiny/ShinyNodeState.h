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

#ifndef SHINY_NODE_STATE_H
#define SHINY_NODE_STATE_H

#include "ShinyNode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/

typedef struct _ShinyNodeState {
	ShinyNode *node;
	int zoneUpdating;

	struct _ShinyNodeState *_prev;
} ShinyNodeState;


/*---------------------------------------------------------------------------*/

ShinyNodeState* ShinyNodeState_push(ShinyNodeState *a_top, ShinyNode *a_node);
ShinyNodeState* ShinyNodeState_pop(ShinyNodeState *a_top);

ShinyNode* ShinyNodeState_finishAndGetNext(ShinyNodeState *self, float a_damping);
ShinyNode* ShinyNodeState_finishAndGetNextClean(ShinyNodeState *self);

#if __cplusplus
} /* end of extern "C" */
#endif

#endif /* SHINY_NODE_STATE_H */
