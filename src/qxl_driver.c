/*
 * Copyright 2008 Red Hat, Inc.
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

/** \file qxl_driver.c
 * \author Adam Jackson <ajax@redhat.com>
 *
 * This is qxl, a driver for the Qumranet paravirtualized graphics device
 * in qemu.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include "qxl.h"
#include "assert.h"

#define qxlSaveState(x) do { (void)x; } while (0)
#define qxlRestoreState(x) do { (void)x; } while (0)

#if 0
#define CHECK_POINT() ErrorF ("%s: %d  (%s)\n", __FILE__, __LINE__, __FUNCTION__);
#endif
#define CHECK_POINT()

static int
garbage_collect (qxlScreen *qxl)
{
    uint64_t id;
    int i = 0;
    
    while (qxl_ring_pop (qxl->release_ring, &id))
    {
	while (id)
	{
	    /* We assume that there the two low bits of a pointer are
	     * available. If the low one is set, then the command in
	     * question is a cursor command
	     */
#define POINTER_MASK ((1 << 2) - 1)
	    
	    union qxl_release_info *info = (void *)(id & ~POINTER_MASK);
	    struct qxl_cursor_cmd *cmd = (struct qxl_cursor_cmd *)info;
	    struct qxl_drawable *drawable = (struct qxl_drawable *)info;
	    int is_cursor = FALSE;

	    if ((id & POINTER_MASK) == 1)
		is_cursor = TRUE;

	    if (is_cursor && cmd->type == QXL_CURSOR_SET)
	    {
		struct qxl_cursor *cursor = (void *)virtual_address (
		    qxl, (void *)cmd->u.set.shape);

		qxl_free (qxl->mem, cursor);
	    }
	    else if (!is_cursor && drawable->type == QXL_DRAW_COPY)
	    {
		struct qxl_image *image = virtual_address (
		    qxl, (void *)drawable->u.copy.src_bitmap);

		qxl_image_destroy (qxl, image);
	    }
	    
	    id = info->next;
	    
	    qxl_free (qxl->mem, info);
	}
    }

    return i > 0;
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

#if 0
static void
push_update_area (qxlScreen *qxl, const struct qxl_rect *area)
{
    struct qxl_update_cmd *update = qxl_allocnf (qxl, sizeof *update);
    struct qxl_command cmd;

    update->release_info.id = (uint64_t)update;
    update->area = *area;
    update->update_id = 0;

    cmd.type = QXL_CMD_UDPATE;
    cmd.data = physical_address (qxl, update);

    qxl_ring_push (qxl->command_ring, &cmd);
}
#endif

void *
qxl_allocnf (qxlScreen *qxl, unsigned long size)
{
    void *result;
    int n_attempts = 0;
    static int nth_oom = 1;

    garbage_collect (qxl);
    
    while (!(result = qxl_alloc (qxl->mem, size)))
    {
	struct qxl_ram_header *ram_header = (void *)((unsigned long)qxl->ram +
						     qxl->rom->ram_header_offset);
	
	/* Rather than go out of memory, we simply tell the
	 * device to dump everything
	 */
	ram_header->update_area.top = 0;
	ram_header->update_area.bottom = 1280;
	ram_header->update_area.left = 0;
	ram_header->update_area.right = 800;
	
	outb (qxl->io_base + QXL_IO_UPDATE_AREA, 0);
	
	ErrorF ("eliminated memory (%d)\n", nth_oom++);

	outb (qxl->io_base + QXL_IO_NOTIFY_OOM, 0);

	qxl_usleep (10000);
	
	if (garbage_collect (qxl))
	{
	    n_attempts = 0;
	}
	else if (++n_attempts == 1000)
	{
	    qxl_mem_dump_stats (qxl->mem, "Out of mem - stats\n");
	    
	    fprintf (stderr, "Out of memory\n");
	    exit (1);
	}
    }

    return result;
}

static Bool
qxlBlankScreen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static void
qxlUnmapMemory(qxlScreen *qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    if (qxl->ram)
	pci_device_unmap_range(qxl->pci, qxl->ram, qxl->pci->regions[0].size);
    if (qxl->vram)
	pci_device_unmap_range(qxl->pci, qxl->vram, qxl->pci->regions[1].size);
    if (qxl->rom)
	pci_device_unmap_range(qxl->pci, qxl->rom, qxl->pci->regions[2].size);
#else
    if (qxl->ram)
	xf86UnMapVidMem(scrnIndex, qxl->ram, qxl->pci->size[0]);
    if (qxl->vram)
	xf86UnMapVidMem(scrnIndex, qxl->vram, qxl->pci->size[1]);
    if (qxl->rom)
	xf86UnMapVidMem(scrnIndex, qxl->rom, qxl->pci->size[2]);
#endif

    qxl->ram = qxl->ram_physical = qxl->vram = qxl->rom = NULL;
}

