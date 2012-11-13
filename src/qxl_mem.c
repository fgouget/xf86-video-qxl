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
#include <errno.h>
#include <time.h>
#include <unistd.h>

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

static void __attribute__ ((format (gnu_printf, 2, 3)))
errout (void *data, const char *format, ...)
{
    va_list va;

    va_start (va, format);

    VErrorF (format, va);

    va_end (va);
}

static void __attribute__ ((__noreturn__))
qxl_mspace_abort_func (void *user_data)
{
    abort ();
}

void
qxl_mem_init(void)
{
    mspace_set_print_func (errout);
    mspace_set_abort_func (qxl_mspace_abort_func);
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


static uint8_t
setup_slot (qxl_screen_t *qxl, uint8_t slot_index_offset,
            unsigned long start_phys_addr, unsigned long end_phys_addr,
            uint64_t start_virt_addr, uint64_t end_virt_addr)
{
    uint64_t       high_bits;
    qxl_memslot_t *slot;
    uint8_t        slot_index;
    struct QXLRam *ram_header;

    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);

    slot_index = qxl->rom->slots_start + slot_index_offset;
    slot = &qxl->mem_slots[slot_index];
    slot->start_phys_addr = start_phys_addr;
    slot->end_phys_addr = end_phys_addr;
    slot->start_virt_addr = start_virt_addr;
    slot->end_virt_addr = end_virt_addr;

    ram_header->mem_slot.mem_start = slot->start_phys_addr;
    ram_header->mem_slot.mem_end = slot->end_phys_addr;

    qxl_io_memslot_add (qxl, slot_index);

    slot->generation = qxl->rom->slot_generation;

    high_bits = slot_index << qxl->slot_gen_bits;
    high_bits |= slot->generation;
    high_bits <<= (64 - (qxl->slot_gen_bits + qxl->slot_id_bits));
    slot->high_bits = high_bits;

    return slot_index;
}

void
qxl_reset_and_create_mem_slots (qxl_screen_t *qxl)
{
    ioport_write (qxl, QXL_IO_RESET, 0);
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
    /* Mem slots */
    ErrorF ("slots start: %d, slots end: %d\n",
            qxl->rom->slots_start,
            qxl->rom->slots_end);

    /* Main slot */
    qxl->n_mem_slots = qxl->rom->slots_end;
    qxl->slot_gen_bits = qxl->rom->slot_gen_bits;
    qxl->slot_id_bits = qxl->rom->slot_id_bits;
    qxl->va_slot_mask = (~(uint64_t)0) >> (qxl->slot_id_bits + qxl->slot_gen_bits);

    qxl->mem_slots = xnfalloc (qxl->n_mem_slots * sizeof (qxl_memslot_t));

#ifdef XSPICE
    qxl->main_mem_slot = qxl->vram_mem_slot = setup_slot (qxl, 0, 0, ~0, 0, ~0);
#else /* QXL */
    qxl->main_mem_slot = setup_slot (qxl, 0,
                                     (unsigned long)qxl->ram_physical,
                                     (unsigned long)qxl->ram_physical + qxl->surface0_size +
                                     (unsigned long)qxl->rom->num_pages * getpagesize (),
                                     (uint64_t)(uintptr_t)qxl->ram,
                                     (uint64_t)(uintptr_t)qxl->ram + qxl->surface0_size +
                                     (unsigned long)qxl->rom->num_pages * getpagesize ()
	);
    qxl->vram_mem_slot = setup_slot (qxl, 1,
                                     (unsigned long)qxl->vram_physical,
                                     (unsigned long)qxl->vram_physical + (unsigned long)qxl->vram_size,
                                     (uint64_t)(uintptr_t)qxl->vram,
                                     (uint64_t)(uintptr_t)qxl->vram + (uint64_t)qxl->vram_size);
#endif

    qxl_allocate_monitors_config(qxl);
}

void
qxl_mark_mem_unverifiable (qxl_screen_t *qxl)
{
    qxl_mem_unverifiable (qxl->mem);
    qxl_mem_unverifiable (qxl->surf_mem);
}


