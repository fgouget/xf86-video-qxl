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

#include <stdarg.h>

#include "qxl.h"
#include "mspace.h"

#ifdef DEBUG_QXL_MEM
#include <valgrind/memcheck.h>
#endif

struct qxl_mem
{
    mspace	space;
    void *	base;
    unsigned long n_bytes;
#ifdef DEBUG_QXL_MEM
    size_t used_initial;
    int unverifiable;
    int missing;
#endif
};

#ifdef DEBUG_QXL_MEM
void
qxl_mem_unverifiable(struct qxl_mem *mem)
{
    mem->unverifiable = 1;
}
#endif

static void
errout (void *data, const char *format, ...)
{
    va_list va;

    va_start (va, format);

    VErrorF (format, va);

    va_end (va);
}

struct qxl_mem *
qxl_mem_create       (void                   *base,
		      unsigned long           n_bytes)
{
    struct qxl_mem *mem;

    mem = calloc (sizeof (*mem), 1);
    if (!mem)
	goto out;

    ErrorF ("memory space from %p to %p\n", base, (char *)base + n_bytes);

    mspace_set_print_func (errout);
    
    mem->space = create_mspace_with_base (base, n_bytes, 0, NULL);
    
    mem->base = base;
    mem->n_bytes = n_bytes;

#ifdef DEBUG_QXL_MEM
    {
        size_t used;

        mspace_malloc_stats_return(mem->space, NULL, NULL, &used);
        mem->used_initial = used;
        mem->unverifiable = 0;
        mem->missing = 0;
    }
#endif

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
		      unsigned long           n_bytes,
		      const char             *name)
{
    void *addr = mspace_malloc (mem->space, n_bytes);

#ifdef DEBUG_QXL_MEM
    VALGRIND_MALLOCLIKE_BLOCK(addr, n_bytes, 0, 0);
#ifdef DEBUG_QXL_MEM_VERBOSE
    fprintf(stderr, "alloc %p: %ld (%s)\n", addr, n_bytes, name);
#endif
#endif
    return addr;
}

void
qxl_free             (struct qxl_mem         *mem,
		      void                   *d,
		      const char *            name)
{
#if 0
    ErrorF ("%p <= free %s\n", d, name);
#endif
    mspace_free (mem->space, d);
#ifdef DEBUG_QXL_MEM
#ifdef DEBUG_QXL_MEM_VERBOSE
    fprintf(stderr, "free  %p %s\n", d, name);
#endif
    VALGRIND_FREELIKE_BLOCK(d, 0);
#endif
}

void
qxl_mem_free_all     (struct qxl_mem         *mem)
{
#ifdef DEBUG_QXL_MEM
    size_t maxfp, fp, used;

    if (mem->space)
    {
        mspace_malloc_stats_return(mem->space, &maxfp, &fp, &used);
        mem->missing = used - mem->used_initial;
        ErrorF ("untracked %zd bytes (%s)", used - mem->used_initial,
            mem->unverifiable ? "marked unverifiable" : "oops");
    }
#endif
    mem->space = create_mspace_with_base (mem->base, mem->n_bytes, 0, NULL);
}