static Bool
qxlMapMemory(qxlScreen *qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range(qxl->pci, qxl->pci->regions[0].base_addr, 
			 qxl->pci->regions[0].size,
			 PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
			 &qxl->ram);
    qxl->ram_physical = (void *)qxl->pci->regions[0].base_addr;

    pci_device_map_range(qxl->pci, qxl->pci->regions[1].base_addr, 
			 qxl->pci->regions[1].size,
			 PCI_DEV_MAP_FLAG_WRITABLE,
			 &qxl->vram);

    pci_device_map_range(qxl->pci, qxl->pci->regions[2].base_addr, 
			 qxl->pci->regions[2].size, 0,
			 &qxl->rom);

    qxl->io_base = qxl->pci->regions[3].base_addr;
#else
    qxl->ram = xf86MapPciMem(scrnIndex, VIDMEM_FRAMEBUFFER,
			     qxl->pciTag, qxl->pci->memBase[0],
			     (1 << qxl->pci->size[0]));
    qxl->ram_physical = (void *)qxl->pci->memBase[0];
    
    qxl->vram = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			      qxl->pciTag, qxl->pci->memBase[1],
			      (1 << qxl->pci->size[1]));
    
    qxl->rom = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			     qxl->pciTag, qxl->pci->memBase[2],
			     (1 << qxl->pci->size[2]));
    
    qxl->io_base = qxl->pci->ioBase[3];
#endif
    if (!qxl->ram || !qxl->vram || !qxl->rom)
	return FALSE;

    xf86DrvMsg(scrnIndex, X_INFO, "ram at %p; vram at %p; rom at %p\n",
	       qxl->ram, qxl->vram, qxl->rom);

    return TRUE;
}

static Bool
qxlCloseScreen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxlScreen *qxl = pScrn->driverPrivate;

    if (pScrn->vtSema)
	qxlUnmapMemory(qxl, scrnIndex);
    pScrn->vtSema = FALSE;

    xfree(qxl->fb);

    pScreen->CreateScreenResources = qxl->CreateScreenResources;
    pScreen->CloseScreen = qxl->CloseScreen;

    return pScreen->CloseScreen(scrnIndex, pScreen);
}

static Bool
qxlSwitchMode(int scrnIndex, DisplayModePtr p, int flags)
{
    qxlScreen *qxl = xf86Screens[scrnIndex]->driverPrivate;
    struct qxl_mode *m = (void *)p->Private;
    ScreenPtr pScreen = qxl->pScrn->pScreen;

    if (!m)
	return FALSE;

    /* if (debug) */
    xf86DrvMsg(scrnIndex, X_INFO, "Setting mode %d (%d x %d) (%d x %d) %p\n", m->id, m->x_res, m->y_res, p->HDisplay, p->VDisplay, p);

    outb(qxl->io_base + QXL_IO_RESET, 0);
    
    outb(qxl->io_base + QXL_IO_SET_MODE, m->id);

    /* If this happens out of ScreenInit, we won't have a screen yet. In that
     * case createScreenResources will make things right.
     */
    if (pScreen)
    {
	PixmapPtr pPixmap = pScreen->GetScreenPixmap(pScreen);

	if (pPixmap)
	{
	    pScreen->ModifyPixmapHeader(
		pPixmap,
		m->x_res, m->y_res,
		-1, -1,
		qxl->pScrn->displayWidth * ((qxl->pScrn->bitsPerPixel + 7) / 8),
		NULL);
	}
    }
    
    if (qxl->mem)
    {
	qxl_mem_free_all (qxl->mem);
	qxl_drop_image_cache (qxl);
    }

    
    return TRUE;
}

static void
push_drawable (qxlScreen *qxl, struct qxl_drawable *drawable)
{
    struct qxl_command cmd;

    cmd.type = QXL_CMD_DRAW;
    cmd.data = physical_address (qxl, drawable);

    qxl_ring_push (qxl->command_ring, &cmd);
}

static struct qxl_drawable *
make_drawable (qxlScreen *qxl, uint8_t type,
	       const struct qxl_rect *rect
	       /* , pRegion clip */)
{
    struct qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = qxl_allocnf (qxl, sizeof *drawable);

    CHECK_POINT();

    drawable->release_info.id = (uint64_t)drawable;

    drawable->type = type;

    drawable->effect = QXL_EFFECT_BLEND;
    drawable->bitmap_offset = 0;
    drawable->bitmap_area.top = 0;
    drawable->bitmap_area.left = 0;
    drawable->bitmap_area.bottom = 0;
    drawable->bitmap_area.right = 0;
    /* FIXME: add clipping */
    drawable->clip.type = QXL_CLIP_TYPE_NONE;

    if (rect)
	drawable->bbox = *rect;

    drawable->mm_time = qxl->rom->mm_clock;

    CHECK_POINT();
    
    return drawable;
}

