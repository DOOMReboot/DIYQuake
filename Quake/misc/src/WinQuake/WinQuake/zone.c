/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// Z_zone.c

#include "quakedef.h"

#define	DYNAMIC_SIZE	0xc000

#define	ZONEID	0x1d4a11
#define MINFRAGMENT	64

// AngryCPPCoder: I was thinking why would IsUsed be an int (32 bit)? All was needed was just 
// a single bit, for used or not, now looking at it, most likely the decision to make it 32 bit 
// was to help with memory alignment to be a multiple of 64.
typedef struct ZoneMemoryBlock_S
{
	int size;           // including the header and possibly tiny fragments
	int IsUsed;         // a IsUsed of 0 is a free block 
	int id;        		// should be ZONEID
	struct ZoneMemoryBlock_S* next, * prev;
	int pad;			// pad to 64 bit boundary
} ZoneMemoryBlock;

typedef struct
{
	int size;		// total bytes malloced, including header
	ZoneMemoryBlock	BlockList;		// start / end cap for linked list
	ZoneMemoryBlock* pRover;
} ZoneMemoryHeader;

void Cache_FreeLow(int new_low_hunk);
void Cache_FreeHigh(int new_high_hunk);


/*
==============================================================================

						ZONE MEMORY ALLOCATION

There is never any space between memblocks, and there will never be two
contiguous free memblocks.

The rover can be left pointing at a non-empty block

The zone calls are pretty much only used for small strings and structures,
all big things are allocated on the hunk.
==============================================================================
*/

ZoneMemoryHeader* pZoneRoot;

/*
========================
Z_ClearZone
========================
*/
void Z_ClearZone(ZoneMemoryHeader* pZoneRoot, int size)
{
	ZoneMemoryBlock* block;

	// set the entire zone to one free block
	block = (ZoneMemoryBlock*)((byte*)pZoneRoot + sizeof(ZoneMemoryHeader));
	//pZoneRoot->size is never set or used
	pZoneRoot->BlockList.size = 0;
	pZoneRoot->BlockList.IsUsed = 1;    // in use block
	pZoneRoot->BlockList.id = 0;
	pZoneRoot->BlockList.next = block;
	pZoneRoot->BlockList.prev = block;
	pZoneRoot->pRover = block;

	block->size = size - sizeof(ZoneMemoryHeader);
	block->IsUsed = 0;                 // free block
	block->id = ZONEID;
	block->next = &pZoneRoot->BlockList;
	block->prev = &pZoneRoot->BlockList;
}


/*
========================
Z_Free
========================
*/
void Z_Free(void* ptr)
{
	ZoneMemoryBlock* pMemoryBlock, * other;

	if (!ptr)
		Sys_Error("Z_Free: NULL pointer");

	pMemoryBlock = (ZoneMemoryBlock*)((byte*)ptr - sizeof(ZoneMemoryBlock));
	if (pMemoryBlock->id != ZONEID)
		Sys_Error("Z_Free: freed a pointer without ZONEID");
	if (pMemoryBlock->IsUsed == 0)
		Sys_Error("Z_Free: freed a freed pointer");

	pMemoryBlock->IsUsed = 0;		// mark as free

	other = pMemoryBlock->prev;
	if (!other->IsUsed)
	{	// merge with previous free block
		other->size += pMemoryBlock->size;
		other->next = pMemoryBlock->next;
		other->next->prev = other;
		if (pMemoryBlock == pZoneRoot->pRover)
			pZoneRoot->pRover = other;
		pMemoryBlock = other;
	}

	other = pMemoryBlock->next;
	if (!other->IsUsed)
	{	// merge the next free block onto the end
		pMemoryBlock->size += other->size;
		pMemoryBlock->next = other->next;
		pMemoryBlock->next->prev = pMemoryBlock;
		if (other == pZoneRoot->pRover)
			pZoneRoot->pRover = pMemoryBlock;
	}
}


