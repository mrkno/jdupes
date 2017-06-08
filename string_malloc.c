/*
 * String table allocator
 * A replacement for malloc() for tables of fixed strings
 *
 * Copyright (C) 2015-2017 by Jody Bruchon <jody@jodybruchon.com>
 * Released under The MIT License
 */

#define restrict __restrict

#include <stdlib.h>
#include <stdint.h>
#include "string_malloc.h"

/* Size of pages to allocate at once. Must be divisible by uintptr_t.
 * The maximum object size is this page size minus about 16 bytes! */
#ifndef SMA_PAGE_SIZE
#define SMA_PAGE_SIZE 262144
#endif

/* Max freed pointers to remember. Increasing this number allows storing
 * more free objects but can slow down allocations. Don't increase it if
 * the program's total reused freed alloc counter doesn't increase as a
 * result or you're slowing allocs down to no benefit. */
#ifndef SMA_MAX_FREE
#define SMA_MAX_FREE 32
#endif

/* Minimum free object size to consider adding to free list */
#ifndef SMA_MIN_SLACK
#define SMA_MIN_SLACK 48
#endif

#ifdef DEBUG
uintmax_t sma_allocs = 0;
uintmax_t sma_free_ignored = 0;
uintmax_t sma_free_good = 0;
uintmax_t sma_free_reclaimed = 0;
uintmax_t sma_free_scanned = 0;
uintmax_t sma_free_tails = 0;
 #define DBG(a) a
#else
 #define DBG(a)
#endif


/* This is used to bypass string_malloc for debugging */
#ifdef SMA_PASSTHROUGH
void *string_malloc(size_t len)
{
	return malloc(len);
}

void string_free(void *ptr)
{
	free(ptr);
	return;
}

void string_malloc_destroy(void) {
	return;
}

#else /* Not SMA_PASSTHROUGH mode */

static void *sma_head = NULL;
static uintptr_t *sma_curpage = NULL;
static unsigned int sma_pages = 0;
static void *sma_freelist[SMA_MAX_FREE];
static int sma_freelist_cnt = 0;
static size_t sma_nextfree = sizeof(uintptr_t);


/* Scan the freed chunk list for a suitably sized object */
static inline void *scan_freelist(const size_t size)
{
	size_t *object, *min_p;
	size_t sz, min = 0;
	int i, used = 0, min_i = -1;

	/* Don't bother scanning if the list is empty */
	if (sma_freelist_cnt == 0) return NULL;

	for (i = 0; i < SMA_MAX_FREE; i++) {
		/* Stop scanning once we run out of valid entries */
		if (used == sma_freelist_cnt) return NULL;

		DBG(sma_free_scanned++;)
		object = sma_freelist[i];
		/* Skip empty entries */
		if (object == NULL) continue;

		sz = *object;
		used++;

		/* Skip smaller objects */
		if (sz < size) continue;
		/* Object is big enough; record if it's the new minimum */
		if (min == 0 || sz <= min) {
			min = sz;
			min_i = i;
			/* Always stop scanning if exact sized object found */
			if (sz == size) break;
		}
	}

	/* Enhancement TODO: split the free item if it's big enough */

	/* Return smallest object found and delete from free list */
	if (min_i != -1) {
		min_p = sma_freelist[min_i];
		sma_freelist[min_i] = NULL;
		sma_freelist_cnt--;
		min_p++;
		return (void *)min_p;
	}
	/* Fall through - free list search failed */
	return NULL;
}


/* malloc() a new page for string_malloc to use */
static inline void *string_malloc_page(void)
{
	uintptr_t * restrict pageptr;

	/* Allocate page and set up pointers at page starts */
	pageptr = (uintptr_t *)malloc(SMA_PAGE_SIZE);
	if (pageptr == NULL) return NULL;
	*pageptr = (uintptr_t)NULL;

	/* Link previous page to this page, if applicable */
	if (sma_curpage != NULL) *sma_curpage = (uintptr_t)pageptr;

	/* Update last page pointers and total page counter */
	sma_curpage = pageptr;
	sma_pages++;

	return (void *)pageptr;
}