enum ROPDescriptor {
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

static void
submit_fill (qxlScreen *qxl, const struct qxl_rect *rect, uint32_t color)
{
    struct qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = make_drawable (qxl, QXL_DRAW_FILL, rect);

    CHECK_POINT();

    drawable->u.fill.brush.type = QXL_BRUSH_TYPE_SOLID;
    drawable->u.fill.brush.u.color = color;
    drawable->u.fill.rop_descriptor = ROPD_OP_PUT;
    drawable->u.fill.mask.flags = 0;
    drawable->u.fill.mask.pos.x = 0;
    drawable->u.fill.mask.pos.y = 0;
    drawable->u.fill.mask.bitmap = 0;

    push_drawable (qxl, drawable);
}

static void
translate_rect (struct qxl_rect *rect)
{
    rect->right -= rect->left;
    rect->bottom -= rect->top;
    rect->left = rect->top = 0;
}

static void
submit_copy (qxlScreen *qxl, const struct qxl_rect *rect)
{
    struct qxl_drawable *drawable;
    ScrnInfoPtr pScrn = qxl->pScrn;

    if (rect->left == rect->right ||
	rect->top == rect->bottom)
    {
	/* Empty rectangle */
	return ;
    }
    
    drawable = make_drawable (qxl, QXL_DRAW_COPY, rect);

    drawable->u.copy.src_bitmap = physical_address (
	qxl, qxl_image_create (qxl, qxl->fb, rect->left, rect->top,
			       rect->right - rect->left,
			       rect->bottom - rect->top,
			       pScrn->displayWidth * 4));
    drawable->u.copy.src_area = *rect;
    translate_rect (&drawable->u.copy.src_area);
    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    push_drawable (qxl, drawable);
}

static void
print_region (const char *header, RegionPtr pRegion)
{
    int nbox = REGION_NUM_RECTS (pRegion);
    BoxPtr pbox = REGION_RECTS (pRegion);

    ErrorF ("%s \n", header);
    
    while (nbox--)
    {
	ErrorF ("   %d %d %d %d (size: %d %d)\n",
		pbox->x1, pbox->y1, pbox->x2, pbox->y2,
		pbox->x2 - pbox->x1, pbox->y2 - pbox->y1);

	pbox++;
    }
}

static void
undamage (qxlScreen *qxl)
{
    REGION_EMPTY (qxl->pScrn->pScreen, &(qxl->pendingCopy));
}

static void
qxlSendCopies (qxlScreen *qxl)
{
    BoxPtr pBox = REGION_RECTS(&qxl->pendingCopy);
    int nbox = REGION_NUM_RECTS(&qxl->pendingCopy);

#if 0
    print_region ("send bits", &qxl->pendingCopy);
#endif
    
    while (nbox--)
    {
	struct qxl_rect qrect;
	
	qrect.top = pBox->y1;
	qrect.left = pBox->x1;
	qrect.bottom = pBox->y2;
	qrect.right = pBox->x2;
	
	submit_copy (qxl, &qrect);

	pBox++;
    }

    REGION_EMPTY(qxl->pScrn->pScreen, &qxl->pendingCopy);
}

static void
paint_shadow (qxlScreen *qxl)
{
    struct qxl_rect qrect;

    qrect.top = 0;
    qrect.bottom = 1200;
    qrect.left = 0;
    qrect.right = 1600;

    submit_copy (qxl, &qrect);
}

static void
qxlBlockHandler(pointer data, OSTimePtr pTimeout, pointer pRead)
{
    qxlScreen *qxl = (qxlScreen *) data;

    qxlSendCopies (qxl);
}

static void
qxlWakeupHandler(pointer data, int i, pointer LastSelectMask)
{
}

static Bool
qxlCreateScreenResources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;
    Bool ret;
    PixmapPtr pPixmap;

    pScreen->CreateScreenResources = qxl->CreateScreenResources;
    ret = pScreen->CreateScreenResources(pScreen);
    pScreen->CreateScreenResources = qxlCreateScreenResources;

    if (!ret)
	return FALSE;

    pPixmap = pScreen->GetScreenPixmap(pScreen);

    if (!RegisterBlockAndWakeupHandlers(qxlBlockHandler, qxlWakeupHandler, qxl))
	return FALSE;
 
    DamageRegister (&pPixmap->drawable, qxl->pDamage);
    return TRUE;
}

static PixmapPtr 
getWindowPixmap (DrawablePtr pDrawable, int *xoff, int *yoff)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    PixmapPtr result;

    if (pDrawable->type != DRAWABLE_WINDOW)
	return NULL;

    result = pScreen->GetWindowPixmap ((WindowPtr)pDrawable);

    *xoff = pDrawable->x;
    *yoff = pDrawable->y;

    return result;
}