/*
========================
Z_Malloc
========================
*/
void* Z_Malloc(int size)
{
	void* buf;

	// Z_CheckHeap();	// DEBUG
	buf = Z_TagMalloc(size, 1);
	if (!buf)
		Sys_Error("Z_Malloc: failed on allocation of %i bytes", size);
	Q_memset(buf, 0, size);

	return buf;
}

void* Z_TagMalloc(int size, int IsUsed)
{
	int		extra;
	ZoneMemoryBlock *start, *pRover, *new, * base;

	if (!IsUsed)
		Sys_Error("Z_TagMalloc: tried to use a 0 IsUsed");

	//
	// scan through the block list looking for the first free block
	// of sufficient size
	//

	size += sizeof(ZoneMemoryBlock);	// account for size of block header
	size += 4;					// space for memory trash tester
	size = (size + 7) & ~7;		// align to 8-byte boundary

	base = pRover = pZoneRoot->pRover;
	start = base->prev;

	do
	{
		if (pRover == start)	// scanned all the way around the list
			return NULL;
		if (pRover->IsUsed)
			base = pRover = pRover->next;
		else
			pRover = pRover->next;
	} while (base->IsUsed || base->size < size);

	//
	// found a block big enough
	//
	extra = base->size - size;
	if (extra > MINFRAGMENT)
	{	// there will be a free fragment after the allocated block
		new = (ZoneMemoryBlock*)((byte*)base + size);
		new->size = extra;
		new->IsUsed = 0;			// free block
		new->prev = base;
		new->id = ZONEID;
		new->next = base->next;
		new->next->prev = new;
		base->next = new;
		base->size = size;
	}

	base->IsUsed = IsUsed;				// no longer a free block

	pZoneRoot->pRover = base->next;	// next allocation will start looking here

	base->id = ZONEID;

	// marker for memory trash testing
	*(int*)((byte*)base + base->size - 4) = ZONEID;

	return (void*)((byte*)base + sizeof(ZoneMemoryBlock));
}


/*
========================
Z_Print
========================
*/
void Z_Print(ZoneMemoryHeader* zone)
{
	ZoneMemoryBlock* block;

	Con_Printf("zone size: %i  location: %p\n", pZoneRoot->size, pZoneRoot);

	for (block = zone->BlockList.next; ; block = block->next)
	{
		Con_Printf("block:%p    size:%7i    IsUsed:%3i\n",
			block, block->size, block->IsUsed);

		if (block->next == &zone->BlockList)
			break;			// all blocks have been hit	
		if ((byte*)block + block->size != (byte*)block->next)
			Con_Printf("ERROR: block size does not touch the next block\n");
		if (block->next->prev != block)
			Con_Printf("ERROR: next block doesn't have proper back link\n");
		if (!block->IsUsed && !block->next->IsUsed)
			Con_Printf("ERROR: two consecutive free blocks\n");
	}
}


/*
========================
Z_CheckHeap
========================
*/
void Z_CheckHeap(void)
{
	ZoneMemoryBlock* pMemoryBlock;

	for (pMemoryBlock = pZoneRoot->BlockList.next; ; pMemoryBlock = pMemoryBlock->next)
	{
		if (pMemoryBlock->next == &pZoneRoot->BlockList)
			break;			// all blocks have been hit	
		if ((byte*)pMemoryBlock + pMemoryBlock->size != (byte*)pMemoryBlock->next)
			Sys_Error("Z_CheckHeap: block size does not touch the next block\n");
		if (pMemoryBlock->next->prev != pMemoryBlock)
			Sys_Error("Z_CheckHeap: next block doesn't have proper back link\n");
		if (!pMemoryBlock->IsUsed && !pMemoryBlock->next->IsUsed)
			Sys_Error("Z_CheckHeap: two consecutive free blocks\n");
	}
}

//============================================================================

#define	HUNK_SENTINAL	0x1df001ed

typedef struct
{
	int		sentinal;
	int		size;		// including sizeof(HunkHeader), -1 = not allocated
	char	name[8];
} HunkHeader; // AngryCPPCoder: hunk_t renamed to HunkHeader