void *string_malloc(size_t len)
{
	const void * restrict page = (char *)sma_curpage;
	static size_t *address;

	/* Calling with no actual length is invalid */
	if (len < 1) return NULL;

	/* Align objects where possible */
	if (len & (sizeof(uintptr_t) - 1)) {
		len &= ~(sizeof(uintptr_t) - 1);
		len += sizeof(uintptr_t);
	}

	/* Pass-through allocations larger than maximum object size to malloc() */
	if (len > (SMA_PAGE_SIZE - sizeof(uintptr_t) - sizeof(size_t))) {
		/* Allocate the space */
		address = (size_t *)malloc(len + sizeof(size_t));
		if (!address) return NULL;
		/* Prefix object with its size */
		*address = len;
		address++;
		DBG(sma_allocs++;)
		return (void *)address;
	}

	/* Initialize on first use */
	if (sma_pages == 0) {
		/* Initialize the freed object list */
		for (int i = 0; i < SMA_MAX_FREE; i++) sma_freelist[i] = NULL;
		/* Allocate first page and set up for first allocation */
		sma_head = string_malloc_page();
		if (sma_head == NULL) return NULL;
		sma_nextfree = sizeof(uintptr_t);
		page = sma_head;
	}

	/* Allocate objects from the free list first */
	address = (size_t *)scan_freelist(len);
	if (address != NULL) {
		DBG(sma_free_reclaimed++;)
		return (void *)address;
	}

	/* Allocate new page if this object won't fit */
	if ((sma_nextfree + len + sizeof(size_t)) > SMA_PAGE_SIZE) {
		size_t sz;
		size_t *tailaddr;
		/* See if remaining space is usable */
		if (sma_freelist_cnt < SMA_MAX_FREE && (sma_nextfree + sizeof(size_t)) < SMA_PAGE_SIZE) {
			/* Get remaining space size minus page linkage and obj size prefix */
			sz = sma_nextfree + sizeof(size_t) + SMA_MIN_SLACK;

			if (sz <= SMA_PAGE_SIZE) {
				sz = SMA_PAGE_SIZE - sma_nextfree - sizeof(size_t);
				tailaddr = (size_t *)((uintptr_t)page + sma_nextfree);
				*tailaddr = (size_t)sz;
				tailaddr++;
				string_free(tailaddr);
				DBG(sma_free_tails++;)
			}
		}
		page = string_malloc_page();
		if (!page) return NULL;
		sma_nextfree = sizeof(uintptr_t);
	}

	/* Allocate the space */
	address = (size_t *)((uintptr_t)page + sma_nextfree);
	/* Prefix object with its size */
	*address = len;
	address++;
	sma_nextfree += len + sizeof(size_t);

	DBG(sma_allocs++;)
	return (void *)address;
}


/* Free an object, adding to free list if possible */
void string_free(void * const restrict addr)
{
	int i = 0;
	size_t * const restrict a = (size_t *)addr - 1;

	/* Do nothing on NULL address or full free list */
	if ((addr == NULL) || sma_freelist_cnt == SMA_MAX_FREE)
		goto sf_failed;

	/* Tiny objects keep big ones from being freed; ignore them */
	if (*(size_t *)((uintptr_t)addr - sizeof(size_t)) < SMA_MIN_SLACK)
		goto sf_failed;

	/* Add object to free list */
	while (i < SMA_MAX_FREE) {
		if (sma_freelist[i] == NULL) {
			sma_freelist[i] = a;
			sma_freelist_cnt++;
			DBG(sma_free_good++;)
			return;
		}
		i++;
	}

	/* Fall through */
sf_failed:
	DBG(sma_free_ignored++;)
	return;
}

/* Destroy all allocated pages */
void string_malloc_destroy(void)
{
	uintptr_t *cur;
	uintptr_t *next;

	cur = sma_head;
	if (sma_head == NULL) return;
	while (sma_pages > 0) {
		next = (uintptr_t *)*cur;
		free(cur);
		cur = next;
		sma_pages--;
	}
	sma_head = NULL;
	return;
}

#endif /* SMA_PASSTHROUGH */