static void
qxlPolyFillRect (DrawablePtr pDrawable,
		 GCPtr	     pGC,
		 int	     nrect,
		 xRectangle *prect)
{
    ScrnInfoPtr pScrn = xf86Screens[pDrawable->pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap;
    int xoff, yoff;

    if ((pPixmap = getWindowPixmap (pDrawable, &xoff, &yoff))	&&
	pGC->fillStyle == FillSolid				&&
	pGC->alu == GXcopy					&&
	(unsigned int)pGC->planemask == FB_ALLONES)
    {
	RegionPtr pReg = RECTS_TO_REGION (pScreen, nrect, prect, CT_UNSORTED);
	RegionPtr pClip = fbGetCompositeClip (pGC);
	BoxPtr pBox;
	int nbox;

	REGION_TRANSLATE(pScreen, pReg, xoff, yoff);
	REGION_INTERSECT(pScreen, pReg, pClip, pReg);

	pBox = REGION_RECTS (pReg);
	nbox = REGION_NUM_RECTS (pReg);

	while (nbox--)
	{
	    struct qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (qxl, &qrect, pGC->fgPixel);

	    pBox++;
	}

	REGION_DESTROY (pScreen, pReg);
	
	/* Clear pending damage */
	undamage (qxl);
    }
    
    fbPolyFillRect (pDrawable, pGC, nrect, prect);
}

static void
qxlCopyNtoN (DrawablePtr    pSrcDrawable,
	     DrawablePtr    pDstDrawable,
	     GCPtr	    pGC,
	     BoxPtr	    pbox,
	     int	    nbox,
	     int	    dx,
	     int	    dy,
	     Bool	    reverse,
	     Bool	    upsidedown,
	     Pixel	    bitplane,
	     void	    *closure)
{
    ScreenPtr pScreen = pSrcDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;
    int src_xoff, src_yoff;
    int dst_xoff, dst_yoff;
    PixmapPtr pSrcPixmap, pDstPixmap;

    if ((pSrcPixmap = getWindowPixmap (pSrcDrawable, &src_xoff, &src_yoff)) &&
	(pDstPixmap = getWindowPixmap (pDstDrawable, &dst_xoff, &dst_yoff)))
    {
	int n = nbox;
	BoxPtr b = pbox;
	
	assert (pSrcPixmap == pDstPixmap);

	while (n--)
	{
	    struct qxl_drawable *drawable;
	    struct qxl_rect qrect;
	    
	    qrect.top = b->y1;
	    qrect.bottom = b->y2;
	    qrect.left = b->x1;
	    qrect.right = b->x2;

#if 0
	    ErrorF ("   Translate %d %d %d %d by %d %d (offsets %d %d)\n",
		    b->x1, b->y1, b->x2, b->y2,
		    dx, dy, dst_xoff, dst_yoff);
#endif
	    
	    drawable = make_drawable (qxl, QXL_COPY_BITS, &qrect);
	    drawable->u.copy_bits.src_pos.x = b->x1 + dx;
	    drawable->u.copy_bits.src_pos.y = b->y1 + dy;

	    push_drawable (qxl, drawable);

#if 0
	    if (closure)
		qxl_usleep (1000000);
#endif
	    
#if 0
	    submit_fill (qxl, &qrect, rand());
#endif

	    b++;
	}
    }

    fbCopyNtoN (pSrcDrawable, pDstDrawable, pGC, pbox, nbox, dx, dy, reverse, upsidedown, bitplane, closure);
}

static RegionPtr
qxlCopyArea(DrawablePtr pSrcDrawable, DrawablePtr pDstDrawable, GCPtr pGC,
	    int srcx, int srcy, int width, int height, int dstx, int dsty)
{
    ScreenPtr pScreen = pSrcDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;

    if (pSrcDrawable->type == DRAWABLE_WINDOW &&
	pDstDrawable->type == DRAWABLE_WINDOW)
    {
	undamage (qxl);

	return fbDoCopy (pSrcDrawable, pDstDrawable, pGC,
			 srcx, srcy, width, height, dstx, dsty,
			 qxlCopyNtoN, 0, NULL);
    }
    else
    {
	return fbCopyArea (pSrcDrawable, pDstDrawable, pGC,
			   srcx, srcy, width, height, dstx, dsty);
    }
}

static void
qxlFillRegionSolid (DrawablePtr pDrawable, RegionPtr pRegion, Pixel pixel)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap;
    int xoff, yoff;

    if ((pPixmap = getWindowPixmap (pDrawable, &xoff, &yoff)))
    {
	int nbox = REGION_NUM_RECTS (pRegion);
	BoxPtr pBox = REGION_RECTS (pRegion);

	while (nbox--)
	{
	    struct qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (qxl, &qrect, pixel);

	    pBox++;
	}
    }

    fbFillRegionSolid (pDrawable, pRegion, 0,
		       fbReplicatePixel (pixel, pDrawable->bitsPerPixel));
}