// AngryCPPCoder: rename hunk_base to pHunkBase;
// AngryCPPCoder: rename hunk_size to iHunkSize;
byte* pHunkBase;
int iHunkSize;

int hunk_low_used;
int hunk_high_used;

qboolean	hunk_tempactive;
int		hunk_tempmark;

//void R_FreeTextures(void);

/*
==============
Hunk_Check

Run consistancy and sentinal trahing checks
==============
*/
void Hunk_Check(void)
{
	HunkHeader* header; // AngryCPPCoder: h renamed to header

	for (header = (HunkHeader*)pHunkBase; (byte*)header != pHunkBase + hunk_low_used; )
	{
		if (header->sentinal != HUNK_SENTINAL)
			Sys_Error("Hunk_Check: trahsed sentinal");
		if (header->size < 16 || header->size + (byte*)header - pHunkBase > iHunkSize)
			Sys_Error("Hunk_Check: bad size");
		header = (HunkHeader*)((byte*)header + header->size);
	}
}

/*
==============
Hunk_Print

If "all" is specified, every single allocation is printed.
Otherwise, allocations with the same name will be totaled up before printing.
==============
*/
void Hunk_Print(qboolean all)
{
	// AngryCPPCoder: 	hunk_t*           h, * next,   * endlow, * starthigh, * endhigh;
	//                  HunkHeader* pHeader, * pNext, * pEndLow, * pStartHigh, * pEndHigh;

	HunkHeader* pHeader, * pNext, * pEndLow, * pStartHigh, * pEndHigh;
	int		count, sum;
	int		totalblocks;
	char	name[9];

	name[8] = 0;
	count = 0;
	sum = 0;
	totalblocks = 0;

	pHeader = (HunkHeader*)pHunkBase;
	pEndLow = (HunkHeader*)(pHunkBase + hunk_low_used);
	pStartHigh = (HunkHeader*)(pHunkBase + iHunkSize - hunk_high_used);
	pEndHigh = (HunkHeader*)(pHunkBase + iHunkSize);

	Con_Printf("          :%8i total hunk size\n", iHunkSize);
	Con_Printf("-------------------------\n");

	while (1)
	{
		//
		// skip to the high hunk if done with low hunk
		//
		if (pHeader == pEndLow)
		{
			Con_Printf("-------------------------\n");
			Con_Printf("          :%8i REMAINING\n", iHunkSize - hunk_low_used - hunk_high_used);
			Con_Printf("-------------------------\n");
			pHeader = pStartHigh;
		}

		//
		// if totally done, break
		//
		if (pHeader == pEndHigh)
			break;

		//
		// run consistancy checks
		//
		if (pHeader->sentinal != HUNK_SENTINAL)
			Sys_Error("Hunk_Check: trahsed sentinal");
		if (pHeader->size < 16 || pHeader->size + (byte*)pHeader - pHunkBase > iHunkSize)
			Sys_Error("Hunk_Check: bad size");

		pNext = (HunkHeader*)((byte*)pHeader + pHeader->size);
		count++;
		totalblocks++;
		sum += pHeader->size;

		//
		// print the single block
		//
		memcpy(name, pHeader->name, 8);
		if (all)
			Con_Printf("%8p :%8i %8s\n", pHeader, pHeader->size, name);

		//
		// print the total
		//
		if (pNext == pEndLow || pNext == pEndHigh ||
			strncmp(pHeader->name, pNext->name, 8))
		{
			if (!all)
				Con_Printf("          :%8i %8s (TOTAL)\n", sum, name);
			count = 0;
			sum = 0;
		}

		pHeader = pNext;
	}

	Con_Printf("-------------------------\n");
	Con_Printf("%8i total blocks\n", totalblocks);

}

