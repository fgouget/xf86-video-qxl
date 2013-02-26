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

#include <xf86Crtc.h>
#include <xf86RandR12.h>

#include "qxl.h"
#include "assert.h"
#include "qxl_option_helpers.h"
#include <spice/protocol.h>

#ifdef XSPICE
#include "spiceqxl_driver.h"
#include "spiceqxl_main_loop.h"
#include "spiceqxl_display.h"
#include "spiceqxl_inputs.h"
#include "spiceqxl_io_port.h"
#include "spiceqxl_spice_server.h"
#include "dfps.h"
#endif /* XSPICE */

extern void compat_init_scrn (ScrnInfoPtr);

#define BREAKPOINT()   do { __asm__ __volatile__ ("int $03"); } while (0)

const OptionInfoRec DefaultOptions[] =
{
    { OPTION_ENABLE_IMAGE_CACHE,
      "EnableImageCache",         OPTV_BOOLEAN, { 0 }, TRUE },
    { OPTION_ENABLE_FALLBACK_CACHE,
      "EnableFallbackCache",      OPTV_BOOLEAN, { 0 }, TRUE },
    { OPTION_ENABLE_SURFACES,
      "EnableSurfaces",           OPTV_BOOLEAN, { 0 }, TRUE },
    { OPTION_NUM_HEADS,
      "NumHeads",                 OPTV_INTEGER, { 4 }, FALSE },
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
      "SpiceStreamingVideo",      OPTV_STRING,    {.str = "filter"}, FALSE},
    { OPTION_SPICE_PLAYBACK_COMPRESSION,
      "SpicePlaybackCompression", OPTV_BOOLEAN,   {1}, FALSE},
    { OPTION_SPICE_ZLIB_GLZ_WAN_COMPRESSION,
      "SpiceZlibGlzWanCompression", OPTV_STRING,  {.str = "auto"}, FALSE},
    { OPTION_SPICE_JPEG_WAN_COMPRESSION,
      "SpiceJpegWanCompression",  OPTV_STRING,    {.str = "auto"}, FALSE},
    { OPTION_SPICE_IMAGE_COMPRESSION,
      "SpiceImageCompression",    OPTV_STRING,    {.str = "auto_glz"}, FALSE},
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
    { OPTION_SPICE_DEFERRED_FPS,
      "SpiceDeferredFPS",         OPTV_INTEGER,   {0}, FALSE},
    { OPTION_SPICE_EXIT_ON_DISCONNECT,
      "SpiceExitOnDisconnect",    OPTV_BOOLEAN,   {0}, FALSE},
#endif
    
    { -1, NULL, OPTV_NONE, {0}, FALSE }
};

static const OptionInfoRec *
qxl_available_options (int chipid, int busid)
{
    return DefaultOptions;
}

/* Having a single monitors config struct allocated on the device avoids any
 *
 * possible fragmentation. Since X is single threaded there is no danger
 * in us changing it between issuing the io and getting the interrupt to signal
 * spice-server is done reading it.
 */
#define MAX_MONITORS_NUM 16

void
qxl_allocate_monitors_config (qxl_screen_t *qxl)
{
    qxl->monitors_config = (QXLMonitorsConfig *)(void *)
	((unsigned long)qxl->ram + qxl->rom->ram_header_offset - qxl->monitors_config_size);
}

static Bool
qxl_blank_screen (ScreenPtr pScreen, int mode)
{
    return TRUE;
}

#ifdef XSPICE
static void
unmap_memory_helper (qxl_screen_t *qxl)
{
    free (qxl->ram);
    free (qxl->vram);
    free (qxl->rom);
}

static void
map_memory_helper (qxl_screen_t *qxl)
{
    qxl->ram = malloc (RAM_SIZE);
    qxl->ram_size = RAM_SIZE;
    qxl->ram_physical = qxl->ram;
    qxl->vram = malloc (VRAM_SIZE);
    qxl->vram_size = VRAM_SIZE;
    qxl->vram_physical = qxl->vram;
    qxl->rom = malloc (ROM_SIZE);
    
    init_qxl_rom (qxl, ROM_SIZE);
}

#else /* Default */

static void
unmap_memory_helper (qxl_screen_t *qxl)
{
#ifdef XSERVER_LIBPCIACCESS
    if (qxl->ram)
	pci_device_unmap_range (qxl->pci, qxl->ram, qxl->pci->regions[0].size);
    if (qxl->vram)
	pci_device_unmap_range (qxl->pci, qxl->vram, qxl->pci->regions[1].size);
    if (qxl->rom)
	pci_device_unmap_range (qxl->pci, qxl->rom, qxl->pci->regions[2].size);
#else
    if (qxl->ram)
	xf86UnMapVidMem (scrnIndex, qxl->ram, (1 << qxl->pci->size[0]));
    if (qxl->vram)
	xf86UnMapVidMem (scrnIndex, qxl->vram, (1 << qxl->pci->size[1]));
    if (qxl->rom)
	xf86UnMapVidMem (scrnIndex, qxl->rom, (1 << qxl->pci->size[2]));
#endif
}

