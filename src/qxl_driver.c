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
 * \author Søren Sandmann <sandmann@redhat.com>
 *
 * This is qxl, a driver for the Qumranet paravirtualized graphics device
 * in qemu.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include "qxl.h"
#include "assert.h"
#include "qxl_option_helpers.h"

#ifdef XSPICE
#include "spiceqxl_driver.h"
#include "spiceqxl_main_loop.h"
#include "spiceqxl_display.h"
#include "spiceqxl_inputs.h"
#include "spiceqxl_io_port.h"
#include "spiceqxl_spice_server.h"
#endif /* XSPICE */

extern void compat_init_scrn(ScrnInfoPtr);

#if 0
#define CHECK_POINT() ErrorF ("%s: %d  (%s)\n", __FILE__, __LINE__, __FUNCTION__);
#endif
#define CHECK_POINT()

const OptionInfoRec DefaultOptions[] = {
    { OPTION_ENABLE_IMAGE_CACHE,
        "EnableImageCache",    OPTV_BOOLEAN, { 0 }, TRUE },
    { OPTION_ENABLE_FALLBACK_CACHE,
        "EnableFallbackCache", OPTV_BOOLEAN, { 0 }, TRUE },
    { OPTION_ENABLE_SURFACES,
        "EnableSurfaces",	   OPTV_BOOLEAN, { 0 }, TRUE },
#ifdef XSPICE
    { OPTION_SPICE_PORT,
        "SpicePort",                OPTV_INTEGER,   {5900}, FALSE },
    { OPTION_SPICE_TLS_PORT,
        "SpiceTlsPort",             OPTV_INTEGER,   {0}, FALSE},
    { OPTION_SPICE_ADDR,
        "SpiceAddr",                OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_X509_DIR,
        "SpiceX509Dir",             OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_SASL,
        "SpiceSasl",                OPTV_BOOLEAN,   {0}, FALSE},
    /* VVV qemu defaults to 1 - not implemented in xspice yet */
    { OPTION_SPICE_AGENT_MOUSE,
        "SpiceAgentMouse",          OPTV_BOOLEAN,   {0}, FALSE},
    { OPTION_SPICE_DISABLE_TICKETING,
        "SpiceDisableTicketing",    OPTV_BOOLEAN,   {0}, FALSE},
    { OPTION_SPICE_PASSWORD,
        "SpicePassword",            OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_X509_KEY_FILE,
        "SpiceX509KeyFile",         OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_STREAMING_VIDEO,
        "SpiceStreamingVideo",      OPTV_STRING,    {.str="filter"}, FALSE},
    { OPTION_SPICE_PLAYBACK_COMPRESSION,
        "SpicePlaybackCompression", OPTV_BOOLEAN,   {1}, FALSE},
    { OPTION_SPICE_ZLIB_GLZ_WAN_COMPRESSION,
        "SpiceZlibGlzWanCompression", OPTV_STRING,  {.str="auto"}, FALSE},
    { OPTION_SPICE_JPEG_WAN_COMPRESSION,
        "SpiceJpegWanCompression",  OPTV_STRING,    {.str="auto"}, FALSE},
    { OPTION_SPICE_IMAGE_COMPRESSION,
        "SpiceImageCompression",    OPTV_STRING,    {.str="auto_glz"}, FALSE},
    { OPTION_SPICE_DISABLE_COPY_PASTE,
        "SpiceDisableCopyPaste",    OPTV_BOOLEAN,   {0}, FALSE},
    { OPTION_SPICE_IPV4_ONLY,
        "SpiceIPV4Only",            OPTV_BOOLEAN,   {0}, FALSE},
    { OPTION_SPICE_IPV6_ONLY,
        "SpiceIPV6Only",            OPTV_BOOLEAN,   {0}, FALSE},
    { OPTION_SPICE_X509_CERT_FILE,
        "SpiceX509CertFile",        OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_X509_KEY_PASSWORD,
        "SpiceX509KeyPassword",     OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_TLS_CIPHERS,
        "SpiceTlsCiphers",          OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_CACERT_FILE,
        "SpiceCacertFile",          OPTV_STRING,    {0}, FALSE},
    { OPTION_SPICE_DH_FILE,
        "SpiceDhFile",              OPTV_STRING,    {0}, FALSE},
#endif

    { -1, NULL, OPTV_NONE, {0}, FALSE }
};

static const OptionInfoRec *
qxl_available_options (int chipid, int busid)
{
    return DefaultOptions;
}

static void qxl_wait_for_io_command(qxl_screen_t *qxl)
{
    struct QXLRam *ram_header = (void *)(
        (unsigned long)qxl->ram + qxl->rom->ram_header_offset);

    while (!(ram_header->int_pending & QXL_INTERRUPT_IO_CMD)) {
        usleep(1);
    }
    ram_header->int_pending &= ~QXL_INTERRUPT_IO_CMD;
}

void qxl_update_area(qxl_screen_t *qxl)
{
#ifndef XSPICE
    if (qxl->pci->revision >= 3) {
        ioport_write(qxl, QXL_IO_UPDATE_AREA_ASYNC, 0);
        qxl_wait_for_io_command(qxl);
    } else {
        ioport_write(qxl, QXL_IO_UPDATE_AREA, 0);
    }
#else
    ioport_write(qxl, QXL_IO_UPDATE_AREA, 0);
#endif
}

void qxl_memslot_add(qxl_screen_t *qxl, uint8_t id)
{
#ifndef XSPICE
    if (qxl->pci->revision >= 3) {
        ioport_write(qxl, QXL_IO_MEMSLOT_ADD_ASYNC, id);
        qxl_wait_for_io_command(qxl);
    } else {
        ioport_write(qxl, QXL_IO_MEMSLOT_ADD, id);
    }
#else
    ioport_write(qxl, QXL_IO_MEMSLOT_ADD, id);
#endif
}

void qxl_create_primary(qxl_screen_t *qxl)
{
#ifndef XSPICE
    if (qxl->pci->revision >= 3) {
        ioport_write(qxl, QXL_IO_CREATE_PRIMARY_ASYNC, 0);
        qxl_wait_for_io_command(qxl);
    } else {
        ioport_write(qxl, QXL_IO_CREATE_PRIMARY, 0);
    }
#else
    ioport_write(qxl, QXL_IO_CREATE_PRIMARY, 0);
#endif
}