static void
qxlPaintWindow(WindowPtr pWin, RegionPtr pRegion, int what)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;

    if (!REGION_NUM_RECTS(pRegion))
	return;

    if (what == PW_BACKGROUND &&
	pWin->backgroundState == BackgroundPixel)
    {
	uint32_t pixel = pWin->background.pixel;

	qxlFillRegionSolid (&pWin->drawable, pRegion, pixel);
	
	undamage (qxl);
    }

    fbPaintWindow (pWin, pRegion, what);
}

static void
qxlCopyWindow (WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
    ScreenPtr pScreen = pWin->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxlScreen *qxl = pScrn->driverPrivate;
    RegionRec rgnDst;
    int dx, dy;

    dx = ptOldOrg.x - pWin->drawable.x;
    dy = ptOldOrg.y - pWin->drawable.y;

    REGION_TRANSLATE (pScreen, prgnSrc, -dx, -dy);

    REGION_INIT (pScreen, &rgnDst, NullBox, 0);

    REGION_INTERSECT(pScreen, &rgnDst, &pWin->borderClip, prgnSrc);

    undamage (qxl);
    
    fbCopyRegion (&pWin->drawable, &pWin->drawable,
		  NULL, &rgnDst, dx, dy, qxlCopyNtoN, 0, NULL);
#if 0

    REGION_TRANSLATE (pScreen, prgnSrc, dx, dy);
    
    fbCopyWindow (pWin, ptOldOrg, prgnSrc);
#endif
}

static int
qxlCreateGC (GCPtr pGC)
{
    static GCOps ops;
    static int initialized;
    
    if (!fbCreateGC (pGC))
	return FALSE;

    if (!initialized)
    {
	ops = *pGC->ops;
	ops.PolyFillRect = qxlPolyFillRect;
	ops.CopyArea = qxlCopyArea;

	initialized = TRUE;
    }
    
    pGC->ops = &ops;
    return TRUE;
}

static void
qxlOnDamage (DamagePtr pDamage, RegionPtr pRegion, pointer closure)
{
    qxlScreen *qxl = closure;

#if 0
    ErrorF ("damage\n");
#endif
    
    qxlSendCopies (qxl);

    REGION_COPY (qxl->pScrn->pScreen, &(qxl->pendingCopy), pRegion);
}

static Bool
qxlScreenInit(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxlScreen *qxl = pScrn->driverPrivate;
    struct qxl_rom *rom;
    struct qxl_ram_header *ram_header;

    CHECK_POINT();

    qxl->pScrn = pScrn;
    
    if (!qxlMapMemory(qxl, scrnIndex))
	return FALSE;

    rom = qxl->rom;
    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);
    
    qxlSaveState(qxl);
    qxlBlankScreen(pScreen, SCREEN_SAVER_ON);
    
    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	goto out;
    if (!miSetPixmapDepths())
	goto out;

    qxl->fb = xcalloc(pScrn->virtualX * pScrn->displayWidth,
		      (pScrn->bitsPerPixel + 7)/8);
    if (!qxl->fb)
	goto out;

    pScrn->virtualX = pScrn->currentMode->HDisplay;
    pScrn->virtualY = pScrn->currentMode->VDisplay;
    
    if (!fbScreenInit(pScreen, qxl->fb,
		      pScrn->currentMode->HDisplay,
		      pScrn->currentMode->VDisplay,
		      pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
		      pScrn->bitsPerPixel))
	goto out;
    {
	VisualPtr visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
	    if ((visual->class | DynamicClass) == DirectColor) {
                visual->offsetRed = pScrn->offset.red;
                visual->offsetGreen = pScrn->offset.green;
                visual->offsetBlue = pScrn->offset.blue;
                visual->redMask = pScrn->mask.red;
                visual->greenMask = pScrn->mask.green;
                visual->blueMask = pScrn->mask.blue;
	    }
	}
    }

    
    fbPictureInit(pScreen, 0, 0);

    qxl->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = qxlCreateScreenResources;

    /* Set up resources */
    qxl->mem = qxl_mem_create ((void *)((unsigned long)qxl->ram + (unsigned long)rom->pages_offset),
			       rom->num_io_pages * getpagesize());
    qxl->io_pages = (void *)((unsigned long)qxl->ram + (unsigned long)rom->pages_offset);
    qxl->io_pages_physical = (void *)((unsigned long)qxl->ram_physical + (unsigned long)rom->pages_offset);

    qxl->command_ring = qxl_ring_create (&(ram_header->cmd_ring_hdr),
					 sizeof (struct qxl_command),
					 32, qxl->io_base + QXL_IO_NOTIFY_CMD);
    qxl->cursor_ring = qxl_ring_create (&(ram_header->cursor_ring_hdr),
					sizeof (struct qxl_command),
					32, qxl->io_base + QXL_IO_NOTIFY_CURSOR);
    qxl->release_ring = qxl_ring_create (&(ram_header->release_ring_hdr),
					 sizeof (uint64_t),
					 8, 0);
					 
    /* xf86DPMSInit(pScreen, xf86DPMSSet, 0); */