static void
map_memory_helper (qxl_screen_t *qxl)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range (qxl->pci, qxl->pci->regions[0].base_addr,
                          qxl->pci->regions[0].size,
                          PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
                          &qxl->ram);
    qxl->ram_physical = u64_to_pointer (qxl->pci->regions[0].base_addr);
    qxl->ram_size = qxl->pci->regions[0].size;
    
    pci_device_map_range (qxl->pci, qxl->pci->regions[1].base_addr,
                          qxl->pci->regions[1].size,
                          PCI_DEV_MAP_FLAG_WRITABLE,
                          &qxl->vram);
    qxl->vram_physical = u64_to_pointer (qxl->pci->regions[1].base_addr);
    qxl->vram_size = qxl->pci->regions[1].size;
    
    pci_device_map_range (qxl->pci, qxl->pci->regions[2].base_addr,
                          qxl->pci->regions[2].size, 0,
                          (void **)&qxl->rom);
    
    qxl->io_base = qxl->pci->regions[3].base_addr;
#else
    qxl->ram = xf86MapPciMem (scrnIndex, VIDMEM_FRAMEBUFFER,
                              qxl->pci_tag, qxl->pci->memBase[0],
                              (1 << qxl->pci->size[0]));
    qxl->ram_physical = (void *)qxl->pci->memBase[0];
    
    qxl->vram = xf86MapPciMem (scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
                               qxl->pci_tag, qxl->pci->memBase[1],
                               (1 << qxl->pci->size[1]));
    qxl->vram_physical = (void *)qxl->pci->memBase[1];
    qxl->vram_size = (1 << qxl->pci->size[1]);
    
    qxl->rom = xf86MapPciMem (scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
                              qxl->pci_tag, qxl->pci->memBase[2],
                              (1 << qxl->pci->size[2]));
    
    qxl->io_base = qxl->pci->ioBase[3];
#endif
}

#endif /* XSPICE */

static void
qxl_unmap_memory (qxl_screen_t *qxl)
{
#ifdef XSPICE
    if (qxl->worker)
    {
	qxl->worker->stop (qxl->worker);
	qxl->worker_running = FALSE;
    }
#endif
    
    if (qxl->mem)
    {
	qxl_mem_free_all (qxl->mem);
	qxl_drop_image_cache (qxl);
    }
    
    if (qxl->surf_mem)
	qxl_mem_free_all (qxl->surf_mem);
    
    unmap_memory_helper (qxl);
    qxl->ram = qxl->ram_physical = qxl->vram = qxl->rom = NULL;
    
    qxl->num_modes = 0;
    qxl->modes = NULL;
}

#ifdef QXLDRV_RESIZABLE_SURFACE0
static void
qxl_dump_ring_stat (qxl_screen_t *qxl)
{
    int cmd_prod, cursor_prod, cmd_cons, cursor_cons;
    int release_prod, release_cons;
    
    cmd_prod = qxl_ring_prod (qxl->command_ring);
    cursor_prod = qxl_ring_prod (qxl->cursor_ring);
    cmd_cons = qxl_ring_cons (qxl->command_ring);
    cursor_cons = qxl_ring_cons (qxl->cursor_ring);
    release_prod = qxl_ring_prod (qxl->release_ring);
    release_cons = qxl_ring_cons (qxl->release_ring);
    
    ErrorF ("%s: Cmd %d/%d, Cur %d/%d, Rel %d/%d\n",
            __func__, cmd_cons, cmd_prod, cursor_cons, cursor_prod,
            release_cons, release_prod);
}

#endif

/* To resize surface0 we need to ensure qxl->mem is empty. We can do that by:
 * - fast:
 *   - ooming until command ring is empty.
 *   - flushing the release ring (>V10)
 * - slow: calling update_area on all surfaces.
 * This is done via already known code, so use that by default now.
 */
static int
qxl_resize_surface0 (qxl_screen_t *qxl, long surface0_size)
{
    long ram_header_size = qxl->ram_size - qxl->rom->ram_header_offset;
    long new_mem_size = qxl->ram_size -
	(surface0_size + ram_header_size + qxl->monitors_config_size);
    
    if (new_mem_size < 0)
    {
	ErrorF ("cannot resize surface0 to %ld, does not fit in BAR 0\n",
	        surface0_size);
	return 0;
    }
    
    ErrorF ("resizing surface0 to %ld\n", surface0_size);
    
    if (qxl->mem)
    {
#ifdef QXLDRV_RESIZABLE_SURFACE0
	void *surfaces;
	qxl_dump_ring_stat (qxl);
	qxl_io_flush_surfaces (qxl);
	surfaces = qxl_surface_cache_evacuate_all (qxl->surface_cache);
	qxl_io_destroy_all_surfaces (qxl); // redundant?
	qxl_io_flush_release (qxl);
	qxl_drop_image_cache (qxl);
	qxl_dump_ring_stat (qxl);
	qxl_surface_cache_replace_all (qxl->surface_cache, surfaces);
#else
	ErrorF ("resizing surface0 compiled out\n");
	return 0;
#endif
    }
    
    /* surface0_area is still fixed to start of ram BAR */
    qxl->surface0_size = surface0_size;
    
    qxl->mem_size = new_mem_size;
    qxl->mem = qxl_mem_create ((void *)((unsigned long)qxl->surface0_area + qxl->surface0_size),
                               qxl->mem_size);
    return 1;
}

