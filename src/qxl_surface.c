/*
 * Copyright 2010 Red Hat, Inc.
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

/* The life cycle of surfaces
 *
 *    free => live => dead => destroyed => free
 *
 * A 'free' surface is one that is not allocated on the device. These
 * are stored on the 'free_surfaces' list.
 *
 * A 'live' surface is one that the X server is using for something. It
 * has an associated pixmap. It is allocated in the device. These are stored on
 * the "live_surfaces" list.
 *
 * A 'dead' surface is one that the X server is no using any more, but
 * is still allocated in the device. These surfaces may be stored in the
 * cache, from where they can be resurrected. The cache holds a ref on the
 * surfaces. 
 *
 * A 'destroyed' surface is one whose ref count has reached 0. It is no
 * longer being referenced by either the server or the device or the cache.
 * When a surface enters this state, the associated pixman images are freed, and
 * a destroy command is sent. This will eventually trigger a 'recycle' call,
 * which puts the surface into the 'free' state.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "qxl.h"

#ifdef DEBUG_SURFACE_LIFECYCLE
#include <stdio.h>

static FILE* surface_log;
#endif

typedef struct evacuated_surface_t evacuated_surface_t;

struct qxl_surface_t
{
    surface_cache_t    *cache;
    
    uint32_t	        id;

    pixman_image_t *	dev_image;
    pixman_image_t *	host_image;

    uxa_access_t	access_type;
    RegionRec		access_region;

    void *		address;
    void *		end;
    
    qxl_surface_t *	next;
    qxl_surface_t *	prev;	/* Only used in the 'live'
				 * chain in the surface cache
				 */

    int			in_use;
    int			bpp;		/* bpp of the pixmap */
    int			ref_count;

    PixmapPtr		pixmap;

    struct evacuated_surface_t *evacuated;

    union
    {
	qxl_surface_t *copy_src;
	Pixel	       solid_pixel;
    } u;
};

struct evacuated_surface_t
{
    pixman_image_t	*image;
    PixmapPtr		 pixmap;
    int			 bpp;

    evacuated_surface_t *prev;
    evacuated_surface_t *next;
};

#define N_CACHED_SURFACES 64

/*
 * Surface cache
 */
struct surface_cache_t
{
    qxl_screen_t *qxl;
    
    /* Array of surfaces (not a linked list).
     * All surfaces, excluding the primary one, indexed by surface id.
     */
    qxl_surface_t *all_surfaces;

    /* All surfaces that the driver is currently using (linked through next/prev) */
    qxl_surface_t *live_surfaces;

    /* All surfaces that need to be allocated (linked through next, but not prev) */
    qxl_surface_t *free_surfaces;

    /* Surfaces that are already allocated, but not in used by the driver,
     * linked through next
     */
    qxl_surface_t *cached_surfaces[N_CACHED_SURFACES];
};

#ifdef DEBUG_SURFACE_LIFECYCLE
static void debug_surface_open(void)
{
    if (surface_log)
        return;
    surface_log = fopen("/tmp/xf86-video-qxl.surface.log", "w+");
    if (!surface_log)
    {
        fprintf(stderr, "error creating surface log file (DEBUG_SURFACE_LIFECYCLE)\n");
        exit(-1);
    }
}

static int surface_count(qxl_surface_t *surface)
{
    int i;

    for (i = 0; surface ;++i, surface = surface->next);
    return i;
}

static void debug_surface_log(surface_cache_t *cache)
{
    int  live_n, free_n;

    debug_surface_open();
    live_n = surface_count(cache->live_surfaces);
    free_n = surface_count(cache->free_surfaces);
    fprintf(surface_log, "live,free,sum = %d, %d, %d\n", live_n, free_n,
            live_n + free_n);
    fflush(surface_log);
}

#else
#define debug_surface_log(cache)
#endif


static Bool
surface_cache_init (surface_cache_t *cache, qxl_screen_t *qxl)
{
    int n_surfaces = qxl->rom->n_surfaces;
    int i;

    if (!cache->all_surfaces)
    {
        /* all_surfaces is not freed when evacuating, since surfaces are still
         * tied to pixmaps that may be destroyed after evacuation before
         * recreation */
        cache->all_surfaces = calloc (n_surfaces, sizeof (qxl_surface_t));
        if (!cache->all_surfaces)
            return FALSE;
    }

    memset (cache->all_surfaces, 0, n_surfaces * sizeof (qxl_surface_t));
    memset (cache->cached_surfaces, 0, N_CACHED_SURFACES * sizeof (qxl_surface_t *));
    
    cache->free_surfaces = NULL;
    cache->live_surfaces = NULL;
    
    for (i = 0; i < n_surfaces; ++i)
    {
	cache->all_surfaces[i].id = i;
	cache->all_surfaces[i].cache = cache;
	cache->all_surfaces[i].dev_image = NULL;
	cache->all_surfaces[i].host_image = NULL;
	cache->all_surfaces[i].evacuated = NULL;
	
	REGION_INIT (
	    NULL, &(cache->all_surfaces[i].access_region), (BoxPtr)NULL, 0);
	cache->all_surfaces[i].access_type = UXA_ACCESS_RO;

	if (i) /* surface 0 is the primary surface */
	{
	    cache->all_surfaces[i].next = cache->free_surfaces;
	    cache->free_surfaces = &(cache->all_surfaces[i]);
	    cache->all_surfaces[i].in_use = FALSE;
	}
    }

    return TRUE;
}