#if 0 /* XV accel */
    qxlInitVideo(pScreen);
#endif

    pScreen->SaveScreen = qxlBlankScreen;
    qxl->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = qxlCloseScreen;

    qxl->CreateGC = pScreen->CreateGC;
    pScreen->CreateGC = qxlCreateGC;

    qxl->PaintWindowBackground = pScreen->PaintWindowBackground;
    qxl->PaintWindowBorder = pScreen->PaintWindowBorder;
    qxl->CopyWindow = pScreen->CopyWindow;
    pScreen->PaintWindowBackground = qxlPaintWindow;
    pScreen->PaintWindowBorder = qxlPaintWindow;
    pScreen->CopyWindow = qxlCopyWindow;
    
    qxl->pDamage = DamageCreate(qxlOnDamage, NULL,
				DamageReportRawRegion,
				TRUE, pScreen, qxl);

    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!miCreateDefColormap(pScreen))
	goto out;

    qxlCursorInit (pScreen);
    
    CHECK_POINT();

    qxlSwitchMode(scrnIndex, pScrn->currentMode, 0);

    CHECK_POINT();
    
    return TRUE;

out:
    return FALSE;
}

static Bool
qxlEnterVT(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxlScreen *qxl = pScrn->driverPrivate;

    qxlSaveState(qxl);

    qxlSwitchMode(scrnIndex, pScrn->currentMode, 0);
    return TRUE;
}

static void
qxlLeaveVT(int scrnIndex, int flags)
{
    qxlScreen *qxl = xf86Screens[scrnIndex]->driverPrivate;

    qxlRestoreState(qxl);
}

static Bool
qxlColorSetup(ScrnInfoPtr pScrn)
{
    int scrnIndex = pScrn->scrnIndex;
    Gamma gzeros = { 0.0, 0.0, 0.0 };
    rgb rzeros = { 0, 0, 0 };

    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb))
	return FALSE;
    if (pScrn->depth != 16 && pScrn->depth != 24) {
	xf86DrvMsg(scrnIndex, X_ERROR, "Depth %d is not supported\n",
		   pScrn->depth);
	return FALSE;
    }
    xf86PrintDepthBpp(pScrn);

    if (!xf86SetWeight(pScrn, rzeros, rzeros))
	return FALSE;
    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;
    if (!xf86SetGamma(pScrn, gzeros))
	return FALSE;

    return TRUE;
}

static void
qxlPrintMode(int scrnIndex, void *p)
{
    struct qxl_mode *m = p;
    xf86DrvMsg(scrnIndex, X_INFO,
	       "%d: %dx%d, %d bits, stride %d, %dmm x %dmm, orientation %d\n",
	       m->id, m->x_res, m->y_res, m->bits, m->stride, m->x_mili,
	       m->y_mili, m->orientation);
}

static Bool
qxlCheckDevice(ScrnInfoPtr pScrn, qxlScreen *qxl)
{
    int scrnIndex = pScrn->scrnIndex;
    int i, mode_offset;
    struct qxl_rom *rom = qxl->rom;
    struct qxl_ram_header *ram_header = (void *)((unsigned long)qxl->ram + rom->ram_header_offset);
    
    CHECK_POINT();
    
    if (rom->magic != 0x4f525851) { /* "QXRO" little-endian */
	xf86DrvMsg(scrnIndex, X_ERROR, "Bad ROM signature %x\n", rom->magic);
	return FALSE;
    }

    xf86DrvMsg(scrnIndex, X_INFO, "Device version %d.%d\n",
	       rom->id, rom->update_id);

    xf86DrvMsg(scrnIndex, X_INFO, "Compression level %d, log level %d\n",
	       rom->compression_level,
	       rom->log_level);

    xf86DrvMsg(scrnIndex, X_INFO, "Currently using mode #%d, list at 0x%x\n",
	       rom->mode, rom->modes_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "%d io pages at 0x%x\n",
	       rom->num_io_pages, rom->pages_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "%d byte draw area at 0x%x\n",
	       qxl->draw_area_size, qxl->draw_area_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "RAM header offset: 0x%x\n", rom->ram_header_offset);

    if (ram_header->magic != 0x41525851) { /* "QXRA" little-endian */
	xf86DrvMsg(scrnIndex, X_ERROR, "Bad RAM signature %x at %p\n",
		   ram_header->magic,
		   &ram_header->magic);
	return FALSE;
    }

    xf86DrvMsg(scrnIndex, X_INFO, "Correct RAM signature %x\n", 
	       ram_header->magic);

    qxl->draw_area_offset = rom->draw_area_offset;
    qxl->draw_area_size = rom->draw_area_size;
    pScrn->videoRam = rom->draw_area_size / 1024;
    
    mode_offset = rom->modes_offset / 4;
    qxl->num_modes = ((uint32_t *)rom)[mode_offset];
    xf86DrvMsg(scrnIndex, X_INFO, "%d available modes:\n", qxl->num_modes);
    qxl->modes = (void *)((uint32_t *)rom + mode_offset + 1);
    for (i = 0; i < qxl->num_modes; i++)
	qxlPrintMode(scrnIndex, qxl->modes + i);

    return TRUE;
}