void qxl_notify_oom(qxl_screen_t *qxl)
{
    ioport_write(qxl, QXL_IO_NOTIFY_OOM, 0);
}

int
qxl_garbage_collect (qxl_screen_t *qxl)
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
		
		qxl_free (qxl->mem, cursor);
	    }
	    else if (is_drawable && drawable->type == QXL_DRAW_COPY)
	    {
		struct QXLImage *image = virtual_address (
		    qxl, u64_to_pointer (drawable->u.copy.src_bitmap), qxl->main_mem_slot);
		
		if (image->descriptor.type == SPICE_IMAGE_TYPE_SURFACE)
		{
		    qxl_surface_unref (qxl->surface_cache, image->surface_image.surface_id);
		    qxl_surface_cache_sanity_check (qxl->surface_cache);
		    qxl_free (qxl->mem, image);
		}
		else
		{
		    qxl_image_destroy (qxl, image);
		}
	    }
	    else if (is_surface && surface_cmd->type == QXL_SURFACE_CMD_DESTROY)
	    {
		qxl_surface_recycle (qxl->surface_cache, surface_cmd->surface_id);
		qxl_surface_cache_sanity_check (qxl->surface_cache);
	    }
	    
	    id = info->next;
	    
	    qxl_free (qxl->mem, info);

	    ++i;
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
    qxl_notify_oom(qxl);

#if 0
    ErrorF (".");
    qxl_usleep (10000);
#endif

    if (!(qxl_garbage_collect (qxl)))
	qxl_usleep (10000);

    return qxl_garbage_collect (qxl);
}

void *
qxl_allocnf (qxl_screen_t *qxl, unsigned long size)
{
    void *result;
    int n_attempts = 0;
#if 0
    static int nth_oom = 1;
#endif

    qxl_garbage_collect (qxl);
    
    while (!(result = qxl_alloc (qxl->mem, size)))
    {
	struct QXLRam *ram_header = (void *)(
	    (unsigned long)qxl->ram + qxl->rom->ram_header_offset);
    
	/* Rather than go out of memory, we simply tell the
	 * device to dump everything
	 */
	ram_header->update_area.top = 0;
	ram_header->update_area.bottom = qxl->virtual_y;
	ram_header->update_area.left = 0;
	ram_header->update_area.right = qxl->virtual_x;
	ram_header->update_surface = 0;		/* Only primary for now */
	
        qxl_update_area(qxl);
	
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

static Bool
qxl_blank_screen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

#ifdef XSPICE
static void
unmap_memory_helper(qxl_screen_t *qxl, int scrnIndex)
{
    free(qxl->ram);
    free(qxl->vram);
    free(qxl->rom);
}

static void
map_memory_helper(qxl_screen_t *qxl, int scrnIndex)
{
    qxl->ram = malloc(RAM_SIZE);
    qxl->ram_physical = qxl->ram;
    qxl->vram = malloc(VRAM_SIZE);
    qxl->vram_size = VRAM_SIZE;
    qxl->vram_physical = qxl->vram;
    qxl->rom = malloc(ROM_SIZE);

    init_qxl_rom(qxl, ROM_SIZE);
}
#else /* Default */
static void
unmap_memory_helper(qxl_screen_t *qxl, int scrnIndex)
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
	xf86UnMapVidMem(scrnIndex, qxl->ram, (1 << qxl->pci->size[0]));
    if (qxl->vram)
	xf86UnMapVidMem(scrnIndex, qxl->vram, (1 << qxl->pci->size[1]));
    if (qxl->rom)
	xf86UnMapVidMem(scrnIndex, qxl->rom, (1 << qxl->pci->size[2]));
#endif
}

static void
map_memory_helper(qxl_screen_t *qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range(qxl->pci, qxl->pci->regions[0].base_addr,
			 qxl->pci->regions[0].size,
			 PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
			 &qxl->ram);
    qxl->ram_physical = u64_to_pointer (qxl->pci->regions[0].base_addr);

    pci_device_map_range(qxl->pci, qxl->pci->regions[1].base_addr,
			 qxl->pci->regions[1].size,
			 PCI_DEV_MAP_FLAG_WRITABLE,
			 &qxl->vram);
    qxl->vram_physical = u64_to_pointer (qxl->pci->regions[1].base_addr);
    qxl->vram_size = qxl->pci->regions[1].size;

    pci_device_map_range(qxl->pci, qxl->pci->regions[2].base_addr,
			 qxl->pci->regions[2].size, 0,
			 (void **)&qxl->rom);

    qxl->io_base = qxl->pci->regions[3].base_addr;
#else
    qxl->ram = xf86MapPciMem(scrnIndex, VIDMEM_FRAMEBUFFER,
			     qxl->pci_tag, qxl->pci->memBase[0],
			     (1 << qxl->pci->size[0]));
    qxl->ram_physical = (void *)qxl->pci->memBase[0];

    qxl->vram = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			      qxl->pci_tag, qxl->pci->memBase[1],
			      (1 << qxl->pci->size[1]));
    qxl->vram_physical = (void *)qxl->pci->memBase[1];
    qxl->vram_size = (1 << qxl->pci->size[1]);

    qxl->rom = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			     qxl->pci_tag, qxl->pci->memBase[2],
			     (1 << qxl->pci->size[2]));

    qxl->io_base = qxl->pci->ioBase[3];
#endif
}
#endif /* XSPICE */

static void
qxl_unmap_memory(qxl_screen_t *qxl, int scrnIndex)
{
#ifdef XSPICE
    if (qxl->worker) {
        qxl->worker->stop(qxl->worker);
        qxl->worker_running = FALSE;
    }
#endif
    unmap_memory_helper(qxl, scrnIndex);
    qxl->ram = qxl->ram_physical = qxl->vram = qxl->rom = NULL;

    qxl->num_modes = 0;
    qxl->modes = NULL;
}