/*
===================
Hunk_AllocName
===================
*/
void* Hunk_AllocName(int size, char* name)
{
	HunkHeader* pHeader;

#ifdef PARANOID
	Hunk_Check();
#endif

	if (size < 0)
		Sys_Error("Hunk_Alloc: bad size: %i", size);

	// AngryCPPCoder: Calculate block size (Header + Size required then rounding to 16 (16 bytes alignment))
	size = sizeof(HunkHeader) + ((size + 15) & ~15);

	if (iHunkSize - hunk_low_used - hunk_high_used < size)
		Sys_Error("Hunk_Alloc: failed on %i bytes", size);

	pHeader = (HunkHeader*)(pHunkBase + hunk_low_used);
	hunk_low_used += size;

	Cache_FreeLow(hunk_low_used);

	memset(pHeader, 0, size);

	pHeader->size = size;
	pHeader->sentinal = HUNK_SENTINAL;
	Q_strncpy(pHeader->name, name, 8);

	return (void*)(pHeader + 1);
}

/*
===================
Hunk_Alloc
===================
*/
void* Hunk_Alloc(int size)
{
	return Hunk_AllocName(size, "unknown");
}

int	Hunk_LowMark(void)
{
	return hunk_low_used;
}

void Hunk_FreeToLowMark(int mark)
{
	if (mark < 0 || mark > hunk_low_used)
		Sys_Error("Hunk_FreeToLowMark: bad mark %i", mark);
	memset(pHunkBase + mark, 0, hunk_low_used - mark);
	hunk_low_used = mark;
}

int	Hunk_HighMark(void)
{
	if (hunk_tempactive)
	{
		hunk_tempactive = false;
		Hunk_FreeToHighMark(hunk_tempmark);
	}

	return hunk_high_used;
}

void Hunk_FreeToHighMark(int mark)
{
	if (hunk_tempactive)
	{
		hunk_tempactive = false;
		Hunk_FreeToHighMark(hunk_tempmark);
	}
	if (mark < 0 || mark > hunk_high_used)
		Sys_Error("Hunk_FreeToHighMark: bad mark %i", mark);
	memset(pHunkBase + iHunkSize - hunk_high_used, 0, hunk_high_used - mark);
	hunk_high_used = mark;
}


/*
===================
Hunk_HighAllocName
===================
*/
void* Hunk_HighAllocName(int size, char* name)
{
	HunkHeader* pHeader;

	if (size < 0)
		Sys_Error("Hunk_HighAllocName: bad size: %i", size);

	if (hunk_tempactive)
	{
		Hunk_FreeToHighMark(hunk_tempmark);
		hunk_tempactive = false;
	}

#ifdef PARANOID
	Hunk_Check();
#endif

	size = sizeof(HunkHeader) + ((size + 15) & ~15);

	if (iHunkSize - hunk_low_used - hunk_high_used < size)
	{
		Con_Printf("Hunk_HighAlloc: failed on %i bytes\n", size);
		return NULL;
	}

	hunk_high_used += size;
	Cache_FreeHigh(hunk_high_used);

	pHeader = (HunkHeader*)(pHunkBase + iHunkSize - hunk_high_used);

	memset(pHeader, 0, size);
	pHeader->size = size;
	pHeader->sentinal = HUNK_SENTINAL;
	Q_strncpy(pHeader->name, name, 8);

	return (void*)(pHeader + 1);
}


/*
=================
Hunk_TempAlloc

Return space from the top of the hunk
=================
*/
void* Hunk_TempAlloc(int size)
{
	void* buf;

	size = (size + 15) & ~15;

	if (hunk_tempactive)
	{
		Hunk_FreeToHighMark(hunk_tempmark);
		hunk_tempactive = false;
	}

	hunk_tempmark = Hunk_HighMark();

	buf = Hunk_HighAllocName(size, "temp");

	hunk_tempactive = true;

	return buf;
}

/*
===============================================================================

CACHE MEMORY

===============================================================================
*/

typedef struct cache_system_s
{
	int						size;		// including this header
	cache_user_t* user;
	char					name[16];
	struct cache_system_s* prev, * next;
	struct cache_system_s* lru_prev, * lru_next;	// for LRU flushing	
} cache_system_t;

cache_system_t* Cache_TryAlloc(int size, qboolean nobottom);

cache_system_t	cache_head;