static struct qxl_mode *
qxlFindNativeMode(ScrnInfoPtr pScrn, DisplayModePtr p)
{
    int i;
    qxlScreen *qxl = pScrn->driverPrivate;

    CHECK_POINT();
    
    for (i = 0; i < qxl->num_modes; i++) {
	struct qxl_mode *m = qxl->modes + i;
	if (m->x_res == p->HDisplay &&
	    m->y_res == p->VDisplay &&
	    m->bits == pScrn->bitsPerPixel)
	{
	    return m;
	}
    }

    return NULL;	
}

static ModeStatus
qxlValidMode(int scrn, DisplayModePtr p, Bool flag, int pass)
{
    ScrnInfoPtr pScrn = xf86Screens[scrn];
    qxlScreen *qxl = pScrn->driverPrivate;
    int bpp = pScrn->bitsPerPixel;

    /* FIXME: I don't think this is necessary now that we report the
     * correct amount of video ram?
     */
    if (p->HDisplay * p->VDisplay * (bpp/8) > qxl->draw_area_size)
	return MODE_MEM;

    p->Private = (void *)qxlFindNativeMode(pScrn, p);
    if (!p->Private)
       return MODE_NOMODE;

    assert (((struct qxl_mode *)p->Private)->x_res == p->HDisplay);
    assert (((struct qxl_mode *)p->Private)->y_res == p->VDisplay);
    
    return MODE_OK;
}

static Bool
qxlPreInit(ScrnInfoPtr pScrn, int flags)
{
    int scrnIndex = pScrn->scrnIndex;
    qxlScreen *qxl = NULL;
    ClockRangePtr clockRanges = NULL;
    int *linePitches = NULL;

    CHECK_POINT();
    
    /* zaphod mode is for suckers and i choose not to implement it */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	xf86DrvMsg(scrnIndex, X_ERROR, "No Zaphod mode for you\n");
	return FALSE;
    }

    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc(sizeof(qxlScreen), 1);
    qxl = pScrn->driverPrivate;
    
    qxl->entity = xf86GetEntityInfo(pScrn->entityList[0]);
    qxl->pci = xf86GetPciInfoForEntity(qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    qxl->pciTag = pciTag(qxl->pci->bus, qxl->pci->device, qxl->pci->func);
#endif

    pScrn->monitor = pScrn->confScreen->monitor;

    if (!qxlColorSetup(pScrn))
	goto out;

    /* option parsing and card differentiation */
    xf86CollectOptions(pScrn, NULL);
    
    if (!qxlMapMemory(qxl, scrnIndex))
	goto out;

    if (!qxlCheckDevice(pScrn, qxl))
	goto out;

    /* ddc stuff here */

    clockRanges = xnfcalloc(sizeof(ClockRange), 1);
    clockRanges->next = NULL;
    clockRanges->minClock = 10000;
    clockRanges->maxClock = 165000;
    clockRanges->clockIndex = -1;
    clockRanges->interlaceAllowed = clockRanges->doubleScanAllowed = 0;
    clockRanges->ClockMulFactor = clockRanges->ClockDivFactor = 1;
    pScrn->progClock = TRUE;

    if (0 >= xf86ValidateModes(pScrn, pScrn->monitor->Modes,
			       pScrn->display->modes, clockRanges, linePitches,
			       128, 2048, 128 * 4, 128, 2048,
			       pScrn->display->virtualX,
			       pScrn->display->virtualY,
			       128 * 1024 * 1024, LOOKUP_BEST_REFRESH))
	goto out;

    CHECK_POINT();
    
    xf86PruneDriverModes(pScrn);
    pScrn->currentMode = pScrn->modes;
    xf86PrintModes(pScrn);
    xf86SetDpi(pScrn, 0, 0);

    if (!xf86LoadSubModule(pScrn, "fb") ||
	!xf86LoadSubModule(pScrn, "exa") ||
	!xf86LoadSubModule(pScrn, "ramdac"))
    {
	goto out;
    }

    /* hate */
    qxlUnmapMemory(qxl, scrnIndex);

    CHECK_POINT();
    
    xf86DrvMsg(scrnIndex, X_INFO, "PreInit complete\n");
    return TRUE;

out:
    if (clockRanges)
	xfree(clockRanges);
    if (qxl)
	xfree(qxl);

    return FALSE;
}