static Bool
qxl_map_memory(qxl_screen_t *qxl, int scrnIndex)
{
    map_memory_helper(qxl, scrnIndex);

    if (!qxl->ram || !qxl->vram || !qxl->rom)
	return FALSE;

    xf86DrvMsg(scrnIndex, X_INFO, "framebuffer at %p (%d KB)\n",
	       qxl->ram, qxl->rom->surface0_area_size / 1024);

    xf86DrvMsg(scrnIndex, X_INFO, "command ram at %p (%d KB)\n",
	       (void *)((unsigned long)qxl->ram + qxl->rom->surface0_area_size),
	       (qxl->rom->num_pages * getpagesize() - qxl->rom->surface0_area_size)/1024);

    xf86DrvMsg(scrnIndex, X_INFO, "vram at %p (%ld KB)\n",
	       qxl->vram, qxl->vram_size / 1024);

    xf86DrvMsg(scrnIndex, X_INFO, "rom at %p\n", qxl->rom);

    qxl->num_modes = *(uint32_t *)((uint8_t *)qxl->rom + qxl->rom->modes_offset);
    qxl->modes = (struct QXLMode *)(((uint8_t *)qxl->rom) + qxl->rom->modes_offset + 4);
    qxl->surface0_area = qxl->ram;
    qxl->surface0_size = qxl->rom->surface0_area_size;

    qxl->mem = qxl_mem_create ((void *)((unsigned long)qxl->ram + qxl->surface0_size),
			       qxl->rom->num_pages * getpagesize() - qxl->surface0_size);
    qxl->surf_mem = qxl_mem_create ((void *)((unsigned long)qxl->vram), qxl->vram_size);

    return TRUE;
}

#ifdef XSPICE
static void
qxl_save_state(ScrnInfoPtr pScrn)
{
}

static void
qxl_restore_state(ScrnInfoPtr pScrn)
{
}
#else /* QXL */
static void
qxl_save_state(ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;

    if (xf86IsPrimaryPci (qxl->pci))
        vgaHWSaveFonts(pScrn, &qxl->vgaRegs);
}

static void
qxl_restore_state(ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;

    if (xf86IsPrimaryPci (qxl->pci))
        vgaHWRestoreFonts(pScrn, &qxl->vgaRegs);
}
#endif /* XSPICE */

static uint8_t
setup_slot(qxl_screen_t *qxl, uint8_t slot_index_offset,
    unsigned long start_phys_addr, unsigned long end_phys_addr,
    uint64_t start_virt_addr, uint64_t end_virt_addr)
{
    uint64_t high_bits;
    qxl_memslot_t *slot;
    uint8_t slot_index;
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

    qxl_memslot_add(qxl, slot_index);

    slot->generation = qxl->rom->slot_generation;
    
    high_bits = slot_index << qxl->slot_gen_bits;
    high_bits |= slot->generation;
    high_bits <<= (64 - (qxl->slot_gen_bits + qxl->slot_id_bits));
    slot->high_bits = high_bits;
    return slot_index;
}

static void
qxl_reset (qxl_screen_t *qxl)
{
    ioport_write(qxl, QXL_IO_RESET, 0);
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
    qxl->main_mem_slot = qxl->vram_mem_slot = setup_slot(qxl, 0, 0, ~0, 0, ~0);
#else /* QXL */
    qxl->main_mem_slot = setup_slot(qxl, 0,
        (unsigned long)qxl->ram_physical,
        (unsigned long)qxl->ram_physical + (unsigned long)qxl->rom->num_pages * getpagesize(),
        (uint64_t)(uintptr_t)qxl->ram,
        (uint64_t)(uintptr_t)qxl->ram + (unsigned long)qxl->rom->num_pages * getpagesize()
    );
    qxl->vram_mem_slot = setup_slot(qxl, 1,
        (unsigned long)qxl->vram_physical,
        (unsigned long)qxl->vram_physical + (unsigned long)qxl->vram_size,
        (uint64_t)(uintptr_t)qxl->vram,
        (uint64_t)(uintptr_t)qxl->vram + (uint64_t)qxl->vram_size);
#endif
}

static Bool
qxl_close_screen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool result;
    
    ErrorF ("Freeing %p\n", qxl->fb);
    free(qxl->fb);
    qxl->fb = NULL;
    
    pScreen->CreateScreenResources = qxl->create_screen_resources;
    pScreen->CloseScreen = qxl->close_screen;
    
    result = pScreen->CloseScreen(scrnIndex, pScreen);

#ifndef XSPICE
    if (!xf86IsPrimaryPci (qxl->pci) && qxl->primary)
       qxl_reset (qxl);
#endif
    
    if (pScrn->vtSema)
    {
        qxl_restore_state(pScrn);
	qxl_unmap_memory(qxl, scrnIndex);
    }
    pScrn->vtSema = FALSE;

    return result;
}

static void
set_screen_pixmap_header (ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap = pScreen->GetScreenPixmap(pScreen);
    
    if (pPixmap && qxl->current_mode)
    {
	ErrorF ("new stride: %d (display width: %d, bpp: %d)\n",
		qxl->pScrn->displayWidth * qxl->bytes_per_pixel,
		qxl->pScrn->displayWidth, qxl->bytes_per_pixel);
	
	pScreen->ModifyPixmapHeader(
	    pPixmap,
	    qxl->current_mode->x_res, qxl->current_mode->y_res,
	    -1, -1,
	    qxl->pScrn->displayWidth * qxl->bytes_per_pixel,
	    NULL);
    }
    else
	ErrorF ("pix: %p; mode: %p\n", pPixmap, qxl->current_mode);
}

static Bool
qxl_switch_mode(int scrnIndex, DisplayModePtr p, int flags)
{
    qxl_screen_t *qxl = xf86Screens[scrnIndex]->driverPrivate;
    int mode_index = (int)(unsigned long)p->Private;
    struct QXLMode *m = qxl->modes + mode_index;
    ScreenPtr pScreen;
    void *evacuated;

    evacuated = qxl_surface_cache_evacuate_all (qxl->surface_cache);

    if (qxl->primary)
    {
	qxl_surface_kill (qxl->primary);
	qxl_surface_cache_sanity_check (qxl->surface_cache);
    }
	
    qxl_reset (qxl);
    
    ErrorF ("done reset\n");

    qxl->primary = qxl_surface_cache_create_primary (qxl->surface_cache, m);
    qxl->current_mode = m;
    qxl->bytes_per_pixel = (qxl->pScrn->bitsPerPixel + 7) / 8;

    pScreen = qxl->pScrn->pScreen;
    if (pScreen)
    {
	PixmapPtr root = pScreen->GetScreenPixmap (pScreen);
	qxl_surface_t *surf;

	if ((surf = get_surface (root)))
	    qxl_surface_kill (surf);
	
	set_surface (root, qxl->primary);
    }
    
    ErrorF ("primary is %p\n", qxl->primary);
    if (qxl->mem)
    {
       qxl_mem_free_all (qxl->mem);
       qxl_drop_image_cache (qxl);
    }

    if (qxl->surf_mem)
	qxl_mem_free_all (qxl->surf_mem);

    qxl_surface_cache_replace_all (qxl->surface_cache, evacuated);
    
    return TRUE;
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

static Bool
qxl_create_screen_resources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool ret;
    PixmapPtr pPixmap;
    qxl_surface_t *surf;
    
    pScreen->CreateScreenResources = qxl->create_screen_resources;
    ret = pScreen->CreateScreenResources (pScreen);
    pScreen->CreateScreenResources = qxl_create_screen_resources;
    
    if (!ret)
	return FALSE;

    pPixmap = pScreen->GetScreenPixmap (pScreen);
    
    set_screen_pixmap_header (pScreen);

    if ((surf = get_surface (pPixmap)))
	qxl_surface_kill (surf);
    
    set_surface (pPixmap, qxl->primary);
    
    return TRUE;
}