surface_cache_t *
qxl_surface_cache_create (qxl_screen_t *qxl)
{
    surface_cache_t *cache = malloc (sizeof *cache);

    if (!cache)
	return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->qxl = qxl;
    if (!surface_cache_init (cache, qxl))
    {
	free (cache);
	return NULL;
    }

    return cache;
}

void
qxl_surface_cache_sanity_check (surface_cache_t *qxl)
{
#if 0
    qxl_surface_t *s;

    for (s = qxl->live_surfaces; s != NULL; s = s->next)
    {
	PixmapPtr pixmap = s->pixmap;

	if (! (get_surface (pixmap) == s) )
	{
	    ErrorF ("Surface %p has pixmap %p, but pixmap %p has surface %p\n",
		    s, pixmap, pixmap, get_surface (pixmap));

	    assert (0);
	}
    }
#endif
}

static void
print_cache_info (surface_cache_t *cache)
{
    int i;
    int n_surfaces = 0;

    ErrorF ("Cache contents:  ");
    for (i = 0; i < N_CACHED_SURFACES; ++i)
    {
	if (cache->cached_surfaces[i])
	{
	    ErrorF ("%4d ", cache->cached_surfaces[i]->id);
	    n_surfaces++;
	}
	else
	    ErrorF ("null ");
    }

    ErrorF ("    total: %d\n", n_surfaces);
}

static void
get_formats (int bpp, SpiceBitmapFmt *format, pixman_format_code_t *pformat)
{
    switch (bpp)
    {
    case 8:
	*format = SPICE_SURFACE_FMT_8_A;
	*pformat = PIXMAN_a8;
	break;

    case 16:
	*format = SPICE_SURFACE_FMT_16_565;
	*pformat = PIXMAN_r5g6b5;
	break;

    case 24:
	*format = SPICE_SURFACE_FMT_32_xRGB;
	*pformat = PIXMAN_a8r8g8b8;
	break;
	
    case 32:
	*format = SPICE_SURFACE_FMT_32_ARGB;
	*pformat = PIXMAN_a8r8g8b8;
	break;

    default:
	*format = *pformat = -1;
	break;
    }
}
		 
static qxl_surface_t *
surface_get_from_cache (surface_cache_t *cache, int width, int height, int bpp)
{
    int i;

    for (i = 0; i < N_CACHED_SURFACES; ++i)
    {
	qxl_surface_t *s = cache->cached_surfaces[i];

	if (s && bpp == s->bpp)
	{
	    int w = pixman_image_get_width (s->host_image);
	    int h = pixman_image_get_height (s->host_image);
	    
	    if (width <= w && width * 4 > w && height <= h && height * 4 > h)
	    {
		cache->cached_surfaces[i] = NULL;

		return s;
	    }
	}
    }

    return NULL;
}

static int n_live;

void
qxl_surface_recycle (surface_cache_t *cache, uint32_t id)
{
    qxl_surface_t *surface = cache->all_surfaces + id;

    n_live--;
    qxl_free (cache->qxl->surf_mem, surface->address, "surface memory");

    surface->next = cache->free_surfaces;
    cache->free_surfaces = surface;
}

/*
 * mode is used for the whole virtual screen, not for a specific head.
 * For a single head where virtual size is equal to the head size, they are
 * equal. For multiple heads this mode will not match any existing heads and
 * will be the containing virtual size.
 */
qxl_surface_t *
qxl_surface_cache_create_primary (surface_cache_t	*cache,
				  struct QXLMode	*mode)
{
    struct QXLRam *ram_header =
	(void *)((unsigned long)cache->qxl->ram + cache->qxl->rom->ram_header_offset);
    struct QXLSurfaceCreate *create = &(ram_header->create_surface);
    pixman_format_code_t format;
    uint8_t *dev_addr;
    pixman_image_t *dev_image, *host_image;
    qxl_surface_t *surface;
    qxl_screen_t *qxl = cache->qxl;

    if (mode->bits == 16)
    {
	format = PIXMAN_x1r5g5b5;
    }
    else if (mode->bits == 32)
    {
	format = PIXMAN_x8r8g8b8;
    }
    else
    {
	xf86DrvMsg (cache->qxl->pScrn->scrnIndex, X_ERROR,
		    "Unknown bit depth %d\n", mode->bits);
	return NULL;
    }
	