static Bool
qxl_map_memory (qxl_screen_t *qxl, int scrnIndex)
{
    map_memory_helper (qxl);
    
    if (!qxl->ram || !qxl->vram || !qxl->rom)
	return FALSE;
    
    xf86DrvMsg (scrnIndex, X_INFO, "framebuffer at %p (%d KB)\n",
                qxl->ram, qxl->rom->surface0_area_size / 1024);
    
    xf86DrvMsg (scrnIndex, X_INFO, "command ram at %p (%d KB)\n",
                (void *)((unsigned long)qxl->ram + qxl->rom->surface0_area_size),
                (qxl->rom->num_pages * getpagesize () - qxl->rom->surface0_area_size) / 1024);
    
    xf86DrvMsg (scrnIndex, X_INFO, "vram at %p (%ld KB)\n",
                qxl->vram, qxl->vram_size / 1024);
    
    xf86DrvMsg (scrnIndex, X_INFO, "rom at %p\n", qxl->rom);
    
    /*
     * Keep a hole for MonitorsConfig. This is not part of QXLRam to ensure
     * the driver can change it without affecting the driver/device ABI.
     */
    qxl->monitors_config_size = (sizeof (QXLMonitorsConfig) +
                                 sizeof (QXLHead) * MAX_MONITORS_NUM + getpagesize () - 1)
	& ~(getpagesize () - 1);
    qxl->num_modes = *(uint32_t *)((uint8_t *)qxl->rom + qxl->rom->modes_offset);
    qxl->modes = (struct QXLMode *)(((uint8_t *)qxl->rom) + qxl->rom->modes_offset + 4);
    qxl->surface0_area = qxl->ram;
    qxl->surface0_size = 0;
    qxl->mem = NULL;
    if (!qxl_resize_surface0 (qxl, qxl->rom->surface0_area_size))
	return FALSE;
    qxl->surf_mem = qxl_mem_create ((void *)((unsigned long)qxl->vram), qxl->vram_size);
    qxl_allocate_monitors_config (qxl);
    
    return TRUE;
}

#ifdef XSPICE
static void
qxl_save_state (ScrnInfoPtr pScrn)
{
}

static void
qxl_restore_state (ScrnInfoPtr pScrn)
{
}

#else /* QXL */
static void
qxl_save_state (ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    if (xf86IsPrimaryPci (qxl->pci))
	vgaHWSaveFonts (pScrn, &qxl->vgaRegs);
}

static void
qxl_restore_state (ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    if (xf86IsPrimaryPci (qxl->pci))
	vgaHWRestoreFonts (pScrn, &qxl->vgaRegs);
}

#endif /* XSPICE */

static Bool
qxl_close_screen (CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool result;
    
    ErrorF ("Disabling FB access for %d\n", pScrn->scrnIndex);
#ifndef XF86_SCRN_INTERFACE
    pScrn->EnableDisableFBAccess (scrnIndex, FALSE);
#else
    pScrn->EnableDisableFBAccess (pScrn, FALSE);
#endif
    
    pScreen->CreateScreenResources = qxl->create_screen_resources;
    pScreen->CloseScreen = qxl->close_screen;
    
    result = pScreen->CloseScreen (CLOSE_SCREEN_ARGS);
    
#ifndef XSPICE
    if (!xf86IsPrimaryPci (qxl->pci) && qxl->primary)
	qxl_reset_and_create_mem_slots (qxl);
#endif
    
    if (pScrn->vtSema)
    {
	qxl_restore_state (pScrn);
	qxl_mark_mem_unverifiable (qxl);
	qxl_unmap_memory (qxl);
    }
    pScrn->vtSema = FALSE;
    
    return result;
}

static void
set_screen_pixmap_header (ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap = pScreen->GetScreenPixmap (pScreen);
    
    // TODO: don't ModifyPixmapHeader too early?
    
    if (pPixmap)
    {
	pScreen->ModifyPixmapHeader (pPixmap,
	                             qxl->primary_mode.x_res, qxl->primary_mode.y_res,
	                             -1, -1,
	                             qxl->primary_mode.x_res * qxl->bytes_per_pixel,
	                             qxl_surface_get_host_bits(qxl->primary));
    }
    else
    {
	ErrorF ("pix: %p;\n", pPixmap);
    }
}