#if HAS_DEVPRIVATEKEYREC
DevPrivateKeyRec uxa_pixmap_index;
#else
int uxa_pixmap_index;
#endif

static Bool
unaccel (void)
{
    return FALSE;
}

static Bool
qxl_prepare_access (PixmapPtr pixmap, RegionPtr region, uxa_access_t access)
{
    return qxl_surface_prepare_access (get_surface (pixmap),
				       pixmap, region, access);
}

static void
qxl_finish_access (PixmapPtr pixmap)
{
    qxl_surface_finish_access (get_surface (pixmap), pixmap);
}

static Bool
qxl_pixmap_is_offscreen (PixmapPtr pixmap)
{
    return !!get_surface (pixmap);
}


static Bool
good_alu_and_pm (DrawablePtr drawable, int alu, Pixel planemask)
{
    if (!UXA_PM_IS_SOLID (drawable, planemask))
	return FALSE;
    
    if (alu != GXcopy)
	return FALSE;
    
    return TRUE;
}

/*
 * Solid fill
 */
static Bool
qxl_check_solid (DrawablePtr drawable, int alu, Pixel planemask)
{
    if (!good_alu_and_pm (drawable, alu, planemask))
	return FALSE;
    
    return TRUE;
}

static Bool
qxl_prepare_solid (PixmapPtr pixmap, int alu, Pixel planemask, Pixel fg)
{
    qxl_surface_t *surface;
    
    if (!(surface = get_surface (pixmap)))
	return FALSE;
    
    return qxl_surface_prepare_solid (surface, fg);
}

static void
qxl_solid (PixmapPtr pixmap, int x1, int y1, int x2, int y2)
{
    qxl_surface_solid (get_surface (pixmap), x1, y1, x2, y2);
}

static void
qxl_done_solid (PixmapPtr pixmap)
{
}

/*
 * Copy
 */
static Bool
qxl_check_copy (PixmapPtr source, PixmapPtr dest,
		int alu, Pixel planemask)
{
    if (!good_alu_and_pm ((DrawablePtr)source, alu, planemask))
	return FALSE;
    
    if (source->drawable.bitsPerPixel != dest->drawable.bitsPerPixel)
    {
	ErrorF ("differing bitsperpixel - this shouldn't happen\n");
	return FALSE;
    }
    
    return TRUE;
}

static Bool
qxl_prepare_copy (PixmapPtr source, PixmapPtr dest,
		  int xdir, int ydir, int alu,
		  Pixel planemask)
{
    return qxl_surface_prepare_copy (get_surface (dest), get_surface (source));
}

static void
qxl_copy (PixmapPtr dest,
	  int src_x1, int src_y1,
	  int dest_x1, int dest_y1,
	  int width, int height)
{
    qxl_surface_copy (get_surface (dest),
		      src_x1, src_y1,
		      dest_x1, dest_y1,
		      width, height);
}

static void
qxl_done_copy (PixmapPtr dest)
{
}

static Bool
qxl_put_image (PixmapPtr pDst, int x, int y, int w, int h,
	       char *src, int src_pitch)
{
    qxl_surface_t *surface = get_surface (pDst);

    if (surface)
	return qxl_surface_put_image (surface, x, y, w, h, src, src_pitch);

    return FALSE;
}

static void
qxl_set_screen_pixmap (PixmapPtr pixmap)
{
    pixmap->drawable.pScreen->devPrivate = pixmap;
}

static PixmapPtr
qxl_create_pixmap (ScreenPtr screen, int w, int h, int depth, unsigned usage)
{
    ScrnInfoPtr scrn = xf86Screens[screen->myNum];
    PixmapPtr pixmap;
    qxl_screen_t *qxl = scrn->driverPrivate;
    qxl_surface_t *surface;
    
    if (w > 32767 || h > 32767)
	return NULL;

    qxl_surface_cache_sanity_check (qxl->surface_cache);

#if 0
    ErrorF ("Create pixmap: %d %d @ %d (usage: %d)\n", w, h, depth, usage);
#endif

    if (uxa_swapped_out (screen))
	goto fallback;
    
    surface = qxl_surface_create (qxl->surface_cache, w, h, depth);
    
    if (surface)
    {
	/* ErrorF ("   Successfully created surface in video memory\n"); */
	
	pixmap = fbCreatePixmap (screen, 0, 0, depth, usage);

	screen->ModifyPixmapHeader(pixmap, w, h,
				   -1, -1, -1,
				   NULL);
	
#if 0
	ErrorF ("Create pixmap %p with surface %p\n", pixmap, surface);
#endif
	set_surface (pixmap, surface);
	qxl_surface_set_pixmap (surface, pixmap);

	qxl_surface_cache_sanity_check (qxl->surface_cache);
    }
    else
    {
#if 0
	ErrorF ("   Couldn't allocate %d x %d @ %d surface in video memory\n",
		w, h, depth);
#endif
    fallback:
	pixmap = fbCreatePixmap (screen, w, h, depth, usage);

#if 0
    	ErrorF ("Create pixmap %p without surface\n", pixmap);
#endif
    }
    
    return pixmap;
}

static Bool
qxl_destroy_pixmap (PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86Screens[screen->myNum];
    qxl_screen_t *qxl = scrn->driverPrivate;
    qxl_surface_t *surface = NULL;

    qxl_surface_cache_sanity_check (qxl->surface_cache);
    
    if (pixmap->refcnt == 1)
    {
	surface = get_surface (pixmap);

#if 0
	ErrorF ("- Destroy %p (had surface %p)\n", pixmap, surface);
#endif
	    
	if (surface)
	{
	    qxl_surface_kill (surface);
	    set_surface (pixmap, NULL);

	    qxl_surface_cache_sanity_check (qxl->surface_cache);
	}
    }
    
    fbDestroyPixmap (pixmap);
    return TRUE;
}