/*
===========
Cache_Move
===========
*/
void Cache_Move(cache_system_t* c)
{
	cache_system_t* new;

	// we are clearing up space at the bottom, so only allocate it late
	new = Cache_TryAlloc(c->size, true);
	if (new)
	{
		//		Con_Printf ("cache_move ok\n");

		Q_memcpy(new + 1, c + 1, c->size - sizeof(cache_system_t));
		new->user = c->user;
		Q_memcpy(new->name, c->name, sizeof(new->name));
		Cache_Free(c->user);
		new->user->data = (void*)(new + 1);
	}
	else
	{
		//		Con_Printf ("cache_move failed\n");

		Cache_Free(c->user);		// tough luck...
	}
}

/*
============
Cache_FreeLow

Throw things out until the hunk can be expanded to the given point
============
*/
void Cache_FreeLow(int new_low_hunk)
{
	cache_system_t* c;

	while (1)
	{
		c = cache_head.next;
		if (c == &cache_head)
			return;		// nothing in cache at all
		if ((byte*)c >= pHunkBase + new_low_hunk)
			return;		// there is space to grow the hunk
		Cache_Move(c);	// reclaim the space
	}
}

/*
============
Cache_FreeHigh

Throw things out until the hunk can be expanded to the given point
============
*/
void Cache_FreeHigh(int new_high_hunk)
{
	cache_system_t* c, * prev;

	prev = NULL;
	while (1)
	{
		c = cache_head.prev;
		if (c == &cache_head)
			return;		// nothing in cache at all
		if ((byte*)c + c->size <= pHunkBase + iHunkSize - new_high_hunk)
			return;		// there is space to grow the hunk
		if (c == prev)
			Cache_Free(c->user);	// didn't move out of the way
		else
		{
			Cache_Move(c);	// try to move it
			prev = c;
		}
	}
}

void Cache_UnlinkLRU(cache_system_t* cs)
{
	if (!cs->lru_next || !cs->lru_prev)
		Sys_Error("Cache_UnlinkLRU: NULL link");

	cs->lru_next->lru_prev = cs->lru_prev;
	cs->lru_prev->lru_next = cs->lru_next;

	cs->lru_prev = cs->lru_next = NULL;
}

void Cache_MakeLRU(cache_system_t* cs)
{
	if (cs->lru_next || cs->lru_prev)
		Sys_Error("Cache_MakeLRU: active link");

	cache_head.lru_next->lru_prev = cs;
	cs->lru_next = cache_head.lru_next;
	cs->lru_prev = &cache_head;
	cache_head.lru_next = cs;
}

/*
============
Cache_TryAlloc

Looks for a free block of memory between the high and low hunk marks
Size should already include the header and padding
============
*/
cache_system_t* Cache_TryAlloc(int size, qboolean nobottom)
{
	cache_system_t* cs, * new;

	// is the cache completely empty?

	if (!nobottom && cache_head.prev == &cache_head)
	{
		if (iHunkSize - hunk_high_used - hunk_low_used < size)
			Sys_Error("Cache_TryAlloc: %i is greater then free hunk", size);

		new = (cache_system_t*)(pHunkBase + hunk_low_used);
		memset(new, 0, sizeof(*new));
		new->size = size;

		cache_head.prev = cache_head.next = new;
		new->prev = new->next = &cache_head;

		Cache_MakeLRU(new);
		return new;
	}

	// search from the bottom up for space

	new = (cache_system_t*)(pHunkBase + hunk_low_used);
	cs = cache_head.next;

	do
	{
		if (!nobottom || cs != cache_head.next)
		{
			if ((byte*)cs - (byte*)new >= size)
			{	// found space
				memset(new, 0, sizeof(*new));
				new->size = size;

				new->next = cs;
				new->prev = cs->prev;
				cs->prev->next = new;
				cs->prev = new;

				Cache_MakeLRU(new);

				return new;
			}
		}

		// continue looking		
		new = (cache_system_t*)((byte*)cs + cs->size);
		cs = cs->next;

	} while (cs != &cache_head);

	// try to allocate one at the very end
	if (pHunkBase + iHunkSize - hunk_high_used - (byte*)new >= size)
	{
		memset(new, 0, sizeof(*new));
		new->size = size;

		new->next = &cache_head;
		new->prev = cache_head.prev;
		cache_head.prev->next = new;
		cache_head.prev = new;

		Cache_MakeLRU(new);

		return new;
	}

	return NULL;		// couldn't allocate
}

