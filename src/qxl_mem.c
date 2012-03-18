/*
 * Copyright 2009 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "qxl.h"
#include "mspace.h"

struct qxl_mem
{
    mspace	space;
    void *	base;
    unsigned long n_bytes;
};

struct qxl_mem *
qxl_mem_create       (void                   *base,
		      unsigned long           n_bytes)
{
    struct qxl_mem *mem;

    mem = calloc (sizeof (*mem), 1);
    if (!mem)
	goto out;

    ErrorF ("memory space from %p to %p\n", base, (char *)base + n_bytes);

    mem->space = create_mspace_with_base (base, n_bytes, 0, NULL);
    
    mem->base = base;
    mem->n_bytes = n_bytes;

out:
    return mem;

}

void
qxl_mem_dump_stats   (struct qxl_mem         *mem,
		      const char             *header)
{
    ErrorF ("%s\n", header);
    
    mspace_malloc_stats (mem->space);
}

void *
qxl_alloc            (struct qxl_mem         *mem,
		      unsigned long           n_bytes)
{
    return mspace_malloc (mem->space, n_bytes);
}

void
qxl_free             (struct qxl_mem         *mem,
		      void                   *d)
{
    mspace_free (mem->space, d);
}

void
qxl_mem_free_all     (struct qxl_mem         *mem)
{
    mem->space = create_mspace_with_base (mem->base, mem->n_bytes, 0, NULL);
}

#if 0

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "qxl.h"

struct block
{
    unsigned long n_bytes;
    
    union
    {
	struct
	{
	    struct block *next;
	} unused;

	struct
	{
	    uint8_t data[0];
	} used;
    } u;
};

struct qxl_mem
{
    void *		base;
    unsigned long	n_bytes;
    
    struct block *unused;
    unsigned long total_allocated;
    unsigned long total_freed;
    unsigned long n_allocated_blocks;
    unsigned long n_freed_blocks;
};

static void
initialize (struct qxl_mem *mem)
{
    mem->unused = (struct block *)mem->base;
    mem->unused->n_bytes = mem->n_bytes;
    mem->unused->u.unused.next = NULL;    

    mem->total_allocated = 0;
    mem->total_freed = 0;
    mem->n_allocated_blocks = 0;
    mem->n_freed_blocks = 0;
}

struct qxl_mem *
qxl_mem_create (void *base, unsigned long n_bytes)
{
    struct qxl_mem *mem = NULL;

    mem = calloc (sizeof (*mem), 1);
    if (!mem)
	goto out;

    ErrorF ("memory space from %p to %p\n", base, base + n_bytes);
    
    mem->base = base;
    mem->n_bytes = n_bytes;

    initialize (mem);
    
out:
    return mem;
}

void
qxl_mem_free_all (struct qxl_mem *mem)
{
    initialize (mem);
}

void
qxl_mem_dump_stats (struct qxl_mem *mem, const char *header)
{
    struct block *b;
    int n_blocks;
    unsigned long max_block = 0;
    unsigned long min_block = 0xffffffffffffffffUL;
    
    fprintf (stderr, "%s\n", header);
    
    n_blocks = 0;
    for (b = mem->unused; b != NULL; b = b->u.unused.next)
    {
	fprintf (stderr, "block: %p (%lu bytes)\n", b, b->n_bytes);
	
	if (b->u.unused.next && b >= b->u.unused.next)
	{
	    fprintf (stderr, "b: %p   b->next: %p\n",
		     b, b->u.unused.next);
	    assert (0);
	}

	if (b->u.unused.next && (void *)b + b->n_bytes >= b->u.unused.next)
	{
	    fprintf (stderr, "OVERLAPPING BLOCKS b: %p   b->next: %p\n",
		     b, b->u.unused.next);
	    assert (0);
	}

	if (b->n_bytes > max_block)
	    max_block = b->n_bytes;

	if (b->n_bytes < min_block)
	    min_block = b->n_bytes;
	
	++n_blocks;
    }

    fprintf (stderr, "=========\n");

    fprintf (stderr, "%d blocks\n", n_blocks);
    fprintf (stderr, "min block: %lu bytes\n", min_block);
    fprintf (stderr, "max block: %lu bytes\n", max_block);
    fprintf (stderr, "total freed: %lu bytres\n", mem->total_freed);
    fprintf (stderr, "total allocated: %lu bytes\n",
	     mem->total_allocated - mem->total_freed);
    fprintf (stderr, "total free: %lu bytes\n",
	     mem->n_bytes - (mem->total_allocated - mem->total_freed));
}

void
sanity_check (struct qxl_mem *mem)
{
#if 0
    struct block *b;

    ErrorF ("sanity check\n");

    for (b = mem->unused; b != NULL; b = b->u.unused.next)
    {
	ErrorF (" %p\n", b);

	if (b == (void *)0xffffffffffffffff)
	    abort();
    }
#endif
}

void *
qxl_alloc (struct qxl_mem *mem, unsigned long n_bytes)
{
    struct block *b, *prev;

    mem->n_allocated_blocks++;
    
    /* Simply pretend the user asked to allocate the header as well. Then
     * we can mostly ignore the difference between blocks and allocations
     */
    n_bytes += sizeof (unsigned long);

    n_bytes = (n_bytes + 7) & ~((1 << 3) - 1);
    
    if (n_bytes < sizeof (struct block))
	n_bytes = sizeof (struct block);

    assert (mem->unused);
    
    prev = NULL;
    for (b = mem->unused; b != NULL; prev = b, b = b->u.unused.next)
    {
	if (b->n_bytes >= n_bytes)
	{
	    struct block *new_block;

	    if (b->n_bytes - n_bytes >= sizeof (struct block))
	    {
		new_block = (void *)b + n_bytes;

		new_block->n_bytes = b->n_bytes - n_bytes;

		if (prev)
		{
		    assert (prev < b);
		    assert (prev->u.unused.next == NULL || prev < prev->u.unused.next);
		    
 		    new_block->u.unused.next = b->u.unused.next;
		    prev->u.unused.next = new_block;
		}
		else
		{
		    assert (mem->unused == b);

		    new_block->u.unused.next = mem->unused->u.unused.next;
		    mem->unused = new_block;
		}

		b->n_bytes = n_bytes;
	    }
	    else
	    {
#if 0
		printf ("Exact match\n");
#endif

		if (prev)
		{
		    prev->u.unused.next = b->u.unused.next;
		}
		else
		{
		    mem->unused = b->u.unused.next;
		}
	    }

	    mem->total_allocated += n_bytes;

	    sanity_check (mem);
	    
	    return (void *)b->u.used.data;
	}
	else
	{
#if 0
	    printf ("Skipping small block %d\n", b->n_bytes);
#endif
	}
    }

    /* If we get here, we are out of memory, so print some stats */