static Bool
setup_uxa (qxl_screen_t *qxl, ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86Screens[screen->myNum];
#if HAS_DIXREGISTERPRIVATEKEY
    if (!dixRegisterPrivateKey(&uxa_pixmap_index, PRIVATE_PIXMAP, 0))
	return FALSE;
#else
    if (!dixRequestPrivate(&uxa_pixmap_index, 0))
	return FALSE;
#endif
    
    qxl->uxa = uxa_driver_alloc();
    if (qxl->uxa == NULL)
	return FALSE;

    memset(qxl->uxa, 0, sizeof(*qxl->uxa));
    
    qxl->uxa->uxa_major = 1;
    qxl->uxa->uxa_minor = 0;
    
    /* Solid fill */
    qxl->uxa->check_solid = qxl_check_solid;
    qxl->uxa->prepare_solid = qxl_prepare_solid;
    qxl->uxa->solid = qxl_solid;
    qxl->uxa->done_solid = qxl_done_solid;
    
    /* Copy */
    qxl->uxa->check_copy = qxl_check_copy;
    qxl->uxa->prepare_copy = qxl_prepare_copy;
    qxl->uxa->copy = qxl_copy;
    qxl->uxa->done_copy = qxl_done_copy;
    
    /* Composite */
    qxl->uxa->check_composite = (typeof(qxl->uxa->check_composite))unaccel;
    qxl->uxa->check_composite_target = (typeof(qxl->uxa->check_composite_target))unaccel;
    qxl->uxa->check_composite_texture = (typeof(qxl->uxa->check_composite_texture))unaccel;
    qxl->uxa->prepare_composite = (typeof(qxl->uxa->prepare_composite))unaccel;
    qxl->uxa->composite = (typeof(qxl->uxa->composite))unaccel;
    qxl->uxa->done_composite = (typeof(qxl->uxa->done_composite))unaccel;
    
    /* PutImage */
    qxl->uxa->put_image = qxl_put_image;
    
    /* Prepare access */
    qxl->uxa->prepare_access = qxl_prepare_access;
    qxl->uxa->finish_access = qxl_finish_access;
    
    qxl->uxa->pixmap_is_offscreen = qxl_pixmap_is_offscreen;

    screen->SetScreenPixmap = qxl_set_screen_pixmap;
    screen->CreatePixmap = qxl_create_pixmap;
    screen->DestroyPixmap = qxl_destroy_pixmap;
    
    if (!uxa_driver_init(screen, qxl->uxa))
    {
	xf86DrvMsg(scrn->scrnIndex, X_ERROR,
		   "UXA initialization failed\n");
	free(qxl->uxa);
	return FALSE;
    }
    
#if 0
    uxa_set_fallback_debug(screen, FALSE);
#endif
    
#if 0
    if (!uxa_driver_init (screen, qxl->uxa))
	return FALSE;
#endif
    
    return TRUE;
}

#ifdef XSPICE

static void
spiceqxl_screen_init(int scrnIndex, ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    SpiceCoreInterface *core;

    // Init spice
    if (!qxl->spice_server) {
        qxl->spice_server = xspice_get_spice_server();
        xspice_set_spice_server_options(qxl->options);
        core = basic_event_loop_init();
        spice_server_init(qxl->spice_server, core);
        qxl_add_spice_display_interface(qxl);
        qxl->worker->start(qxl->worker);
        qxl->worker_running = TRUE;
    }
    qxl->spice_server = qxl->spice_server;
}

#endif

static Bool
qxl_screen_init(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    struct QXLRam *ram_header;
    VisualPtr visual;

    CHECK_POINT();

    qxl->pScrn = pScrn;

    if (!qxl_map_memory(qxl, scrnIndex))
	return FALSE;

#ifdef XSPICE
    spiceqxl_screen_init(scrnIndex, pScrn, qxl);
#endif
    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);
    
    printf ("ram_header at %d\n", qxl->rom->ram_header_offset);
    printf ("surf0 size: %d\n", qxl->rom->surface0_area_size);
    
    qxl_save_state(pScrn);
    qxl_blank_screen(pScreen, SCREEN_SAVER_ON);
    
    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	goto out;
    if (!miSetPixmapDepths())
	goto out;
    
    qxl->virtual_x = pScrn->virtualX;
    qxl->virtual_y = pScrn->virtualY;
    qxl->stride = pScrn->virtualX * 4;
    
    pScrn->displayWidth = pScrn->virtualX;
    
    qxl->fb = calloc (pScrn->virtualY * pScrn->displayWidth, 4);
    if (!qxl->fb)
	goto out;
    
#if 0
    ErrorF ("allocated %d x %d  %p\n", pScrn->virtualX, pScrn->virtualY, qxl->fb);
#endif
    
    pScreen->totalPixmapSize = 100;

    pScrn->virtualX = pScrn->currentMode->HDisplay;
    pScrn->virtualY = pScrn->currentMode->VDisplay;

    if (!fbScreenInit(pScreen, qxl->fb,
		      pScrn->virtualX, pScrn->virtualY,
		      pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
		      pScrn->bitsPerPixel))
    {
	goto out;
    }
    
    visual = pScreen->visuals + pScreen->numVisuals;
    while (--visual >= pScreen->visuals) 
    {
	if ((visual->class | DynamicClass) == DirectColor) 
	{
	    visual->offsetRed = pScrn->offset.red;
	    visual->offsetGreen = pScrn->offset.green;
	    visual->offsetBlue = pScrn->offset.blue;
	    visual->redMask = pScrn->mask.red;
	    visual->greenMask = pScrn->mask.green;
	    visual->blueMask = pScrn->mask.blue;
	}
    }
    
    fbPictureInit(pScreen, NULL, 0);
    
    qxl->uxa = uxa_driver_alloc ();
    
    /* Set up resources */
    qxl_reset (qxl);
    ErrorF ("done reset\n");

#ifndef XSPICE
    qxl->io_pages = (void *)((unsigned long)qxl->ram);
    qxl->io_pages_physical = (void *)((unsigned long)qxl->ram_physical);