/*
============
Cache_Flush

Throw everything out, so new data will be demand cached
============
*/
void Cache_Flush(void)
{
	while (cache_head.next != &cache_head)
		Cache_Free(cache_head.next->user);	// reclaim the space
}


/*
============
Cache_Print

============
*/
void Cache_Print(void)
{
	cache_system_t* cd;

	for (cd = cache_head.next; cd != &cache_head; cd = cd->next)
	{
		Con_Printf("%8i : %s\n", cd->size, cd->name);
	}
}

/*
============
Cache_Report

============
*/
void Cache_Report(void)
{
	Con_DPrintf("%4.1f megabyte data cache\n", (iHunkSize - hunk_high_used - hunk_low_used) / (float)(1024 * 1024));
}

/*
============
Cache_Compact

============
*/
void Cache_Compact(void)
{
}

/*
============
Cache_Init

============
*/
void Cache_Init(void)
{
	cache_head.next = cache_head.prev = &cache_head;
	cache_head.lru_next = cache_head.lru_prev = &cache_head;

	Cmd_AddCommand("flush", Cache_Flush);
}

/*
==============
Cache_Free

Frees the memory and removes it from the LRU list
==============
*/
void Cache_Free(cache_user_t* c)
{
	cache_system_t* cs;

	if (!c->data)
		Sys_Error("Cache_Free: not allocated");

	cs = ((cache_system_t*)c->data) - 1;

	cs->prev->next = cs->next;
	cs->next->prev = cs->prev;
	cs->next = cs->prev = NULL;

	c->data = NULL;

	Cache_UnlinkLRU(cs);
}



/*
==============

Cache_Check
==============
*/
void* Cache_Check(cache_user_t* c)
{
	cache_system_t* cs;

	if (!c->data)
		return NULL;

	cs = ((cache_system_t*)c->data) - 1;

	// move to head of LRU
	Cache_UnlinkLRU(cs);
	Cache_MakeLRU(cs);

	return c->data;
}


/*
==============
Cache_Alloc
==============
*/
void* Cache_Alloc(cache_user_t* c, int size, char* name)
{
	cache_system_t* cs;

	if (c->data)
		Sys_Error("Cache_Alloc: already allocated");

	if (size <= 0)
		Sys_Error("Cache_Alloc: size %i", size);

	size = (size + sizeof(cache_system_t) + 15) & ~15;

	// find memory for it	
	while (1)
	{
		cs = Cache_TryAlloc(size, false);
		if (cs)
		{
			strncpy(cs->name, name, sizeof(cs->name) - 1);
			c->data = (void*)(cs + 1);
			cs->user = c;
			break;
		}

		// free the least recently used cahedat
		if (cache_head.lru_prev == &cache_head)
			Sys_Error("Cache_Alloc: out of memory");
		// not enough memory at all
		Cache_Free(cache_head.lru_prev->user);
	}

	return Cache_Check(c);
}

//============================================================================


/*
========================
Memory_Init
========================
*/
void Memory_Init(void* buf, int size)
{
	int p;
	int iZoneSize = DYNAMIC_SIZE;

	pHunkBase = buf;
	iHunkSize = size;
	hunk_low_used = 0;
	hunk_high_used = 0;

	Cache_Init();
	p = COM_CheckParm("-zone");
	if (p)
	{
		if (p < com_argc - 1)
			iZoneSize = Q_atoi(com_argv[p + 1]) * 1024;
		else
			Sys_Error("Memory_Init: you must specify a size in KB after -zone");
	}

	pZoneRoot = Hunk_AllocName(iZoneSize, "zone");
	Z_ClearZone(pZoneRoot, iZoneSize);
}
