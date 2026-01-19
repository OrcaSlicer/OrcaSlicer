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

#include "ShinyZone.h"

#include <memory.h>

/*---------------------------------------------------------------------------*/

void ShinyZone_preUpdateChain(ShinyZone *first) {
	ShinyZone* zone = first;

	while (zone) {
		ShinyData_clearCurrent(&(zone->data));
		zone = zone->next;
	}
}


/*---------------------------------------------------------------------------*/

void ShinyZone_updateChain(ShinyZone *first, float a_damping) {
	ShinyZone* zone = first;

	do {
		ShinyData_computeAverage(&(zone->data), a_damping);
		zone = zone->next;
	} while (zone);
}


/*---------------------------------------------------------------------------*/

void ShinyZone_updateChainClean(ShinyZone *first) {
	ShinyZone* zone = first;

	do {
		ShinyData_copyAverage(&(zone->data));
		zone = zone->next;
	} while (zone);
}


/*---------------------------------------------------------------------------*/

void ShinyZone_resetChain(ShinyZone *first) {
	ShinyZone* zone = first, *temp;

	do {
		zone->_state = SHINY_ZONE_STATE_HIDDEN;
		temp = zone->next;
		zone->next = NULL;
		zone = temp;
	} while (zone);
}

/*---------------------------------------------------------------------------*/

/* A Linked-List Memory Sort
   by Philip J. Erdelsky
   pje@efgh.com
   http://www.alumni.caltech.edu/~pje/

   Modified by Aidin Abedi
*/

ShinyZone* ShinyZone_sortChain(ShinyZone **first) /* return ptr to last zone */
{
	ShinyZone *p = *first;

	unsigned base;
	unsigned long block_size;

	struct tape
	{
		ShinyZone *first, *last;
		unsigned long count;
	} tape[4];

	/* Distribute the records alternately to tape[0] and tape[1]. */

	tape[0].count = tape[1].count = 0L;
	tape[0].first = NULL;
	base = 0;
	while (p != NULL)
	{
		ShinyZone *next = p->next;
		p->next = tape[base].first;
		tape[base].first = p;
		tape[base].count++;
		p = next;
		base ^= 1;
	}

	/* If the list is empty or contains only a single record, then */
	/* tape[1].count == 0L and this part is vacuous.               */

	for (base = 0, block_size = 1L; tape[base+1].count != 0L;
		base ^= 2, block_size <<= 1)
	{
		int dest;
		struct tape *tape0, *tape1;
		tape0 = tape + base;
		tape1 = tape + base + 1;
		dest = base ^ 2;
		tape[dest].count = tape[dest+1].count = 0;
		for (; tape0->count != 0; dest ^= 1)
		{
			unsigned long n0, n1;
			struct tape *output_tape = tape + dest;
			n0 = n1 = block_size;
			while (1)
			{
				ShinyZone *chosen_record;
				struct tape *chosen_tape;
				if (n0 == 0 || tape0->count == 0)
				{
					if (n1 == 0 || tape1->count == 0)
						break;
					chosen_tape = tape1;
					n1--;
				}
				else if (n1 == 0 || tape1->count == 0)
				{
					chosen_tape = tape0;
					n0--;
				}
				else if (ShinyZone_compare(tape1->first, tape0->first) > 0)
				{
					chosen_tape = tape1;
					n1--;
				}
				else
				{
					chosen_tape = tape0;
					n0--;
				}
				chosen_tape->count--;
				chosen_record = chosen_tape->first;
				chosen_tape->first = chosen_record->next;
				if (output_tape->count == 0)
					output_tape->first = chosen_record;
				else
					output_tape->last->next = chosen_record;
				output_tape->last = chosen_record;
				output_tape->count++;
			}
		}
	}

	if (tape[base].count > 1L) {
		ShinyZone* last = tape[base].last;
		*first = tape[base].first;
		last->next = NULL;
		return last;

	} else {
		return NULL;
	}
}


/*---------------------------------------------------------------------------*/

void ShinyZone_clear(ShinyZone* self) {
	memset(self, 0, sizeof(ShinyZone));
}


/*---------------------------------------------------------------------------*/

void ShinyZone_enumerateZones(const ShinyZone* a_zone, void (*a_func)(const ShinyZone*)) {
	a_func(a_zone);

	if (a_zone->next) ShinyZone_enumerateZones(a_zone->next, a_func);
}

#endif /* SLIC3R_PROFILE */