    create->width = mode->x_res;
    create->height = mode->y_res;
    create->stride = - mode->stride;
    create->format = mode->bits;
    create->position = 0; /* What is this? The Windows driver doesn't use it */
    create->flags = 0;
    create->type = QXL_SURF_TYPE_PRIMARY;
    create->mem = physical_address (cache->qxl, cache->qxl->ram, cache->qxl->main_mem_slot);

    qxl_io_create_primary(qxl);

    dev_addr = (uint8_t *)qxl->ram + mode->stride * (mode->y_res - 1);

    dev_image = pixman_image_create_bits (format, mode->x_res, mode->y_res,
					  (uint32_t *)dev_addr, -mode->stride);

    if (qxl->fb != NULL)
        free(qxl->fb);
    qxl->fb = calloc (qxl->virtual_x * qxl->virtual_y, 4);
    if (!qxl->fb)
        return NULL;

    host_image = pixman_image_create_bits (format, 
					   qxl->virtual_x, qxl->virtual_y,
					   qxl->fb, mode->stride);
#if 0
    xf86DrvMsg(cache->qxl->pScrn->scrnIndex, X_ERROR,
               "testing dev_image memory (%d x %d)\n",
               mode->x_res, mode->y_res);
    memset(qxl->ram, 0, mode->stride * mode->y_res);
    xf86DrvMsg(cache->qxl->pScrn->scrnIndex, X_ERROR,
               "testing host_image memory\n");
    memset(qxl->fb, 0, mode->stride * mode->y_res);
#endif

    surface = malloc (sizeof *surface);
    surface->id = 0;
    surface->dev_image = dev_image;
    surface->host_image = host_image;
    surface->cache = cache;
    surface->bpp = mode->bits;
    surface->next = NULL;
    surface->prev = NULL;
    surface->evacuated = NULL;
    
    REGION_INIT (NULL, &(surface->access_region), (BoxPtr)NULL, 0);
    surface->access_type = UXA_ACCESS_RO;
    
    return surface;
}

static struct QXLSurfaceCmd *
make_surface_cmd (surface_cache_t *cache, uint32_t id, QXLSurfaceCmdType type)
{
    struct QXLSurfaceCmd *cmd;
    qxl_screen_t *qxl = cache->qxl;

    cmd = qxl_allocnf (qxl, sizeof *cmd, "surface command");

    cmd->release_info.id = pointer_to_u64 (cmd) | 2;
    cmd->type = type;
    cmd->flags = 0;
    cmd->surface_id = id;
    
    return cmd;
}

static void
push_surface_cmd (surface_cache_t *cache, struct QXLSurfaceCmd *cmd)
{
    struct QXLCommand command;
    qxl_screen_t *qxl = cache->qxl;

    command.type = QXL_CMD_SURFACE;
    command.data = physical_address (qxl, cmd, qxl->main_mem_slot);
    
    qxl_ring_push (qxl->command_ring, &command);
}

enum ROPDescriptor
{
    ROPD_INVERS_SRC = (1 << 0),
    ROPD_INVERS_BRUSH = (1 << 1),
    ROPD_INVERS_DEST = (1 << 2),
    ROPD_OP_PUT = (1 << 3),
    ROPD_OP_OR = (1 << 4),
    ROPD_OP_AND = (1 << 5),
    ROPD_OP_XOR = (1 << 6),
    ROPD_OP_BLACKNESS = (1 << 7),
    ROPD_OP_WHITENESS = (1 << 8),
    ROPD_OP_INVERS = (1 << 9),
    ROPD_INVERS_RES = (1 <<10),
};

static struct QXLDrawable *
make_drawable (qxl_screen_t *qxl, int surface, uint8_t type,
	       const struct QXLRect *rect
	       /* , pRegion clip */)
{
    struct QXLDrawable *drawable;
    int i;
    
    drawable = qxl_allocnf (qxl, sizeof *drawable, "drawable command");
    assert(drawable);
    
    drawable->release_info.id = pointer_to_u64 (drawable);
    
    drawable->type = type;
    
    drawable->surface_id = surface;		/* Only primary for now */
    drawable->effect = QXL_EFFECT_OPAQUE;
    drawable->self_bitmap = 0;
    drawable->self_bitmap_area.top = 0;
    drawable->self_bitmap_area.left = 0;
    drawable->self_bitmap_area.bottom = 0;
    drawable->self_bitmap_area.right = 0;
    /* FIXME: add clipping */
    drawable->clip.type = SPICE_CLIP_TYPE_NONE;
    
    /*
     * surfaces_dest[i] should apparently be filled out with the
     * surfaces that we depend on, and surface_rects should be
     * filled with the rectangles of those surfaces that we
     * are going to use.
     */
    for (i = 0; i < 3; ++i)
	drawable->surfaces_dest[i] = -1;
    
    if (rect)
	drawable->bbox = *rect;
    
    drawable->mm_time = qxl->rom->mm_clock;
    
    return drawable;
}