#ifdef XSERVER_LIBPCIACCESS
enum qxl_class
{
    CHIP_QXL_1,
};

static const struct pci_id_match qxl_device_match[] = {
    {
	PCI_VENDOR_RED_HAT, PCI_CHIP_QXL_0100, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00030000, 0x00ffffff, CHIP_QXL_1
    },

    { 0 },
};
#endif

static SymTabRec qxlChips[] =
{
    { PCI_CHIP_QXL_0100,	"QXL 1", },
    { -1, NULL }
};

#ifndef XSERVER_LIBPCIACCESS
static PciChipsets qxlPciChips[] =
{
    { PCI_CHIP_QXL_0100,    PCI_CHIP_QXL_0100,	RES_SHARED_VGA },
    { -1, -1, RES_UNDEFINED }
};
#endif

static void
qxlIdentify(int flags)
{
    xf86PrintChipsets("qxl", "Driver for QXL virtual graphics", qxlChips);
}

static void
qxlInitScrn(ScrnInfoPtr pScrn)
{
    pScrn->driverVersion    = 0;
    pScrn->driverName	    = pScrn->name = "qxl";
    pScrn->PreInit	    = qxlPreInit;
    pScrn->ScreenInit	    = qxlScreenInit;
    pScrn->SwitchMode	    = qxlSwitchMode;
    pScrn->ValidMode	    = qxlValidMode;
    pScrn->EnterVT	    = qxlEnterVT;
    pScrn->LeaveVT	    = qxlLeaveVT;
}

#ifndef XSERVER_LIBPCIACCESS
static Bool
qxlProbe(DriverPtr drv, int flags)
{
    int i, numUsed;
    int numDevSections;
    int *usedChips;
    GDevPtr *devSections;

    if ((numDevSections = xf86MatchDevice(QXL_NAME, &devSections)) <= 0)
	return FALSE;

    if (!xf86GetPciVideoInfo())
	return FALSE;

    numUsed = xf86MatchPciInstances(QXL_NAME, PCI_VENDOR_RED_HAT,
				    qxlChips, qxlPciChips,
				    devSections, numDevSections,
				    drv, &usedChips);

    xfree(devSections);

    if (numUsed < 0) {
	xfree(usedChips);
	return FALSE;
    }

    if (flags & PROBE_DETECT) {
	xfree(usedChips);
	return TRUE;
    }

    for (i = 0; i < numUsed; i++) {
	ScrnInfoPtr pScrn = NULL;
	if ((pScrn = xf86ConfigPciEntity(pScrn, 0, usedChips[i], qxlPciChips,
					 0, 0, 0, 0, 0)))
	    qxlInitScrn(pScrn);
    }

    xfree(usedChips);
    return TRUE;
}

#else /* pciaccess */

static Bool
qxlPciProbe(DriverPtr drv, int entity, struct pci_device *dev, intptr_t match)
{
    qxlScreen *qxl;
    ScrnInfoPtr pScrn = xf86ConfigPciEntity(NULL, 0, entity, NULL, NULL,
					    NULL, NULL, NULL, NULL);

    if (!pScrn)
	return FALSE;

    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc(sizeof(qxlScreen), 1);
    qxl = pScrn->driverPrivate;
    qxl->pci = dev;

    qxlInitScrn(pScrn);

    return TRUE;
}

#define qxlProbe NULL

#endif

static DriverRec qxl_driver = {
    0,
    "qxl",
    qxlIdentify,
    qxlProbe,
    NULL,
    NULL,
    0,
    NULL,
#ifdef XSERVER_LIBPCIACCESS
    qxl_device_match,
    qxlPciProbe
#endif
};

static pointer
qxlSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool loaded = FALSE;

    if (!loaded) {
	loaded = TRUE;
	xf86AddDriver(&qxl_driver, module, HaveDriverFuncs);
	return (void *)1;
    } else {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	return NULL;
    }
}

static XF86ModuleVersionInfo qxlModuleInfo = {
    "qxl",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    0, 0, 0,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    { 0, 0, 0, 0 }
};

_X_EXPORT XF86ModuleData qxlModuleData = {
    &qxlModuleInfo,
    qxlSetup,
    NULL
};