#if 0
    fprintf (stderr, "Failing to allocate %lu bytes\n", n_bytes);
    qxl_mem_dump_stats (mem, "out of memory");
#endif
    
    return NULL;
}

/* Finds the unused block before and the unused block after @data. Both
 * before and after can be NULL if data is before the first or after the
 * last unused block.
 */
static void
find_neighbours (struct qxl_mem *mem, void *data,
		 struct block **before, struct block **after)
{
    struct block *b;
    *before = NULL;
    *after = NULL;
    
    for (b = mem->unused; b != NULL; b = b->u.unused.next)
    {
	if ((void *)b < data)
	    *before = b;
	
	if ((void *)b > data)
	{
	    *after = b;
	    break;
	}
    }

    if (*before)
	assert ((*before)->u.unused.next == *after);
}

void
qxl_free (struct qxl_mem *mem, void *d)
{
    struct block *b = d - sizeof (unsigned long);
    struct block *before, *after;

    mem->total_freed += b->n_bytes;
    mem->n_freed_blocks++;
    
#if 0
    printf ("freeing %p (%d bytes)\n", b, b->n_bytes);
    
    qxl_mem_dump_stats (mem, "before free");
#endif
    
    find_neighbours (mem, (void *)b, &before, &after);

    if (before)
    {
#if 0
	printf ("  free: merge before: %p\n", before->u.used.data);
#endif
	    
	if ((void *)before + before->n_bytes == b)
	{
#if 0
 	    printf ("  free: merge with before (adding %d bytes)\n", b->n_bytes);
#endif
	    
	    /* Merge before and b */
	    before->n_bytes += b->n_bytes;
	    b = before;
	}
	else
	{
#if 0
	    printf ("  free: no merge with before\n");
#endif

	    if (b == (void *)0xffffffffffffffff)
		abort();
	    
	    before->u.unused.next = b;
	}
    }
    else
    {
#if 0
	printf ("  free: no before\n");
#endif
	mem->unused = b;
    }
    
    if (after)
    {
	if (after == (void *)0xffffffffffffffff)
	    abort();

#if 0
	printf ("  free: after: %p\n", after->u.used.data);
#endif
	if ((void *)b + b->n_bytes == after)
	{
#if 0
	    printf ("  merge with after\n");
#endif
	    b->n_bytes += after->n_bytes;
	    b->u.unused.next = after->u.unused.next;
	}
	else
	{
#if 0
	    printf ("  no merge with after\n");
#endif
	    b->u.unused.next = after;
 	}
    }
    else
    {
#if 0
	printf ("  free: no after\n");
#endif
	b->u.unused.next = NULL;
    }

#if 0
    qxl_mem_dump_stats (mem, "after free");
#endif
    sanity_check (mem);
}


#endif