static void
push_drawable (qxl_screen_t *qxl, struct QXLDrawable *drawable)
{
    struct QXLCommand cmd;
    
    /* When someone runs "init 3", the device will be 
     * switched into VGA mode and there is nothing we
     * can do about it. We get no notification.
     * 
     * However, if commands are submitted when the device
     * is in VGA mode, they will be queued up, and then
     * the next time a mode set set, an assertion in the
     * device will take down the entire virtual machine.
     */
    if (qxl->pScrn->vtSema)
    {
	cmd.type = QXL_CMD_DRAW;
	cmd.data = physical_address (qxl, drawable, qxl->main_mem_slot);
	
	qxl_ring_push (qxl->command_ring, &cmd);
    }
}

static void
submit_fill (qxl_screen_t *qxl, int id,
	     const struct QXLRect *rect, uint32_t color)
{
    struct QXLDrawable *drawable;
    
    drawable = make_drawable (qxl, id, QXL_DRAW_FILL, rect);
    
    drawable->u.fill.brush.type = SPICE_BRUSH_TYPE_SOLID;
    drawable->u.fill.brush.u.color = color;
    drawable->u.fill.rop_descriptor = ROPD_OP_PUT;
    drawable->u.fill.mask.flags = 0;
    drawable->u.fill.mask.pos.x = 0;
    drawable->u.fill.mask.pos.y = 0;
    drawable->u.fill.mask.bitmap = 0;
    
    push_drawable (qxl, drawable);
}

static qxl_surface_t *
surface_get_from_free_list (surface_cache_t *cache)
{
    qxl_surface_t *result = NULL;

    if (cache->free_surfaces)
    {
	qxl_surface_t *s;

	result = cache->free_surfaces;
	cache->free_surfaces = cache->free_surfaces->next;

	result->next = NULL;
	result->in_use = TRUE;
	result->ref_count = 1;
	result->pixmap = NULL;

	for (s = cache->free_surfaces; s; s = s->next)
	{
	    if (s->id == result->id)
		ErrorF ("huh: %d to be returned, but %d is in list\n",
			s->id, result->id);

	    assert (s->id != result->id);
	}
    }
    
    return result;
}

static int
align (int x)
{
    return x;
}

static qxl_surface_t *
surface_send_create (surface_cache_t *cache,
		     int	      width,
		     int	      height,
		     int	      bpp)
{
    SpiceBitmapFmt format;
    pixman_format_code_t pformat;
    struct QXLSurfaceCmd *cmd;
    int stride;
    uint32_t *dev_addr;
    int n_attempts = 0;
    qxl_screen_t *qxl = cache->qxl;
    qxl_surface_t *surface;
    void *address;

    get_formats (bpp, &format, &pformat);
    
    width = align (width);
    height = align (height);
    
    stride = width * PIXMAN_FORMAT_BPP (pformat) / 8;
    stride = (stride + 3) & ~3;

    /* the final + stride is to work around a bug where the device apparently 
     * scribbles after the end of the image
     */
    qxl_garbage_collect (qxl);
retry2:
    address = qxl_alloc (qxl->surf_mem, stride * height + stride, "surface memory");

    if (!address)
    {
	ErrorF ("- %dth attempt\n", n_attempts++);

	if (qxl_garbage_collect (qxl))
	    goto retry2;

	ErrorF ("- OOM at %d %d %d (= %d bytes)\n", width, height, bpp, width * height * (bpp / 8));
	print_cache_info (cache);
	
	if (qxl_handle_oom (qxl))
	{
	    while (qxl_garbage_collect (qxl))
		;
	    goto retry2;
	}

	ErrorF ("Out of video memory: Could not allocate %d bytes\n",
		stride * height + stride);
	
	return NULL;
    }

retry:
    surface = surface_get_from_free_list (cache);
    if (!surface)
    {
	if (!qxl_handle_oom (cache->qxl))
	{
	    ErrorF ("  Out of surfaces\n");
	    qxl_free (qxl->surf_mem, address, "surface memory");
	    return NULL;
	}
	else
	    goto retry;
    }

    surface->address = address;    
    surface->end = (char *)address + stride * height;
    
    cmd = make_surface_cmd (cache, surface->id, QXL_SURFACE_CMD_CREATE);

    cmd->u.surface_create.format = format;
    cmd->u.surface_create.width = width;
    cmd->u.surface_create.height = height;
    cmd->u.surface_create.stride = - stride;

    cmd->u.surface_create.data =
      physical_address (qxl, surface->address, qxl->vram_mem_slot);

    push_surface_cmd (cache, cmd);

    dev_addr = (uint32_t *)((uint8_t *)surface->address + stride * (height - 1));

    surface->dev_image = pixman_image_create_bits (
	pformat, width, height, dev_addr, - stride);

    surface->host_image = pixman_image_create_bits (
	pformat, width, height, NULL, -1);

    surface->bpp = bpp;

    n_live++;
    
    return surface;
}

