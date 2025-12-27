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

#include "ShinyOutput.h"

#include <stdio.h>

#if SHINY_COMPILER == SHINY_COMPILER_MSVC
#	pragma warning(disable: 4996)
#	define snprintf		_snprintf
#	define TRAILING		0

#else
#	define TRAILING		1
#endif

/*---------------------------------------------------------------------------*/

#define OUTPUT_WIDTH_CALL	6
#define OUTPUT_WIDTH_TIME	(6+3)
#define OUTPUT_WIDTH_PERC	(4+3)
#define OUTPUT_WIDTH_SUM	120 

#define OUTPUT_WIDTH_DATA	(1+OUTPUT_WIDTH_CALL + 1 + 2*(OUTPUT_WIDTH_TIME+4+OUTPUT_WIDTH_PERC+1) + 1)
#define OUTPUT_WIDTH_NAME	(OUTPUT_WIDTH_SUM - OUTPUT_WIDTH_DATA)


/*---------------------------------------------------------------------------*/

SHINY_INLINE char* printHeader(char *output, const char *a_title) {
	snprintf(output, OUTPUT_WIDTH_SUM + TRAILING,
		"%-*s %*s %*s %*s",
		OUTPUT_WIDTH_NAME, a_title,
		OUTPUT_WIDTH_CALL, "calls",
		OUTPUT_WIDTH_TIME+4+OUTPUT_WIDTH_PERC+1, "self time",
		OUTPUT_WIDTH_TIME+4+OUTPUT_WIDTH_PERC+1, "total time");

	return output + OUTPUT_WIDTH_SUM;
}


/*---------------------------------------------------------------------------*/

SHINY_INLINE char* printData(char *output, const ShinyData *a_data, float a_topercent) {
	float totalTicksAvg = ShinyData_totalTicksAvg(a_data);
	const ShinyTimeUnit *selfUnit = ShinyGetTimeUnit(a_data->selfTicks.avg);
	const ShinyTimeUnit *totalUnit = ShinyGetTimeUnit(totalTicksAvg);

	snprintf(output, OUTPUT_WIDTH_DATA + TRAILING,
		" %*.1f %*.2f %-2s %*.2f%% %*.2f %-2s %*.2f%%",
		OUTPUT_WIDTH_CALL, a_data->entryCount.avg,
		OUTPUT_WIDTH_TIME, a_data->selfTicks.avg * selfUnit->invTickFreq, selfUnit->suffix,
		OUTPUT_WIDTH_PERC, a_data->selfTicks.avg * a_topercent,
		OUTPUT_WIDTH_TIME, totalTicksAvg * totalUnit->invTickFreq, totalUnit->suffix,
		OUTPUT_WIDTH_PERC, totalTicksAvg * a_topercent);

	return output + OUTPUT_WIDTH_DATA;
}


/*---------------------------------------------------------------------------*/

SHINY_INLINE char* printNode(char* output, const ShinyNode *a_node, float a_topercent) {
	int offset = a_node->entryLevel * 2;

	snprintf(output, OUTPUT_WIDTH_NAME + TRAILING, "%*s%-*s",
		offset, "", OUTPUT_WIDTH_NAME - offset, a_node->zone->name);

	output += OUTPUT_WIDTH_NAME;

	output = printData(output, &a_node->data, a_topercent);
	return output;
}


/*---------------------------------------------------------------------------*/

SHINY_INLINE char* printZone(char* output, const ShinyZone *a_zone, float a_topercent) {
	snprintf(output, OUTPUT_WIDTH_NAME + TRAILING, "%-*s",
		OUTPUT_WIDTH_NAME, a_zone->name);

	output += OUTPUT_WIDTH_NAME;

	output = printData(output, &a_zone->data, a_topercent);
	return output;
}


/*---------------------------------------------------------------------------*/

int ShinyPrintNodesSize(uint32_t a_count) {
	return (1 + a_count) * (OUTPUT_WIDTH_SUM + 1);
}


/*---------------------------------------------------------------------------*/

int ShinyPrintZonesSize(uint32_t a_count) {
	return (1 + a_count) * (OUTPUT_WIDTH_SUM + 1);
}


/*---------------------------------------------------------------------------*/

void ShinyPrintANode(char* output, const ShinyNode *a_node, const ShinyNode *a_root) {
	float fTicksToPc = 100.0f / a_root->data.childTicks.avg;
	output = printNode(output, a_node, fTicksToPc);
	(*output++) = '\0';
}


/*---------------------------------------------------------------------------*/

void ShinyPrintAZone(char* output, const ShinyZone *a_zone, const ShinyZone *a_root) {
	float fTicksToPc = 100.0f / a_root->data.childTicks.avg;
	output = printZone(output, a_zone, fTicksToPc);
	(*output++) = '\0';
}


/*---------------------------------------------------------------------------*/

void ShinyPrintNodes(char* output, const ShinyNode *a_root) {
	float fTicksToPc = 100.0f / a_root->data.childTicks.avg;
	const ShinyNode *node = a_root;

	output = printHeader(output, "call tree");
	(*output++) = '\n';

	for (;;) {
		output = printNode(output, node, fTicksToPc);

		node = ShinyNode_findNextInTree(node);
		if (node) {
			(*output++) = '\n';
		} else {
			(*output++) = '\0';
			return;
		}
	}
}


/*---------------------------------------------------------------------------*/

void ShinyPrintZones(char* output, const ShinyZone *a_root) {
	float fTicksToPc = 100.0f / a_root->data.childTicks.avg;
	const ShinyZone *zone = a_root;

	output = printHeader(output, "sorted list");
	(*output++) = '\n';

	for (;;) {
		output = printZone(output, zone, fTicksToPc);

		zone = zone->next;
		if (zone) {
			(*output++) = '\n';
		} else {
			(*output++) = '\0';
			return;
		}
	}
}

#endif /* SLIC3R_PROFILE */