#endif

    qxl->command_ring = qxl_ring_create ((struct qxl_ring_header *)&(ram_header->cmd_ring),
					 sizeof (struct QXLCommand),
					 QXL_COMMAND_RING_SIZE, QXL_IO_NOTIFY_CMD, qxl);
    qxl->cursor_ring = qxl_ring_create ((struct qxl_ring_header *)&(ram_header->cursor_ring),
					sizeof (struct QXLCommand),
					QXL_CURSOR_RING_SIZE, QXL_IO_NOTIFY_CURSOR, qxl);
    qxl->release_ring = qxl_ring_create ((struct qxl_ring_header *)&(ram_header->release_ring),
					 sizeof (uint64_t),
					 QXL_RELEASE_RING_SIZE, 0, qxl);

    qxl->surface_cache = qxl_surface_cache_create (qxl);
    
    /* xf86DPMSInit(pScreen, xf86DPMSSet, 0); */
    
    pScreen->SaveScreen = qxl_blank_screen;

    setup_uxa (qxl, pScreen);

    DamageSetup(pScreen);
    
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());
    if (!miCreateDefColormap(pScreen))
      goto out;

    /* Note: this must be done after DamageSetup() because it calls
     * _dixInitPrivates. And if that has been called, DamageSetup()
     * will assert.
     */
    if (!uxa_resources_init (pScreen))
	return FALSE;
    
    qxl->create_screen_resources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = qxl_create_screen_resources;
    
    qxl->close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = qxl_close_screen;
    
    qxl_cursor_init (pScreen);

    CHECK_POINT();

    pScreen->width = pScrn->currentMode->HDisplay;
    pScreen->height = pScrn->currentMode->VDisplay;
    
    qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);
    
    CHECK_POINT();

    return TRUE;
    
out:
    return FALSE;
}

static Bool
qxl_enter_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;

    qxl_save_state(pScrn);
    qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);

    if (qxl->vt_surfaces)
    {
	qxl_surface_cache_replace_all (qxl->surface_cache, qxl->vt_surfaces);

	qxl->vt_surfaces = NULL;
    }

    pScrn->EnableDisableFBAccess (scrnIndex, TRUE);
    
    return TRUE;
}

static void
qxl_leave_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    pScrn->EnableDisableFBAccess (scrnIndex, FALSE);

    qxl->vt_surfaces = qxl_surface_cache_evacuate_all (qxl->surface_cache);

    outb(qxl->io_base + QXL_IO_RESET, 0);

    qxl_restore_state(pScrn);
}

static Bool
qxl_color_setup(ScrnInfoPtr pScrn)
{
    int scrnIndex = pScrn->scrnIndex;
    Gamma gzeros = { 0.0, 0.0, 0.0 };
    rgb rzeros = { 0, 0, 0 };
    
    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb))
	return FALSE;
    
    if (pScrn->depth != 15 && pScrn->depth != 24) 
    {
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
print_modes (qxl_screen_t *qxl, int scrnIndex)
{
    int i;
    
    for (i = 0; i < qxl->num_modes; ++i)
    {
	struct QXLMode *m = qxl->modes + i;
	
	xf86DrvMsg (scrnIndex, X_INFO,
		    "%d: %dx%d, %d bits, stride %d, %dmm x %dmm, orientation %d\n",
		    m->id, m->x_res, m->y_res, m->bits, m->stride, m->x_mili,
		    m->y_mili, m->orientation);
    }
}

#ifndef XSPICE
static Bool
qxl_check_device(ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    int scrnIndex = pScrn->scrnIndex;
    struct QXLRom *rom = qxl->rom;
    struct QXLRam *ram_header = (void *)((unsigned long)qxl->ram + rom->ram_header_offset);
    
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
    
    xf86DrvMsg(scrnIndex, X_INFO, "%d io pages at 0x%lx\n",
	       rom->num_pages, (unsigned long)qxl->ram);
    
    xf86DrvMsg(scrnIndex, X_INFO, "RAM header offset: 0x%x\n", rom->ram_header_offset);

    if (ram_header->magic != 0x41525851) { /* "QXRA" little-endian */
	xf86DrvMsg(scrnIndex, X_ERROR, "Bad RAM signature %x at %p\n",
		   ram_header->magic,
		   &ram_header->magic);
	return FALSE;
    }

    xf86DrvMsg(scrnIndex, X_INFO, "Correct RAM signature %x\n",
	       ram_header->magic);
    return TRUE;
}
#endif /* !XSPICE */

static int
qxl_find_native_mode(ScrnInfoPtr pScrn, DisplayModePtr p)
{
    int i;
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    CHECK_POINT();
    
    for (i = 0; i < qxl->num_modes; i++) 
    {
	struct QXLMode *m = qxl->modes + i;
	
	if (m->x_res == p->HDisplay &&
	    m->y_res == p->VDisplay &&
	    m->bits == pScrn->bitsPerPixel)
	{
	    if (m->bits == 16) 
	    {
		/* What QXL calls 16 bit is actually x1r5g5b515 */
		if (pScrn->depth == 15)
		    return i;
	    }
	    else if (m->bits == 32)
	    {
		/* What QXL calls 32 bit is actually x8r8g8b8 */
		if (pScrn->depth == 24)
		    return i;
	    }
	}
    }
    
    return -1;
}

static ModeStatus
qxl_valid_mode(int scrn, DisplayModePtr p, Bool flag, int pass)
{
    ScrnInfoPtr pScrn = xf86Screens[scrn];
    int scrnIndex = pScrn->scrnIndex;
    qxl_screen_t *qxl = pScrn->driverPrivate;
    int bpp = pScrn->bitsPerPixel;
    int mode_idx;
    
    /* FIXME: I don't think this is necessary now that we report the
     * correct amount of video ram?
     */
    if (p->HDisplay * p->VDisplay * (bpp/8) > qxl->surface0_size)
    {
	xf86DrvMsg(scrnIndex, X_INFO, "rejecting mode %d x %d: insufficient memory\n", p->HDisplay, p->VDisplay);
	return MODE_MEM;
    }
    
    mode_idx = qxl_find_native_mode (pScrn, p);
    if (mode_idx == -1)
    {
	xf86DrvMsg(scrnIndex, X_INFO, "rejecting unknown mode %d x %d\n", p->HDisplay, p->VDisplay);
	return MODE_NOMODE;
    }
    p->Private = (void *)(unsigned long)mode_idx;
    
    xf86DrvMsg (scrnIndex, X_INFO, "accepting %d x %d\n", p->HDisplay, p->VDisplay);
    
    return MODE_OK;
}

static void qxl_add_mode(ScrnInfoPtr pScrn, int width, int height, int type)
{
    DisplayModePtr mode;

    /* Skip already present modes */
    for (mode = pScrn->monitor->Modes; mode; mode = mode->next)
        if (mode->HDisplay == width && mode->VDisplay == height)
            return;

    mode = xnfcalloc(1, sizeof(DisplayModeRec));

    mode->status = MODE_OK;
    mode->type = type;
    mode->HDisplay   = width;
    mode->HSyncStart = (width * 105 / 100 + 7) & ~7;
    mode->HSyncEnd   = (width * 115 / 100 + 7) & ~7;
    mode->HTotal     = (width * 130 / 100 + 7) & ~7;
    mode->VDisplay   = height;
    mode->VSyncStart = height + 1;
    mode->VSyncEnd   = height + 4;
    mode->VTotal     = height * 1035 / 1000;
    mode->Clock = mode->HTotal * mode->VTotal * 60 / 1000;
    mode->Flags = V_NHSYNC | V_PVSYNC;

    xf86SetModeDefaultName(mode);
    xf86ModesAdd(pScrn->monitor->Modes, mode);
}

static Bool
qxl_pre_init(ScrnInfoPtr pScrn, int flags)
{
    int i, scrnIndex = pScrn->scrnIndex;
    qxl_screen_t *qxl = NULL;
    ClockRangePtr clockRanges = NULL;
    int *linePitches = NULL;
    DisplayModePtr mode;
    unsigned int max_x = 0, max_y = 0;

    /* In X server 1.7.5, Xorg -configure will cause this
     * function to get called without a confScreen.
     */
    if (!pScrn->confScreen)
	return FALSE;
    
    CHECK_POINT();

    /* zaphod mode is for suckers and i choose not to implement it */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	xf86DrvMsg(scrnIndex, X_ERROR, "No Zaphod mode for you\n");
	return FALSE;
    }
    
    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc(sizeof(qxl_screen_t), 1);
    qxl = pScrn->driverPrivate;

    qxl->entity = xf86GetEntityInfo(pScrn->entityList[0]);
    
#ifndef XSPICE
    qxl->pci = xf86GetPciInfoForEntity(qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    qxl->pci_tag = pciTag(qxl->pci->bus, qxl->pci->device, qxl->pci->func);
#endif
#endif /* XSPICE */

    pScrn->monitor = pScrn->confScreen->monitor;
    
    if (!qxl_color_setup(pScrn))
	goto out;
    
    /* option parsing and card differentiation */
    xf86CollectOptions(pScrn, NULL);
    memcpy(qxl->options, DefaultOptions, sizeof(DefaultOptions));
    xf86ProcessOptions(scrnIndex, pScrn->options, qxl->options);

    qxl->enable_image_cache =
	xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_IMAGE_CACHE, TRUE);
    qxl->enable_fallback_cache =
	xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_FALLBACK_CACHE, TRUE);
    qxl->enable_surfaces =
	xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_SURFACES, TRUE);

    xf86DrvMsg(scrnIndex, X_INFO, "Offscreen Surfaces: %s\n",
	       qxl->enable_surfaces? "Enabled" : "Disabled");
    xf86DrvMsg(scrnIndex, X_INFO, "Image Cache: %s\n",
	       qxl->enable_image_cache? "Enabled" : "Disabled");
    xf86DrvMsg(scrnIndex, X_INFO, "Fallback Cache: %s\n",
	       qxl->enable_fallback_cache? "Enabled" : "Disabled");
    
    if (!qxl_map_memory(qxl, scrnIndex))
	goto out;
    