qxl_surface_t *
qxl_surface_create (surface_cache_t *    cache,
		    int			 width,
		    int			 height,
		    int			 bpp)
{
    qxl_surface_t *surface;

    if (!cache->qxl->enable_surfaces)
	return NULL;
    
    if ((bpp & 3) != 0)
    {
	ErrorF ("%s: Bad bpp: %d (%d)\n", __FUNCTION__, bpp, bpp & 7);
	return NULL;
    }

#if 0
    if (bpp == 8)
      {
	static int warned;
	if (!warned)
	{
	    warned = 1;
	    ErrorF ("bpp == 8 triggers bugs in spice apparently\n");
	}
	
	return NULL;
      }
#endif
    
    if (bpp != 8 && bpp != 16 && bpp != 32 && bpp != 24)
    {
	ErrorF ("%s: Unknown bpp\n", __FUNCTION__);
	return NULL;
    }

    if (width == 0 || height == 0)
    {
	ErrorF ("%s: Zero width or height\n", __FUNCTION__);
	return NULL;
    }

    if (!(surface = surface_get_from_cache (cache, width, height, bpp)))
	if (!(surface = surface_send_create (cache, width, height, bpp)))
	    return NULL;
    
    surface->next = cache->live_surfaces;
    surface->prev = NULL;
    if (cache->live_surfaces)
	cache->live_surfaces->prev = surface;
    cache->live_surfaces = surface;
    
    return surface;
}

void
qxl_surface_set_pixmap (qxl_surface_t *surface, PixmapPtr pixmap)
{
    surface->pixmap = pixmap;

    assert (get_surface (pixmap) == surface);
}

static void
unlink_surface (qxl_surface_t *surface)
{
    if (surface->id != 0)
    {
        if (surface->prev)
            surface->prev->next = surface->next;
        else
            surface->cache->live_surfaces = surface->next;
    }

    debug_surface_log(surface->cache);
    
    if (surface->next)
	surface->next->prev = surface->prev;

    surface->pixmap = NULL;
    
    surface->prev = NULL;
    surface->next = NULL;
}

static void
surface_destroy (qxl_surface_t *surface)
{
    struct QXLSurfaceCmd *cmd;

    if (surface->dev_image)
	pixman_image_unref (surface->dev_image);
    if (surface->host_image)
	pixman_image_unref (surface->host_image);

#if 0
    ErrorF("destroy %ld\n", (long int)surface->end - (long int)surface->address);
#endif
    cmd = make_surface_cmd (surface->cache, surface->id, QXL_SURFACE_CMD_DESTROY);

    push_surface_cmd (surface->cache, cmd);
}

static void
surface_add_to_cache (qxl_surface_t *surface)
{
    surface_cache_t *cache = surface->cache;
    int oldest = -1;
    int n_surfaces = 0;
    int i, delta;
    int destroy_id = -1;
    qxl_surface_t *destroy_surface = NULL;

    surface->ref_count++;
    
    for (i = 0; i < N_CACHED_SURFACES; ++i)
    {
	if (cache->cached_surfaces[i])
	{
	    oldest = i;
	    n_surfaces++;
	}
    }
    
    if (n_surfaces == N_CACHED_SURFACES)
    {
	destroy_id = cache->cached_surfaces[oldest]->id;
	
	destroy_surface = cache->cached_surfaces[oldest];
	
	cache->cached_surfaces[oldest] = NULL;
	
	for (i = 0; i < N_CACHED_SURFACES; ++i)
	    assert (!cache->cached_surfaces[i] ||
		    cache->cached_surfaces[i]->id != destroy_id);
    }
    
    delta = 0;
    for (i = N_CACHED_SURFACES - 1; i >= 0; i--)
    {
	if (cache->cached_surfaces[i])
	{
	    if (delta > 0)
	    {
		cache->cached_surfaces[i + delta] =
		    cache->cached_surfaces[i];
		
		assert (cache->cached_surfaces[i + delta]->id != destroy_id);
		
		cache->cached_surfaces[i] = NULL;
	    }
	}
	else
	{
	    delta++;
	}
    }
    
    assert (delta > 0);
    
    cache->cached_surfaces[i + delta] = surface;
    
    for (i = 0; i < N_CACHED_SURFACES; ++i)
	assert (!cache->cached_surfaces[i] || cache->cached_surfaces[i]->id != destroy_id);

    /* Note that sending a destroy command can trigger callbacks into
     * this function (due to memory management), so we have to
     * do this after updating the cache
     */
    if (destroy_surface)
	qxl_surface_unref (destroy_surface->cache, destroy_surface->id);
}

void
qxl_surface_unref (surface_cache_t *cache, uint32_t id)
{
    if (id != 0)
    {
	qxl_surface_t *surface = cache->all_surfaces + id;

	if (--surface->ref_count == 0)
	    surface_destroy (surface);
    }
}