static qxl_surface_t *
qxl_create_primary(qxl_screen_t *qxl)
{
    struct QXLMode *pm = &qxl->primary_mode;
    pm->id = 0x4242;
    pm->x_res = qxl->virtual_x;
    pm->y_res = qxl->virtual_y;
    pm->bits = qxl->pScrn->bitsPerPixel;
    pm->stride = qxl->virtual_x * pm->bits / 8;
    pm->x_mili = 0; // TODO
    pm->y_mili = 0; // TODO
    pm->orientation = 0; // ? supported by us for single head usage? more TODO

    return qxl_surface_cache_create_primary (qxl, &qxl->primary_mode);
}

static Bool
qxl_resize_primary_to_virtual (qxl_screen_t *qxl)
{
    ScreenPtr pScreen;
    long new_surface0_size;

    if ((qxl->primary_mode.x_res == qxl->virtual_x &&
         qxl->primary_mode.y_res == qxl->virtual_y) &&
        qxl->device_primary == QXL_DEVICE_PRIMARY_CREATED)
    {
	return TRUE; /* empty Success */
    }
    
    ErrorF ("resizing primary to %dx%d\n", qxl->virtual_x, qxl->virtual_y);
    
    new_surface0_size =
        qxl->virtual_x * qxl->pScrn->bitsPerPixel / 8 * qxl->virtual_y;
    
    if (new_surface0_size > qxl->surface0_size)
    {
	if (!qxl_resize_surface0 (qxl, new_surface0_size))
	{
	    ErrorF ("not resizing primary to virtual, leaving old virtual\n");
	    return FALSE;
	}
    }
    
    if (qxl->primary)
    {
	qxl_surface_kill (qxl->primary);
	qxl_surface_cache_sanity_check (qxl->surface_cache);
	qxl_io_destroy_primary (qxl);
    }
    
    qxl->primary = qxl_create_primary(qxl);
    qxl->bytes_per_pixel = (qxl->pScrn->bitsPerPixel + 7) / 8;
    
    pScreen = qxl->pScrn->pScreen;
    if (pScreen)
    {
	PixmapPtr root = pScreen->GetScreenPixmap (pScreen);

#ifdef XSPICE
        if (qxl->deferred_fps <= 0)
#endif
        {
            qxl_surface_t *surf;

            if ((surf = get_surface (root)))
                qxl_surface_kill (surf);
            
            set_surface (root, qxl->primary);
        }

        set_screen_pixmap_header (pScreen);
    }
    
    ErrorF ("primary is %p\n", qxl->primary);
    return TRUE;
}

Bool
qxl_resize_primary (qxl_screen_t *qxl, uint32_t width, uint32_t height)
{
    qxl->virtual_x = width;
    qxl->virtual_y = height;
    
    if (qxl->vt_surfaces)
    {
	ErrorF ("%s: ignoring resize due to not being in control of VT\n",
	        __FUNCTION__);
	return FALSE;
    }
    return qxl_resize_primary_to_virtual (qxl);
}