#ifndef XSPICE
    if (!qxl_check_device(pScrn, qxl))
	goto out;
#else
    xspice_init_qxl_ram(qxl); /* initialize the rings */
#endif
    pScrn->videoRam = (qxl->rom->num_pages * 4096) / 1024;
    xf86DrvMsg(scrnIndex, X_INFO, "%d KB of video RAM\n", pScrn->videoRam);
    xf86DrvMsg(scrnIndex, X_INFO, "%d surfaces\n", qxl->rom->n_surfaces);

    /* ddc stuff here */
    
    clockRanges = xnfcalloc(sizeof(ClockRange), 1);
    clockRanges->next = NULL;
    clockRanges->minClock = 10000;
    clockRanges->maxClock = 400000;
    clockRanges->clockIndex = -1;
    clockRanges->interlaceAllowed = clockRanges->doubleScanAllowed = 0;
    clockRanges->ClockMulFactor = clockRanges->ClockDivFactor = 1;
    pScrn->progClock = TRUE;
    
    /* override QXL monitor stuff */
    if (pScrn->monitor->nHsync <= 0) {
	pScrn->monitor->hsync[0].lo =  29.0;
	pScrn->monitor->hsync[0].hi = 160.0;
	pScrn->monitor->nHsync = 1;
    }
    if (pScrn->monitor->nVrefresh <= 0) {
	pScrn->monitor->vrefresh[0].lo = 50;
	pScrn->monitor->vrefresh[0].hi = 75;
	pScrn->monitor->nVrefresh = 1;
    }
    
    /* Add any modes not in xorg's default mode list */
    for (i = 0; i < qxl->num_modes; i++)
        if (qxl->modes[i].orientation == 0) {
            qxl_add_mode(pScrn, qxl->modes[i].x_res, qxl->modes[i].y_res,
                         M_T_DRIVER);
            if (qxl->modes[i].x_res > max_x)
                max_x = qxl->modes[i].x_res;
            if (qxl->modes[i].y_res > max_y)
                max_y = qxl->modes[i].y_res;
        }

    if (pScrn->display->virtualX == 0 && pScrn->display->virtualY == 0) {
        /* It is possible for the largest x + largest y size combined leading
           to a virtual size which will not fit into the framebuffer when this
           happens we prefer max width and make height as large as possible */
        if (max_x * max_y * (pScrn->bitsPerPixel / 8) >
                qxl->rom->surface0_area_size)
            pScrn->display->virtualY = qxl->rom->surface0_area_size /
                                       (max_x * (pScrn->bitsPerPixel / 8));
        else
            pScrn->display->virtualY = max_y;

    	pScrn->display->virtualX = max_x;
    }

    if (0 >= xf86ValidateModes(pScrn, pScrn->monitor->Modes,
			       pScrn->display->modes, clockRanges, linePitches,
			       128, max_x, 128 * 4, 128, max_y,
			       pScrn->display->virtualX,
			       pScrn->display->virtualY,
			       128 * 1024 * 1024, LOOKUP_BEST_REFRESH))
	goto out;
    
    CHECK_POINT();
    
    xf86PruneDriverModes(pScrn);
    pScrn->currentMode = pScrn->modes;
    /* If no modes are specified in xorg.conf, default to 1024x768 */
    if (pScrn->display->modes == NULL || pScrn->display->modes[0] == NULL)
        for (mode = pScrn->modes; mode; mode = mode->next)
            if (mode->HDisplay == 1024 && mode->VDisplay == 768) {
                pScrn->currentMode = mode;
                break;
            }

    xf86PrintModes(pScrn);
    xf86SetDpi(pScrn, 0, 0);
    
    if (!xf86LoadSubModule(pScrn, "fb")
#ifndef XSPICE
	|| !xf86LoadSubModule(pScrn, "ramdac")
	|| !xf86LoadSubModule(pScrn, "vgahw")
#endif
    )
    {
	goto out;
    }
    
    print_modes (qxl, scrnIndex);