static uint64_t
qxl_garbage_collect_internal (qxl_screen_t *qxl, uint64_t id)
{
    /* We assume that there the two low bits of a pointer are
     * available. If the low one is set, then the command in
     * question is a cursor command
     */
#define POINTER_MASK ((1 << 2) - 1)

    union QXLReleaseInfo *info = u64_to_pointer (id & ~POINTER_MASK);
    struct QXLCursorCmd *cmd = (struct QXLCursorCmd *)info;
    struct QXLDrawable *drawable = (struct QXLDrawable *)info;
    struct QXLSurfaceCmd *surface_cmd = (struct QXLSurfaceCmd *)info;
    int is_cursor = FALSE;
    int is_surface = FALSE;
    int is_drawable = FALSE;

    if ((id & POINTER_MASK) == 1)
	is_cursor = TRUE;
    else if ((id & POINTER_MASK) == 2)
	is_surface = TRUE;
    else
	is_drawable = TRUE;

    if (is_cursor && cmd->type == QXL_CURSOR_SET)
    {
	struct QXLCursor *cursor = (void *)virtual_address (
	    qxl, u64_to_pointer (cmd->u.set.shape), qxl->main_mem_slot);

	qxl_free (qxl->mem, cursor, "cursor image");
    }
    else if (is_drawable && drawable->type == QXL_DRAW_COPY)
    {
	struct QXLImage *image = virtual_address (
	    qxl, u64_to_pointer (drawable->u.copy.src_bitmap), qxl->main_mem_slot);

	if (image->descriptor.type == SPICE_IMAGE_TYPE_SURFACE)
	{
	    qxl_surface_unref (qxl->surface_cache, image->surface_image.surface_id);
	    qxl_surface_cache_sanity_check (qxl->surface_cache);
	    qxl_free (qxl->mem, image, "surface image");
	}
	else
	{
	    qxl_image_destroy (qxl, image);
	}
    }
    else if (is_drawable && drawable->type == QXL_DRAW_COMPOSITE)
    {
	struct QXLTransform *src_trans, *mask_trans;
	struct QXLImage *src_img, *mask_img;
	struct QXLComposite *composite = &drawable->u.composite;

	/* Source */
	src_img = virtual_address (
	    qxl, u64_to_pointer (drawable->u.composite.src), qxl->main_mem_slot);
	qxl_free (qxl->mem, src_img, "image struct");

	if (composite->src_transform)
	{
	    src_trans = virtual_address (
		qxl, u64_to_pointer (composite->src_transform), qxl->main_mem_slot);
	    qxl_free (qxl->mem, src_trans, "transform");
	}

	/* Mask */
	if (drawable->u.composite.mask)
	{
	    if (drawable->u.composite.mask_transform)
	    {
		mask_trans = virtual_address (
		    qxl, u64_to_pointer (drawable->u.composite.mask_transform), qxl->main_mem_slot);

		qxl_free (qxl->mem, mask_trans, "transform");
	    }

	    mask_img = virtual_address (
		qxl, u64_to_pointer (drawable->u.composite.mask), qxl->main_mem_slot);
	    qxl_free (qxl->mem, mask_img, "image struct");
	}
    }
    else if (is_surface && surface_cmd->type == QXL_SURFACE_CMD_DESTROY)
    {
	qxl_surface_recycle (qxl->surface_cache, surface_cmd->surface_id);
	qxl_surface_cache_sanity_check (qxl->surface_cache);
    }

    id = info->next;

    qxl_free (qxl->mem, info, "command");

    return id;
}

int
qxl_garbage_collect (qxl_screen_t *qxl)
{
    uint64_t id;
    int      i = 0;

    while (qxl_ring_pop (qxl->release_ring, &id))
    {
	while (id)
	{
	    id = qxl_garbage_collect_internal (qxl, id);

	    i++;
	}
    }

    return i;
}

static void
qxl_usleep (int useconds)
{
    struct timespec t;

    t.tv_sec = useconds / 1000000;
    t.tv_nsec = (useconds - (t.tv_sec * 1000000)) * 1000;

    errno = 0;
    while (nanosleep (&t, &t) == -1 && errno == EINTR)
	;
}

int
qxl_handle_oom (qxl_screen_t *qxl)
{
    qxl_io_notify_oom (qxl);

#if 0
    ErrorF (".");
    qxl_usleep (10000);
#endif

    if (!(qxl_garbage_collect (qxl)))
	qxl_usleep (10000);

    return qxl_garbage_collect (qxl);
}

void *
qxl_allocnf (qxl_screen_t *qxl, unsigned long size, const char *name)
{
    void *result;
    int n_attempts = 0;

#if 0
    static int nth_oom = 1;
#endif

    qxl_garbage_collect (qxl);

    while (!(result = qxl_alloc (qxl->mem, size, name)))
    {
#if 0
	ErrorF ("eliminated memory (%d)\n", nth_oom++);
#endif
	if (!qxl_garbage_collect (qxl))
	{
	    if (qxl_handle_oom (qxl))
	    {
		n_attempts = 0;
	    }
	    else if (++n_attempts == 1000)
	    {
		ErrorF ("Out of memory allocating %ld bytes\n", size);
		qxl_mem_dump_stats (qxl->mem, "Out of mem - stats\n");
		fprintf (stderr, "Out of memory\n");
		exit (1);
	    }
	}
    }

    return result;
}