static Bool
qxl_switch_mode (SWITCH_MODE_ARGS_DECL)
{
    SCRN_INFO_PTR (arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    ErrorF ("Ignoring display mode, ensuring recreation of primary\n");
    
    return qxl_resize_primary_to_virtual (qxl);
}

static Bool
qxl_create_screen_resources (ScreenPtr pScreen)
{
    ScrnInfoPtr    pScrn = xf86ScreenToScrn (pScreen);
    qxl_screen_t * qxl = pScrn->driverPrivate;
    Bool           ret;
    PixmapPtr      pPixmap;
    qxl_surface_t *surf;
    int            i;
    
    pScreen->CreateScreenResources = qxl->create_screen_resources;
    ret = pScreen->CreateScreenResources (pScreen);
    pScreen->CreateScreenResources = qxl_create_screen_resources;
    
    if (!ret)
	return FALSE;
    
    pPixmap = pScreen->GetScreenPixmap (pScreen);

#ifdef XSPICE
    if (qxl->deferred_fps <= 0)
#endif
    {
        set_screen_pixmap_header (pScreen);

        if ((surf = get_surface (pPixmap)))
            qxl_surface_kill (surf);

        set_surface (pPixmap, qxl->primary);
    }

    /* HACK - I don't want to enable any crtcs other then the first at the beginning */
    for (i = 1; i < qxl->num_heads; ++i)
    {
	qxl_output_private *private;

	qxl->crtcs[i]->enabled = 0;
	private = qxl->outputs[i]->driver_private;
	private->status = XF86OutputStatusDisconnected;
    }
    
    qxl_create_desired_modes (qxl);
    qxl_update_edid (qxl);
    
    return TRUE;
}

#ifdef XSPICE

static void
spiceqxl_screen_init (ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    // Init spice
    if (!qxl->spice_server)
    {
	qxl->spice_server = xspice_get_spice_server ();
	xspice_set_spice_server_options (qxl->options);
	qxl->core = basic_event_loop_init ();
	spice_server_init (qxl->spice_server, qxl->core);
	qxl_add_spice_display_interface (qxl);
	qxl->worker->start (qxl->worker);
	qxl->worker_running = TRUE;
        if (qxl->deferred_fps)
        {
            qxl->frames_timer = qxl->core->timer_add(dfps_ticker, qxl);
            qxl->core->timer_start(qxl->frames_timer, 1000 / qxl->deferred_fps);
        }
    }
    qxl->spice_server = qxl->spice_server;
}

#endif

static Bool
qxl_fb_init (qxl_screen_t *qxl, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = qxl->pScrn;
   
    if (!fbScreenInit (pScreen, qxl_surface_get_host_bits(qxl->primary),
                       pScrn->virtualX, pScrn->virtualY,
                       pScrn->xDpi, pScrn->yDpi, pScrn->virtualX,
                       pScrn->bitsPerPixel))
	return FALSE;
    
    fbPictureInit (pScreen, NULL, 0);
    return TRUE;
}

static Bool
qxl_screen_init (SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr    pScrn = xf86ScreenToScrn (pScreen);
    qxl_screen_t * qxl = pScrn->driverPrivate;
    struct QXLRam *ram_header;
    VisualPtr      visual;
    
    CHECK_POINT ();
    
    assert (qxl->pScrn == pScrn);
    
    if (!qxl_map_memory (qxl, pScrn->scrnIndex))
	return FALSE;
    
#ifdef XSPICE
    spiceqxl_screen_init (pScrn, qxl);
#endif
    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);
    
    printf ("ram_header at %d\n", qxl->rom->ram_header_offset);
    printf ("surf0 size: %d\n", qxl->rom->surface0_area_size);
    
    qxl_save_state (pScrn);
    qxl_blank_screen (pScreen, SCREEN_SAVER_ON);
    
    miClearVisualTypes ();
    if (!miSetVisualTypes (pScrn->depth, miGetDefaultVisualMask (pScrn->depth),
                           pScrn->rgbBits, pScrn->defaultVisual))
	goto out;
    if (!miSetPixmapDepths ())
	goto out;
    
#if 0
    ErrorF ("allocated %d x %d  %p\n", pScrn->virtualX, pScrn->virtualY, qxl->fb);
#endif

    /* Set up resources */
    qxl_reset_and_create_mem_slots (qxl);
    ErrorF ("done reset\n");

    qxl->surface_cache = qxl_surface_cache_create (qxl);
    qxl->primary = qxl_create_primary(qxl);
    
    if (!qxl_fb_init (qxl, pScreen))
	goto out;
    
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
    
    qxl->uxa = uxa_driver_alloc ();
    
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
    
    /* xf86DPMSInit (pScreen, xf86DPMSSet, 0); */
    
    pScreen->SaveScreen = qxl_blank_screen;
    
    qxl_uxa_init (qxl, pScreen);

#if 0
    uxa_set_fallback_debug(pScreen, TRUE);
#endif
    
    DamageSetup (pScreen);
    
    /* We need to set totalPixmapSize after setup_uxa and Damage,
       as the privates size is not computed correctly until then
     */
#if (XORG_VERSION_CURRENT < XORG_VERSION_NUMERIC(1, 12, 99, 901, 0))
    pScreen->totalPixmapSize = BitmapBytePad ((sizeof (PixmapRec) + dixPrivatesSize (PRIVATE_PIXMAP) ) * 8);
#else
    pScreen->totalPixmapSize = BitmapBytePad((sizeof(PixmapRec) +
			    dixScreenSpecificPrivatesSize(pScreen, PRIVATE_PIXMAP) ) * 8);
#endif

    miDCInitialize (pScreen, xf86GetPointerScreenFuncs());
    if (!miCreateDefColormap (pScreen))
        goto out;

    qxl->create_screen_resources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = qxl_create_screen_resources;
    
    qxl->close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = qxl_close_screen;
    
    qxl_cursor_init (pScreen);
    
    CHECK_POINT ();
    
    pScreen->width = pScrn->currentMode->HDisplay;
    pScreen->height = pScrn->currentMode->VDisplay;
    
    if (!xf86CrtcScreenInit (pScreen))
	return FALSE;
    
    if (!qxl_resize_primary_to_virtual (qxl))
	return FALSE;
    
    /* Note: this must be done after DamageSetup() because it calls
     * _dixInitPrivates. And if that has been called, DamageSetup()
     * will assert.
     */
    if (!uxa_resources_init (pScreen))
	return FALSE;
    CHECK_POINT ();
    
    /* fake transform support, to allow agent to switch crtc mode */
    /* without X doing checks, see rrcrtc.c "Check screen size */
    /* bounds" */
    xf86RandR12SetTransformSupport (pScreen, TRUE);
    
    return TRUE;
    
out:
    return FALSE;
}