void
qxl_surface_kill (qxl_surface_t *surface)
{
    struct evacuated_surface_t *ev = surface->evacuated;

    if (ev)
    {
        /* server side surface is already destroyed (via reset), don't
         * resend a destroy. Just mark surface as not to be recreated */
        ev->pixmap = NULL;
        if (ev->image)
            pixman_image_unref (ev->image);
        if (ev->next)
            ev->next->prev = ev->prev;
        if (ev->prev)
            ev->prev->next = ev->next;
        free(ev);
        surface->evacuated = NULL;
        return;
    }

    unlink_surface (surface);

    if (!surface->cache->all_surfaces) {
        return;
    }

    if (surface->id != 0					&&
        surface->host_image                                     &&
	pixman_image_get_width (surface->host_image) >= 128	&&
	pixman_image_get_height (surface->host_image) >= 128)
    {
	surface_add_to_cache (surface);
    }
    
    qxl_surface_unref (surface->cache, surface->id);
}

/* send anything pending to the other side */
void
qxl_surface_flush (qxl_surface_t *surface)
{
    ;
}

/* access */
static void
download_box_no_update (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    pixman_image_composite (PIXMAN_OP_SRC,
                            surface->dev_image,
                            NULL,
                            surface->host_image,
                            x1, y1, 0, 0, x1, y1, x2 - x1, y2 - y1);
}

static void
download_box (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    struct QXLRam *ram_header = get_ram_header (surface->cache->qxl);
    
    ram_header->update_area.top = y1;
    ram_header->update_area.bottom = y2;
    ram_header->update_area.left = x1;
    ram_header->update_area.right = x2;
    
    ram_header->update_surface = surface->id;

    qxl_update_area(surface->cache->qxl);

    download_box_no_update(surface, x1, y1, x2, y2);
}

Bool
qxl_surface_prepare_access (qxl_surface_t  *surface,
			    PixmapPtr       pixmap,
			    RegionPtr       region,
			    uxa_access_t    access)
{
    int n_boxes;
    BoxPtr boxes;
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    RegionRec new;

    if (!pScrn->vtSema)
        return FALSE;

    REGION_INIT (NULL, &new, (BoxPtr)NULL, 0);
    REGION_SUBTRACT (NULL, &new, region, &surface->access_region);

    if (access == UXA_ACCESS_RW)
	surface->access_type = UXA_ACCESS_RW;
    
    region = &new;
    
    n_boxes = REGION_NUM_RECTS (region);
    boxes = REGION_RECTS (region);

    if (n_boxes < 25)
    {
	while (n_boxes--)
	{
	    download_box (surface, boxes->x1, boxes->y1, boxes->x2, boxes->y2);
	    
	    boxes++;
	}
    }
    else
    {
	download_box (
	    surface,
	    new.extents.x1, new.extents.y1, new.extents.x2, new.extents.y2);
    }
    
    REGION_UNION (pScreen,
		  &(surface->access_region),
		  &(surface->access_region),
		      region);
    
    REGION_UNINIT (NULL, &new);
    
    pScreen->ModifyPixmapHeader(
	pixmap,
	pixmap->drawable.width,
	pixmap->drawable.height,
	-1, -1, -1,
	pixman_image_get_data (surface->host_image));

    pixmap->devKind = pixman_image_get_stride (surface->host_image);
    
    return TRUE;
}

static void
translate_rect (struct QXLRect *rect)
{
    rect->right -= rect->left;
    rect->bottom -= rect->top;
    rect->left = rect->top = 0;
}

static void
real_upload_box (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    struct QXLRect rect;
    struct QXLDrawable *drawable;
    struct QXLImage *image;
    qxl_screen_t *qxl = surface->cache->qxl;
    uint32_t *data;
    int stride;
    
    rect.left = x1;
    rect.right = x2;
    rect.top = y1;
    rect.bottom = y2;
    
    drawable = make_drawable (qxl, surface->id, QXL_DRAW_COPY, &rect);
    drawable->u.copy.src_area = rect;
    translate_rect (&drawable->u.copy.src_area);
    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    data = pixman_image_get_data (surface->host_image);
    stride = pixman_image_get_stride (surface->host_image);
    
    image = qxl_image_create (
	qxl, (const uint8_t *)data, x1, y1, x2 - x1, y2 - y1, stride, 
	surface->bpp == 24 ? 4 : surface->bpp / 8, TRUE);
    drawable->u.copy.src_bitmap =
	physical_address (qxl, image, qxl->main_mem_slot);
    
    push_drawable (qxl, drawable);
}

#define TILE_WIDTH 512
#define TILE_HEIGHT 512