#ifndef XSPICE
    /* VGA hardware initialisation */
    if (!vgaHWGetHWRec(pScrn))
        return FALSE;
    vgaHWSetStdFuncs(VGAHWPTR(pScrn));
#endif

    /* hate */
    qxl_unmap_memory(qxl, scrnIndex);
    
    CHECK_POINT();
    
    xf86DrvMsg(scrnIndex, X_INFO, "PreInit complete\n");
#ifdef GIT_VERSION
    xf86DrvMsg(scrnIndex, X_INFO, "git commit %s\n", GIT_VERSION);
#endif
    return TRUE;
    
out:
    if (clockRanges)
	free(clockRanges);
    if (qxl)
	free(qxl);
    
    return FALSE;
}

#ifndef XSPICE
#ifdef XSERVER_LIBPCIACCESS
enum qxl_class
{
    CHIP_QXL_1,
};

static const struct pci_id_match qxl_device_match[] = {
    {
	PCI_VENDOR_RED_HAT, PCI_CHIP_QXL_0100, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00000000, 0x00000000, CHIP_QXL_1
    },
    {
	PCI_VENDOR_RED_HAT, PCI_CHIP_QXL_01FF, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00000000, 0x00000000, CHIP_QXL_1
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
#endif /* !XSPICE */

static void
qxl_identify(int flags)
{
#ifndef XSPICE
    xf86PrintChipsets("qxl", "Driver for QXL virtual graphics", qxlChips);
#endif
}

static void
qxl_init_scrn(ScrnInfoPtr pScrn)
{
    pScrn->driverVersion    = 0;
    pScrn->driverName	    = pScrn->name = QXL_DRIVER_NAME;
    pScrn->PreInit	    = qxl_pre_init;
    pScrn->ScreenInit	    = qxl_screen_init;
    pScrn->SwitchMode	    = qxl_switch_mode;
    pScrn->ValidMode	    = qxl_valid_mode;
    pScrn->EnterVT	    = qxl_enter_vt;
    pScrn->LeaveVT	    = qxl_leave_vt;
}

#ifdef XSPICE
static Bool
qxl_probe(struct _DriverRec *drv, int flags)
{
    ScrnInfoPtr pScrn;
    int entityIndex;
    EntityInfoPtr pEnt;
    GDevPtr* device;

    if (flags & PROBE_DETECT) {
        return TRUE;
    }

    pScrn = xf86AllocateScreen(drv, flags);
    qxl_init_scrn(pScrn);

    xf86MatchDevice(QXL_DRIVER_NAME, &device);
    entityIndex = xf86ClaimNoSlot(drv, 0, device[0], TRUE);
    pEnt = xf86GetEntityInfo(entityIndex);
    pEnt->driver = drv;

    xf86AddEntityToScreen(pScrn, entityIndex);

    return TRUE;
}
static Bool qxl_driver_func(ScrnInfoPtr screen_info_ptr, xorgDriverFuncOp xorg_driver_func_op, pointer hw_flags)
{
    *(xorgHWFlags*)hw_flags = (xorgHWFlags)HW_SKIP_CONSOLE;
    return TRUE;
}
#else /* normal, not XSPICE */
#ifndef XSERVER_LIBPCIACCESS
static Bool
qxl_probe(DriverPtr drv, int flags)
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
	    qxl_init_scrn(pScrn);
    }

    xfree(usedChips);
    return TRUE;
}

#else /* pciaccess */

static Bool
qxl_pci_probe(DriverPtr drv, int entity, struct pci_device *dev, intptr_t match)
{
    qxl_screen_t *qxl;
    ScrnInfoPtr pScrn = xf86ConfigPciEntity(NULL, 0, entity, NULL, NULL,
					    NULL, NULL, NULL, NULL);

    if (!pScrn)
	return FALSE;

    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc(sizeof(qxl_screen_t), 1);
    qxl = pScrn->driverPrivate;
    qxl->pci = dev;

    qxl_init_scrn(pScrn);

    return TRUE;
}

#define qxl_probe NULL

#endif
#endif /* XSPICE */

static DriverRec qxl_driver = {
    0,
    QXL_DRIVER_NAME,
    qxl_identify,
    qxl_probe,
    qxl_available_options,
    NULL,
    0,
#ifdef XSPICE
    qxl_driver_func,
    NULL,
    NULL
#else
    NULL,
#ifdef XSERVER_LIBPCIACCESS
    qxl_device_match,
    qxl_pci_probe
#endif
#endif /* XSPICE */
};

static pointer
qxl_setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool loaded = FALSE;
    
    if (!loaded) {
	loaded = TRUE;
	xf86AddDriver(&qxl_driver, module, HaveDriverFuncs);
#ifdef XSPICE
	xspice_add_input_drivers(module);
#endif
	return (void *)1;
    } else {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	return NULL;
    }
}

static XF86ModuleVersionInfo qxl_module_info = {
    QXL_DRIVER_NAME,
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

_X_EXPORT XF86ModuleData
#ifdef XSPICE
spiceqxlModuleData
#else
qxlModuleData
#endif
= {
    &qxl_module_info,
    qxl_setup,
    NULL
};