static Bool
qxl_enter_vt (VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR (arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    qxl_save_state (pScrn);
    
    qxl_reset_and_create_mem_slots (qxl);
    
    if (!qxl_resize_primary_to_virtual (qxl))
	return FALSE;
    
    if (qxl->mem)
    {
	qxl_mem_free_all (qxl->mem);
	qxl_drop_image_cache (qxl);
    }
    
    if (qxl->surf_mem)
	qxl_mem_free_all (qxl->surf_mem);
    
    if (qxl->vt_surfaces)
    {
	qxl_surface_cache_replace_all (qxl->surface_cache, qxl->vt_surfaces);
	
	qxl->vt_surfaces = NULL;
    }
    
    qxl_create_desired_modes (qxl);
    
    pScrn->EnableDisableFBAccess (XF86_SCRN_ARG (pScrn), TRUE);
    
    return TRUE;
}

static void
qxl_leave_vt (VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR (arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    
    xf86_hide_cursors (pScrn);

    pScrn->EnableDisableFBAccess (XF86_SCRN_ARG (pScrn), FALSE);

#ifdef XSPICE
    if (qxl->deferred_fps <= 0)
#endif
        qxl->vt_surfaces = qxl_surface_cache_evacuate_all (qxl->surface_cache);

    ioport_write (qxl, QXL_IO_RESET, 0);
    
    qxl_restore_state (pScrn);
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
}

static Bool
qxl_color_setup (ScrnInfoPtr pScrn)
{
    int   scrnIndex = pScrn->scrnIndex;
    Gamma gzeros = { 0.0, 0.0, 0.0 };
    rgb   rzeros = { 0, 0, 0 };
    
    if (!xf86SetDepthBpp (pScrn, 0, 0, 0, Support32bppFb))
	return FALSE;
    
    if (pScrn->depth != 15 && pScrn->depth != 24)
    {
	xf86DrvMsg (scrnIndex, X_ERROR, "Depth %d is not supported\n",
	            pScrn->depth);
	return FALSE;
    }
    xf86PrintDepthBpp (pScrn);
    
    if (!xf86SetWeight (pScrn, rzeros, rzeros))
	return FALSE;
    
    if (!xf86SetDefaultVisual (pScrn, -1))
	return FALSE;
    
    if (!xf86SetGamma (pScrn, gzeros))
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
qxl_check_device (ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    int            scrnIndex = pScrn->scrnIndex;
    struct QXLRom *rom = qxl->rom;
    struct QXLRam *ram_header = (void *)((unsigned long)qxl->ram + rom->ram_header_offset);
    
    CHECK_POINT ();
    
    if (rom->magic != 0x4f525851)   /* "QXRO" little-endian */
    {
	xf86DrvMsg (scrnIndex, X_ERROR, "Bad ROM signature %x\n", rom->magic);
	return FALSE;
    }
    
    xf86DrvMsg (scrnIndex, X_INFO, "Device version %d.%d\n",
                rom->id, rom->update_id);
    
    xf86DrvMsg (scrnIndex, X_INFO, "Compression level %d, log level %d\n",
                rom->compression_level,
                rom->log_level);
    
    xf86DrvMsg (scrnIndex, X_INFO, "%d io pages at 0x%lx\n",
                rom->num_pages, (unsigned long)qxl->ram);
    
    xf86DrvMsg (scrnIndex, X_INFO, "RAM header offset: 0x%x\n", rom->ram_header_offset);
    
    if (ram_header->magic != 0x41525851)   /* "QXRA" little-endian */
    {
	xf86DrvMsg (scrnIndex, X_ERROR, "Bad RAM signature %x at %p\n",
	            ram_header->magic,
	            &ram_header->magic);
	return FALSE;
    }
    
    xf86DrvMsg (scrnIndex, X_INFO, "Correct RAM signature %x\n",
                ram_header->magic);
    return TRUE;
}

#endif /* !XSPICE */

static Bool
qxl_pre_init (ScrnInfoPtr pScrn, int flags)
{
    int           scrnIndex = pScrn->scrnIndex;
    qxl_screen_t *qxl = NULL;
    ClockRangePtr clockRanges = NULL;
    //int *linePitches = NULL;
    //DisplayModePtr mode;
    unsigned int max_x, max_y;
    
    /* In X server 1.7.5, Xorg -configure will cause this
     * function to get called without a confScreen.
     */
    if (!pScrn->confScreen)
	return FALSE;
    
    CHECK_POINT ();
    
    qxl_mem_init();
    
    /* zaphod mode is for suckers and i choose not to implement it */
    if (xf86IsEntityShared (pScrn->entityList[0]))
    {
	xf86DrvMsg (scrnIndex, X_ERROR, "No Zaphod mode for you\n");
	return FALSE;
    }
    
    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc (sizeof (qxl_screen_t), 1);
    
    qxl = pScrn->driverPrivate;
    memset (qxl, 0, sizeof (qxl));
    qxl->device_primary = QXL_DEVICE_PRIMARY_UNDEFINED;
    qxl->pScrn = pScrn;
    qxl->x_modes = NULL;
    qxl->entity = xf86GetEntityInfo (pScrn->entityList[0]);
    
#ifndef XSPICE
    qxl->pci = xf86GetPciInfoForEntity (qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    qxl->pci_tag = pciTag (qxl->pci->bus, qxl->pci->device, qxl->pci->func);
#endif
    if (qxl->pci->revision < 4)
    {
	ErrorF ("Ignoring monitor config, device revision < 4\n");
    }
#endif /* XSPICE */
    pScrn->monitor = pScrn->confScreen->monitor;
    
    if (!qxl_color_setup (pScrn))
	goto out;
    
    /* option parsing and card differentiation */
    xf86CollectOptions (pScrn, NULL);
    memcpy (qxl->options, DefaultOptions, sizeof (DefaultOptions));
    xf86ProcessOptions (scrnIndex, pScrn->options, qxl->options);
    
    qxl->enable_image_cache =
        xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_IMAGE_CACHE, TRUE);
    qxl->enable_fallback_cache =
        xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_FALLBACK_CACHE, TRUE);
    qxl->enable_surfaces =
        xf86ReturnOptValBool (qxl->options, OPTION_ENABLE_SURFACES, TRUE);
    qxl->num_heads =
        get_int_option (qxl->options, OPTION_NUM_HEADS, "QXL_NUM_HEADS");
    
#ifdef XSPICE
    qxl->deferred_fps = get_int_option(qxl->options, OPTION_SPICE_DEFERRED_FPS, "XSPICE_DEFERRED_FPS");
    if (qxl->deferred_fps > 0)
        xf86DrvMsg(scrnIndex, X_INFO, "Deferred FPS: %d\n", qxl->deferred_fps);
    else
        xf86DrvMsg(scrnIndex, X_INFO, "Deferred Frames: Disabled\n");
#endif

    xf86DrvMsg (scrnIndex, X_INFO, "Offscreen Surfaces: %s\n",
                qxl->enable_surfaces ? "Enabled" : "Disabled");
    xf86DrvMsg (scrnIndex, X_INFO, "Image Cache: %s\n",
                qxl->enable_image_cache ? "Enabled" : "Disabled");
    xf86DrvMsg (scrnIndex, X_INFO, "Fallback Cache: %s\n",
                qxl->enable_fallback_cache ? "Enabled" : "Disabled");
    
    if (!qxl_map_memory (qxl, scrnIndex))
	goto out;
    
#ifndef XSPICE
    if (!qxl_check_device (pScrn, qxl))
	goto out;
#else
    xspice_init_qxl_ram (qxl); /* initialize the rings */
#endif

#define DIV_ROUND_UP(n, a) (((n) + (a) - 1) / (a))
#define BYTES_TO_KB(bytes) DIV_ROUND_UP(bytes, 1024)
#define PAGES_TO_KB(pages) ((pages) * getpagesize() / 1024)

    pScrn->videoRam = PAGES_TO_KB(qxl->rom->num_pages)
                      - BYTES_TO_KB(qxl->monitors_config_size);
    xf86DrvMsg (scrnIndex, X_INFO, "%d KB of video RAM\n", pScrn->videoRam);
    xf86DrvMsg (scrnIndex, X_INFO, "%d surfaces\n", qxl->rom->n_surfaces);
    
    /* ddc stuff here */
    
    clockRanges = xnfcalloc (sizeof (ClockRange), 1);
    clockRanges->next = NULL;
    clockRanges->minClock = 10000;
    clockRanges->maxClock = 400000;
    clockRanges->clockIndex = -1;
    clockRanges->interlaceAllowed = clockRanges->doubleScanAllowed = 0;
    clockRanges->ClockMulFactor = clockRanges->ClockDivFactor = 1;
    pScrn->progClock = TRUE;
    
    /* override QXL monitor stuff */
    if (pScrn->monitor->nHsync <= 0)
    {
	pScrn->monitor->hsync[0].lo =  29.0;
	pScrn->monitor->hsync[0].hi = 160.0;
	pScrn->monitor->nHsync = 1;
    }
    if (pScrn->monitor->nVrefresh <= 0)
    {
	pScrn->monitor->vrefresh[0].lo = 50;
	pScrn->monitor->vrefresh[0].hi = 75;
	pScrn->monitor->nVrefresh = 1;
    }

    qxl_initialize_x_modes (qxl, pScrn, &max_x, &max_y);
    
    CHECK_POINT ();
    
    xf86PruneDriverModes (pScrn);
    
    qxl_init_randr (pScrn, qxl);

    xf86SetDpi (pScrn, 0, 0);
    
    if (!xf86LoadSubModule (pScrn, "fb")
#ifndef XSPICE
        || !xf86LoadSubModule (pScrn, "ramdac")
        || !xf86LoadSubModule (pScrn, "vgahw")
#endif
        )
    {
	goto out;
    }
    
    print_modes (qxl, scrnIndex);
    
#ifndef XSPICE
    /* VGA hardware initialisation */
    if (!vgaHWGetHWRec (pScrn))
	return FALSE;
    vgaHWSetStdFuncs (VGAHWPTR (pScrn));
#endif
    
    /* hate */
    qxl_unmap_memory (qxl);
    
    CHECK_POINT ();
    
    xf86DrvMsg (scrnIndex, X_INFO, "PreInit complete\n");
#ifdef GIT_VERSION
    xf86DrvMsg (scrnIndex, X_INFO, "git commit %s\n", GIT_VERSION);
#endif
    return TRUE;
    
out:
    if (clockRanges)
	free (clockRanges);
    if (qxl)
	free (qxl);
    
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

static SymTabRec qxlChips[] = {
    { PCI_CHIP_QXL_0100, "QXL 1", },
    { -1, NULL }
};

#ifndef XSERVER_LIBPCIACCESS
static PciChipsets qxlPciChips[] = {
    { PCI_CHIP_QXL_0100, PCI_CHIP_QXL_0100, RES_SHARED_VGA },
    { -1, -1, RES_UNDEFINED }
};
#endif
#endif /* !XSPICE */

static void
qxl_identify (int flags)
{
#ifndef XSPICE
    xf86PrintChipsets ("qxl", "Driver for QXL virtual graphics", qxlChips);
#endif
}

static void
qxl_init_scrn (ScrnInfoPtr pScrn)
{
    pScrn->driverVersion    = 0;
    pScrn->driverName       = QXL_DRIVER_NAME;
    pScrn->name             = QXL_DRIVER_NAME;
    pScrn->PreInit          = qxl_pre_init;
    pScrn->ScreenInit       = qxl_screen_init;
    pScrn->SwitchMode       = qxl_switch_mode;
    pScrn->ValidMode        = NULL,
	pScrn->EnterVT          = qxl_enter_vt;
    pScrn->LeaveVT          = qxl_leave_vt;
}

#ifdef XSPICE
static Bool
qxl_probe (struct _DriverRec *drv, int flags)
{
    ScrnInfoPtr   pScrn;
    int           entityIndex;
    EntityInfoPtr pEnt;
    GDevPtr*      device;
    
    if (flags & PROBE_DETECT)
	return TRUE;
    
    pScrn = xf86AllocateScreen (drv, flags);
    qxl_init_scrn (pScrn);
    
    xf86MatchDevice (QXL_DRIVER_NAME, &device);
    entityIndex = xf86ClaimNoSlot (drv, 0, device[0], TRUE);
    pEnt = xf86GetEntityInfo (entityIndex);
    pEnt->driver = drv;
    
    xf86AddEntityToScreen (pScrn, entityIndex);
    
    return TRUE;
}

static Bool qxl_driver_func (ScrnInfoPtr screen_info_ptr, xorgDriverFuncOp xorg_driver_func_op, pointer hw_flags)
{
    *(xorgHWFlags*)hw_flags = (xorgHWFlags)HW_SKIP_CONSOLE;
    return TRUE;
}

#else /* normal, not XSPICE */
#ifndef XSERVER_LIBPCIACCESS
static Bool
qxl_probe (DriverPtr drv, int flags)
{
    int      i, numUsed;
    int      numDevSections;
    int *    usedChips;
    GDevPtr *devSections;
    
    if ((numDevSections = xf86MatchDevice (QXL_NAME, &devSections)) <= 0)
	return FALSE;
    
    if (!xf86GetPciVideoInfo ())
	return FALSE;
    
    numUsed = xf86MatchPciInstances (QXL_NAME, PCI_VENDOR_RED_HAT,
                                     qxlChips, qxlPciChips,
                                     devSections, numDevSections,
                                     drv, &usedChips);
    
    xfree (devSections);
    
    if (numUsed < 0)
    {
	xfree (usedChips);
	return FALSE;
    }
    
    if (flags & PROBE_DETECT)
    {
	xfree (usedChips);
	return TRUE;
    }
    
    for (i = 0; i < numUsed; i++)
    {
	ScrnInfoPtr pScrn = NULL;
	if ((pScrn = xf86ConfigPciEntity (pScrn, 0, usedChips[i], qxlPciChips,
	                                  0, 0, 0, 0, 0)))
	    qxl_init_scrn (pScrn);
    }
    
    xfree (usedChips);
    return TRUE;
}

#else /* pciaccess */

static Bool
qxl_pci_probe (DriverPtr drv, int entity, struct pci_device *dev, intptr_t match)
{
    qxl_screen_t *qxl;
    ScrnInfoPtr   pScrn = xf86ConfigPciEntity (NULL, 0, entity, NULL, NULL,
                                               NULL, NULL, NULL, NULL);
    
    if (!pScrn)
	return FALSE;
    
    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc (sizeof (qxl_screen_t), 1);
    qxl = pScrn->driverPrivate;
    qxl->pci = dev;
    
    qxl_init_scrn (pScrn);
    
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
qxl_setup (pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool loaded = FALSE;
    
    if (!loaded)
    {
	loaded = TRUE;
	xf86AddDriver (&qxl_driver, module, HaveDriverFuncs);
#ifdef XSPICE
	xspice_add_input_drivers (module);
#endif
	return (void *)1;
    }
    else
    {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	
	return NULL;
    }
}

static XF86ModuleVersionInfo qxl_module_info =
{
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