static void
upload_box (qxl_surface_t *surface, int x1, int y1, int x2, int y2)
{
    int tile_x1, tile_y1;

    for (tile_y1 = y1; tile_y1 < y2; tile_y1 += TILE_HEIGHT)
    {
	for (tile_x1 = x1; tile_x1 < x2; tile_x1 += TILE_WIDTH)
	{
	    int tile_x2 = tile_x1 + TILE_WIDTH;
	    int tile_y2 = tile_y1 + TILE_HEIGHT;

	    if (tile_x2 > x2)
		tile_x2 = x2;
	    if (tile_y2 > y2)
		tile_y2 = y2;

	    real_upload_box (surface, tile_x1, tile_y1, tile_x2, tile_y2);
	}
    }
}

void
qxl_surface_finish_access (qxl_surface_t *surface, PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    int w = pixmap->drawable.width;
    int h = pixmap->drawable.height;
    int n_boxes;
    BoxPtr boxes;

    n_boxes = REGION_NUM_RECTS (&surface->access_region);
    boxes = REGION_RECTS (&surface->access_region);

    if (surface->access_type == UXA_ACCESS_RW)
    {
	if (n_boxes < 25)
	{
	    while (n_boxes--)
	    {
		upload_box (surface, boxes->x1, boxes->y1, boxes->x2, boxes->y2);
		
		boxes++;
	    }
	}
	else
	{
	    upload_box (surface,
			surface->access_region.extents.x1,
			surface->access_region.extents.y1,
			surface->access_region.extents.x2,
			surface->access_region.extents.y2);
	}
    }

    REGION_EMPTY (pScreen, &surface->access_region);
    surface->access_type = UXA_ACCESS_RO;
    
    pScreen->ModifyPixmapHeader(pixmap, w, h, -1, -1, 0, NULL);
}

void *
qxl_surface_cache_evacuate_all (surface_cache_t *cache)
{
    evacuated_surface_t *evacuated_surfaces = NULL;
    qxl_surface_t *s;
    int i;

    for (i = 0; i < N_CACHED_SURFACES; ++i)
    {
	if (cache->cached_surfaces[i])
	{
            surface_destroy (cache->cached_surfaces[i]);
	    cache->cached_surfaces[i] = NULL;
	}
    }

    s = cache->live_surfaces;
    while (s != NULL)
    {
	qxl_surface_t *next = s->next;
	evacuated_surface_t *evacuated = malloc (sizeof (evacuated_surface_t));
	int width, height;

	width = pixman_image_get_width (s->host_image);
	height = pixman_image_get_height (s->host_image);

	download_box (s, 0, 0, width, height);

	evacuated->image = s->host_image;
	evacuated->pixmap = s->pixmap;

	assert (get_surface (evacuated->pixmap) == s);
	
	evacuated->bpp = s->bpp;
	
	s->host_image = NULL;

	unlink_surface (s);
	
	evacuated->next = evacuated_surfaces;
        if (evacuated_surfaces)
            evacuated_surfaces->prev = evacuated;
	evacuated_surfaces = evacuated;
        s->evacuated = evacuated;

	s = next;
    }

    cache->live_surfaces = NULL;
    cache->free_surfaces = NULL;

    return evacuated_surfaces;
}

void
qxl_surface_cache_replace_all (surface_cache_t *cache, void *data)
{
    evacuated_surface_t *ev;

    if (!surface_cache_init (cache, cache->qxl))
    {
	/* FIXME: report the error */
	return;
    }
    
    ev = data;
    while (ev != NULL)
    {
	evacuated_surface_t *next = ev->next;
	int width = pixman_image_get_width (ev->image);
	int height = pixman_image_get_height (ev->image);
	qxl_surface_t *surface;

	surface = qxl_surface_create (cache, width, height, ev->bpp);

	assert (surface->host_image);
	assert (surface->dev_image);

	pixman_image_unref (surface->host_image);
	surface->host_image = ev->image;

	upload_box (surface, 0, 0, width, height);

	set_surface (ev->pixmap, surface);

	qxl_surface_set_pixmap (surface, ev->pixmap);

	free (ev);
	
	ev = next;
    }

    qxl_surface_cache_sanity_check (cache);

}

#ifdef DEBUG_REGIONS
static void
print_region (const char *header, RegionPtr pRegion)
{
    int nbox = REGION_NUM_RECTS (pRegion);
    BoxPtr pbox = REGION_RECTS (pRegion);
    
    ErrorF ("%s", header);

    if (nbox == 0)
	ErrorF (" (empty)\n");
    else
	ErrorF ("\n");
    
    while (nbox--)
    {
	ErrorF ("   %d %d %d %d (size: %d %d)\n",
		pbox->x1, pbox->y1, pbox->x2, pbox->y2,
		pbox->x2 - pbox->x1, pbox->y2 - pbox->y1);
	
	pbox++;
    }
}
#endif // DEBUG_REGIONS

/* solid */
Bool
qxl_surface_prepare_solid (qxl_surface_t *destination,
			   Pixel	  fg)
{
    if (!REGION_NIL (&(destination->access_region)))
    {
	ErrorF (" solid not in vmem\n");
    }

#ifdef DEBUG_REGIONS
    print_region ("prepare solid", &(destination->access_region));
#endif
    
    destination->u.solid_pixel = fg; //  ^ (rand() >> 16);

    return TRUE;
}

void
qxl_surface_solid (qxl_surface_t *destination,
		   int	          x1,
		   int	          y1,
		   int	          x2,
		   int	          y2)
{
    qxl_screen_t *qxl = destination->cache->qxl;
    struct QXLRect qrect;
    uint32_t p;

    qrect.top = y1;
    qrect.bottom = y2;
    qrect.left = x1;
    qrect.right = x2;

    p = destination->u.solid_pixel;
    
    submit_fill (qxl, destination->id, &qrect, p);
}

/* copy */
Bool
qxl_surface_prepare_copy (qxl_surface_t *dest,
			  qxl_surface_t *source)
{
    if (!REGION_NIL (&(dest->access_region))	||
	!REGION_NIL (&(source->access_region)))
    {
	return FALSE;
    }

    dest->u.copy_src = source;

    return TRUE;
}

void
qxl_surface_copy (qxl_surface_t *dest,
		  int  src_x1, int src_y1,
		  int  dest_x1, int dest_y1,
		  int width, int height)
{
    qxl_screen_t *qxl = dest->cache->qxl;
    struct QXLDrawable *drawable;
    struct QXLRect qrect;

#ifdef DEBUG_REGIONS
    print_region (" copy src", &(dest->u.copy_src->access_region));
    print_region (" copy dest", &(dest->access_region));
#endif

    qrect.top = dest_y1;
    qrect.bottom = dest_y1 + height;
    qrect.left = dest_x1;
    qrect.right = dest_x1 + width;
    
    if (dest->id == dest->u.copy_src->id)
    {
	drawable = make_drawable (qxl, dest->id, QXL_COPY_BITS, &qrect);

	drawable->u.copy_bits.src_pos.x = src_x1;
	drawable->u.copy_bits.src_pos.y = src_y1;
    }
    else
    {
	struct QXLImage *image = qxl_allocnf (qxl, sizeof *image, "surface image struct");

	dest->u.copy_src->ref_count++;

	image->descriptor.id = 0;
	image->descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
	image->descriptor.width = 0;
	image->descriptor.height = 0;
	image->surface_image.surface_id = dest->u.copy_src->id;

	drawable = make_drawable (qxl, dest->id, QXL_DRAW_COPY, &qrect);

	drawable->u.copy.src_bitmap = physical_address (qxl, image, qxl->main_mem_slot);
	drawable->u.copy.src_area.left = src_x1;
	drawable->u.copy.src_area.top = src_y1;
	drawable->u.copy.src_area.right = src_x1 + width;
	drawable->u.copy.src_area.bottom = src_y1 + height;
	drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
	drawable->u.copy.scale_mode = 0;
	drawable->u.copy.mask.flags = 0;
	drawable->u.copy.mask.pos.x = 0;
	drawable->u.copy.mask.pos.y = 0;
	drawable->u.copy.mask.bitmap = 0;

	drawable->surfaces_dest[0] = dest->u.copy_src->id;
	drawable->surfaces_rects[0] = drawable->u.copy.src_area;
 	
	assert (src_x1 >= 0);
	assert (src_y1 >= 0);

	if (width > pixman_image_get_width (dest->u.copy_src->host_image))
	{
	    ErrorF ("dest w: %d   src w: %d\n",
		    width, pixman_image_get_width (dest->u.copy_src->host_image));
	}
	
	assert (width <= pixman_image_get_width (dest->u.copy_src->host_image));
	assert (height <= pixman_image_get_height (dest->u.copy_src->host_image));
    }

    push_drawable (qxl, drawable);
}

Bool
qxl_surface_put_image (qxl_surface_t *dest,
		       int x, int y, int width, int height,
		       const char *src, int src_pitch)
{
    struct QXLDrawable *drawable;
    qxl_screen_t *qxl = dest->cache->qxl;
    struct QXLRect rect;
    struct QXLImage *image;
    
    rect.left = x;
    rect.right = x + width;
    rect.top = y;
    rect.bottom = y + height;

    drawable = make_drawable (qxl, dest->id, QXL_DRAW_COPY, &rect);

    drawable->u.copy.src_area.top = 0;
    drawable->u.copy.src_area.bottom = height;
    drawable->u.copy.src_area.left = 0;
    drawable->u.copy.src_area.right = width;

    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    image = qxl_image_create (
	qxl, (const uint8_t *)src, 0, 0, width, height, src_pitch,
	dest->bpp == 24 ? 4 : dest->bpp / 8, FALSE);
    drawable->u.copy.src_bitmap =
	physical_address (qxl, image, qxl->main_mem_slot);
    
    push_drawable (qxl, drawable);

    return TRUE;
}
