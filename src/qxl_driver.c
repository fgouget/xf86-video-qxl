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

#include "mspace.h"

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

#ifdef WITH_CHECK_POINT
#define CHECK_POINT() ErrorF("%s: %d  (%s)\n", __FILE__, __LINE__, __FUNCTION__);
#else
#define CHECK_POINT()
#endif

#define BREAKPOINT()   do{ __asm__ __volatile__ ("int $03"); } while(0)

const OptionInfoRec DefaultOptions[] = {
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
qxl_available_options(int chipid, int busid)
{
    return DefaultOptions;
}

#ifndef XSPICE
static void qxl_wait_for_io_command(qxl_screen_t *qxl)
{
    struct QXLRam *ram_header;

    ram_header = (void *)((unsigned long)qxl->ram + qxl->rom->ram_header_offset);

    while (!(ram_header->int_pending & QXL_INTERRUPT_IO_CMD))
        usleep(1);

    ram_header->int_pending &= ~QXL_INTERRUPT_IO_CMD;
}

#if 0
static void qxl_wait_for_display_interrupt(qxl_screen_t *qxl)
{
    struct QXLRam *ram_header;

    ram_header = (void *)((unsigned long)qxl->ram + qxl->rom->ram_header_offset);

    while (!(ram_header->int_pending & QXL_INTERRUPT_DISPLAY))
        usleep(1);

    ram_header->int_pending &= ~QXL_INTERRUPT_DISPLAY;
}
#endif
#endif

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

void qxl_io_memslot_add(qxl_screen_t *qxl, uint8_t id)
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

void qxl_io_create_primary(qxl_screen_t *qxl)
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
    qxl->device_primary = QXL_DEVICE_PRIMARY_CREATED;
}

void qxl_io_destroy_primary(qxl_screen_t *qxl)
{
#ifndef XSPICE
    if (qxl->pci->revision >= 3) {
        ioport_write(qxl, QXL_IO_DESTROY_PRIMARY_ASYNC, 0);
        qxl_wait_for_io_command(qxl);
    } else {
        ioport_write(qxl, QXL_IO_DESTROY_PRIMARY, 0);
    }
#else
    ioport_write(qxl, QXL_IO_DESTROY_PRIMARY, 0);
#endif
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
}

void qxl_io_notify_oom(qxl_screen_t *qxl)
{
    ioport_write(qxl, QXL_IO_NOTIFY_OOM, 0);
}

void qxl_io_flush_surfaces(qxl_screen_t *qxl)
{
    // FIXME: write individual update_area for revision < V10
#ifndef XSPICE
    ioport_write(qxl, QXL_IO_FLUSH_SURFACES_ASYNC, 0);
    qxl_wait_for_io_command(qxl);
#else
    ioport_write(qxl, QXL_IO_FLUSH_SURFACES_ASYNC, 0);
#endif
}

static void
qxl_usleep(int useconds)
{
    struct timespec t;

    t.tv_sec = useconds / 1000000;
    t.tv_nsec = (useconds - (t.tv_sec * 1000000)) * 1000;

    errno = 0;
    while (nanosleep (&t, &t) == -1 && errno == EINTR)
        ;
}

#ifdef QXLDRV_RESIZABLE_SURFACE0
static void qxl_io_flush_release(qxl_screen_t *qxl)
{
#ifndef XSPICE
    int sum = 0;

    sum += qxl_garbage_collect(qxl);
    ioport_write(qxl, QXL_IO_FLUSH_RELEASE, 0);
    sum +=  qxl_garbage_collect(qxl);
    ErrorF("%s: collected %d\n", __func__, sum);
#else
#endif
}
#endif

static void qxl_io_monitors_config_async(qxl_screen_t *qxl)
{
#ifndef XSPICE
    if (qxl->pci->revision < 4) {
        return;
    }

    ioport_write(qxl, QXL_IO_MONITORS_CONFIG_ASYNC, 0);
    qxl_wait_for_io_command(qxl);
#else
    fprintf(stderr, "UNIMPLEMENTED!\n");
#endif
}

/* Having a single monitors config struct allocated on the device avoids any
 *
 * possible fragmentation. Since X is single threaded there is no danger
 * in us changing it between issuing the io and getting the interrupt to signal
 * spice-server is done reading it. */
#define MAX_MONITORS_NUM 16

static void
qxl_allocate_monitors_config(qxl_screen_t *qxl)
{
    int size = sizeof(QXLMonitorsConfig) + sizeof(QXLHead) * MAX_MONITORS_NUM;

    if (qxl->monitors_config)
        return;

    qxl->monitors_config = (QXLMonitorsConfig *)(void *)
        ((unsigned long)qxl->ram + qxl->rom->ram_header_offset - qxl->monitors_config_size);

    memset(qxl->monitors_config, 0, size);
}

static uint64_t
qxl_garbage_collect_internal(qxl_screen_t *qxl, uint64_t id)
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

    if (is_cursor && cmd->type == QXL_CURSOR_SET) {
        struct QXLCursor *cursor;

        cursor = (void *)virtual_address(qxl, u64_to_pointer (cmd->u.set.shape),
                                         qxl->main_mem_slot);
        qxl_free(qxl->mem, cursor);
    } else if (is_drawable && drawable->type == QXL_DRAW_COPY) {
        struct QXLImage *image;

        image = virtual_address(qxl, u64_to_pointer (drawable->u.copy.src_bitmap),
                                qxl->main_mem_slot);
        if (image->descriptor.type == SPICE_IMAGE_TYPE_SURFACE){
            qxl_surface_unref(qxl->surface_cache, image->surface_image.surface_id);
            qxl_surface_cache_sanity_check(qxl->surface_cache);
            qxl_free(qxl->mem, image);
        } else {
            qxl_image_destroy(qxl, image);
        }
    } else if (is_surface && surface_cmd->type == QXL_SURFACE_CMD_DESTROY) {
        qxl_surface_recycle(qxl->surface_cache, surface_cmd->surface_id);
        qxl_surface_cache_sanity_check(qxl->surface_cache);
    }

    id = info->next;
    qxl_free(qxl->mem, info);

    return id;
}

int
qxl_garbage_collect(qxl_screen_t *qxl)
{
    uint64_t id;
    int i = 0;

    while (qxl_ring_pop (qxl->release_ring, &id))
        while (id) {
            id = qxl_garbage_collect_internal(qxl, id);
            i++;
        }

    return i;
}

int
qxl_handle_oom(qxl_screen_t *qxl)
{
    qxl_io_notify_oom(qxl);

#if 0
    ErrorF (".");
    qxl_usleep (10000);
#endif

    if (!(qxl_garbage_collect(qxl)))
        qxl_usleep(10000);

    return qxl_garbage_collect(qxl);
}

void *
qxl_allocnf(qxl_screen_t *qxl, unsigned long size)
{
    void *result;
    int n_attempts = 0;
#if 0
    static int nth_oom = 1;
#endif

    qxl_garbage_collect(qxl);

    while (!(result = qxl_alloc(qxl->mem, size))) {
#if 0
        ErrorF("eliminated memory (%d)\n", nth_oom++);
#endif

        if (!qxl_garbage_collect(qxl)) {
            if (qxl_handle_oom(qxl)) {
                n_attempts = 0;
            } else if (++n_attempts == 1000) {
                ErrorF("Out of memory allocating %ld bytes\n", size);
                qxl_mem_dump_stats(qxl->mem, "Out of mem - stats\n");
                fprintf(stderr, "Out of memory\n");
                exit(1);
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
unmap_memory_helper(qxl_screen_t *qxl)
{
    free(qxl->ram);
    free(qxl->vram);
    free(qxl->rom);
}

static void
map_memory_helper(qxl_screen_t *qxl)
{
    qxl->ram = malloc(RAM_SIZE);
    qxl->ram_size = RAM_SIZE;
    qxl->ram_physical = qxl->ram;
    qxl->vram = malloc(VRAM_SIZE);
    qxl->vram_size = VRAM_SIZE;
    qxl->vram_physical = qxl->vram;
    qxl->rom = malloc(ROM_SIZE);

    init_qxl_rom(qxl, ROM_SIZE);
}
#else /* Default */
static void
unmap_memory_helper(qxl_screen_t *qxl)
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
map_memory_helper(qxl_screen_t *qxl)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range(qxl->pci, qxl->pci->regions[0].base_addr,
                         qxl->pci->regions[0].size,
                         PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
                         &qxl->ram);
    qxl->ram_physical = u64_to_pointer (qxl->pci->regions[0].base_addr);
    qxl->ram_size = qxl->pci->regions[0].size;

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
qxl_unmap_memory(qxl_screen_t *qxl)
{
#ifdef XSPICE
    if (qxl->worker) {
        qxl->worker->stop(qxl->worker);
        qxl->worker_running = FALSE;
    }
#endif
    if (qxl->mem) {
        qxl_mem_free_all(qxl->mem);
        qxl_drop_image_cache (qxl);
    }
    if (qxl->surf_mem)
        qxl_mem_free_all(qxl->surf_mem);

    unmap_memory_helper(qxl);
    qxl->ram = qxl->ram_physical = qxl->vram = qxl->rom = NULL;

    qxl->num_modes = 0;
    qxl->modes = NULL;
}

static void __attribute__ ((__noreturn__))
qxl_mspace_abort_func(void *user_data)
{
    abort();
}

static void __attribute__((format(gnu_printf, 2, 3)))
qxl_mspace_print_func(void *user_data, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    VErrorF(format, args);
    va_end(args);
}

#ifdef QXLDRV_RESIZABLE_SURFACE0
static void
qxl_dump_ring_stat(qxl_screen_t *qxl)
{
    int cmd_prod, cursor_prod, cmd_cons, cursor_cons;
    int release_prod, release_cons;

    cmd_prod = qxl_ring_prod(qxl->command_ring);
    cursor_prod = qxl_ring_prod(qxl->cursor_ring);
    cmd_cons = qxl_ring_cons(qxl->command_ring);
    cursor_cons = qxl_ring_cons(qxl->cursor_ring);
    release_prod = qxl_ring_prod(qxl->release_ring);
    release_cons = qxl_ring_cons(qxl->release_ring);

    ErrorF("%s: Cmd %d/%d, Cur %d/%d, Rel %d/%d\n",
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
qxl_resize_surface0(qxl_screen_t *qxl, long surface0_size)
{
    long ram_header_size = qxl->ram_size - qxl->rom->ram_header_offset;
    long new_mem_size = qxl->ram_size -
        (surface0_size + ram_header_size + qxl->monitors_config_size);

    if (new_mem_size < 0) {
        ErrorF("cannot resize surface0 to %ld, does not fit in BAR 0\n",
                surface0_size);
        return 0;
    }

    ErrorF("resizing surface0 to %ld\n", surface0_size);

    if (qxl->mem) {
#ifdef QXLDRV_RESIZABLE_SURFACE0
        void *surfaces;
        qxl_dump_ring_stat(qxl);
        qxl_io_flush_surfaces(qxl);
        surfaces = qxl_surface_cache_evacuate_all(qxl->surface_cache);
        qxl_io_destroy_all_surfaces(qxl); // redundant?
        qxl_io_flush_release(qxl);
        qxl_drop_image_cache(qxl);
        qxl_dump_ring_stat(qxl);
        qxl_surface_cache_replace_all(qxl->surface_cache, surfaces);
#else
        ErrorF("resizing surface0 compiled out\n");
        return 0;
#endif
    }

    /* surface0_area is still fixed to start of ram BAR */
    qxl->surface0_size = surface0_size;

    qxl->mem_size = new_mem_size;
    qxl->mem = qxl_mem_create((void *)((unsigned long)qxl->surface0_area + qxl->surface0_size),
                              qxl->mem_size);
    return 1;
}

static Bool
qxl_map_memory(qxl_screen_t *qxl, int scrnIndex)
{
    map_memory_helper(qxl);

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

    /*
     * Keep a hole for MonitorsConfig. This is not part of QXLRam to ensure
     * the driver can change it without affecting the driver/device ABI.
     */
    qxl->monitors_config_size = (sizeof(QXLMonitorsConfig) +
                                 sizeof(QXLHead) * MAX_MONITORS_NUM + getpagesize() - 1)
        & ~(getpagesize() - 1);
    qxl->num_modes = *(uint32_t *)((uint8_t *)qxl->rom + qxl->rom->modes_offset);
    qxl->modes = (struct QXLMode *)(((uint8_t *)qxl->rom) + qxl->rom->modes_offset + 4);
    qxl->surface0_area = qxl->ram;
    qxl->surface0_size = 0;
    qxl->mem = NULL;
    if (!qxl_resize_surface0(qxl, qxl->rom->surface0_area_size))
        return FALSE;
    qxl->surf_mem = qxl_mem_create((void *)((unsigned long)qxl->vram), qxl->vram_size);
    qxl_allocate_monitors_config(qxl);

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

    if (xf86IsPrimaryPci(qxl->pci))
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

    qxl_io_memslot_add(qxl, slot_index);

    slot->generation = qxl->rom->slot_generation;

    high_bits = slot_index << qxl->slot_gen_bits;
    high_bits |= slot->generation;
    high_bits <<= (64 - (qxl->slot_gen_bits + qxl->slot_id_bits));
    slot->high_bits = high_bits;

    return slot_index;
}

static void
qxl_reset_and_create_mem_slots (qxl_screen_t *qxl)
{
    ioport_write(qxl, QXL_IO_RESET, 0);
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
    /* Mem slots */
    ErrorF("slots start: %d, slots end: %d\n",
           qxl->rom->slots_start,
           qxl->rom->slots_end);

    /* Main slot */
    qxl->n_mem_slots = qxl->rom->slots_end;
    qxl->slot_gen_bits = qxl->rom->slot_gen_bits;
    qxl->slot_id_bits = qxl->rom->slot_id_bits;
    qxl->va_slot_mask = (~(uint64_t)0) >> (qxl->slot_id_bits + qxl->slot_gen_bits);

    qxl->mem_slots = xnfalloc(qxl->n_mem_slots * sizeof (qxl_memslot_t));

#ifdef XSPICE
    qxl->main_mem_slot = qxl->vram_mem_slot = setup_slot(qxl, 0, 0, ~0, 0, ~0);
#else /* QXL */
    qxl->main_mem_slot = setup_slot(qxl, 0,
                                    (unsigned long)qxl->ram_physical,
                                    (unsigned long)qxl->ram_physical + qxl->surface0_size +
                                    (unsigned long)qxl->rom->num_pages * getpagesize(),
                                    (uint64_t)(uintptr_t)qxl->ram,
                                    (uint64_t)(uintptr_t)qxl->ram + qxl->surface0_size +
                                    (unsigned long)qxl->rom->num_pages * getpagesize()
                                    );
    qxl->vram_mem_slot = setup_slot(qxl, 1,
                                    (unsigned long)qxl->vram_physical,
                                    (unsigned long)qxl->vram_physical + (unsigned long)qxl->vram_size,
                                    (uint64_t)(uintptr_t)qxl->vram,
                                    (uint64_t)(uintptr_t)qxl->vram + (uint64_t)qxl->vram_size);
#endif
}

static void
qxl_mark_mem_unverifiable(qxl_screen_t *qxl)
{
    qxl_mem_unverifiable(qxl->mem);
    qxl_mem_unverifiable(qxl->surf_mem);
}

void
qxl_io_destroy_all_surfaces(qxl_screen_t *qxl)
{
#ifndef XSPICE
    if (qxl->pci->revision >= 3) {
        ioport_write(qxl, QXL_IO_DESTROY_ALL_SURFACES_ASYNC, 0);
        qxl_wait_for_io_command(qxl);
    } else {
        ioport_write(qxl, QXL_IO_DESTROY_ALL_SURFACES, 0);
    }
#else
    ErrorF("Xspice: error: UNIMPLEMENTED qxl_io_destroy_all_surfaces\n");
#endif
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
}

static Bool
qxl_close_screen(CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool result;

    ErrorF("Disabling FB access for %d\n", pScrn->scrnIndex);
#ifndef XF86_SCRN_INTERFACE
    pScrn->EnableDisableFBAccess(scrnIndex, FALSE);
#else
    pScrn->EnableDisableFBAccess(pScrn, FALSE);
#endif
    ErrorF("Freeing %p\n", qxl->fb);
    free(qxl->fb);
    qxl->fb = NULL;

    pScreen->CreateScreenResources = qxl->create_screen_resources;
    pScreen->CloseScreen = qxl->close_screen;

    result = pScreen->CloseScreen(CLOSE_SCREEN_ARGS);

#ifndef XSPICE
    if (!xf86IsPrimaryPci(qxl->pci) && qxl->primary)
        qxl_reset_and_create_mem_slots(qxl);
#endif

    if (pScrn->vtSema) {
        qxl_restore_state(pScrn);
        qxl_mark_mem_unverifiable(qxl);
        qxl_unmap_memory(qxl);
    }
    pScrn->vtSema = FALSE;

    return result;
}

static void
set_screen_pixmap_header(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap = pScreen->GetScreenPixmap(pScreen);

    // TODO: don't ModifyPixmapHeader too early?

    if (pPixmap) {
        ErrorF("new stride: %d (display width: %d, bpp: %d)\n",
               qxl->pScrn->displayWidth * qxl->bytes_per_pixel,
               qxl->pScrn->displayWidth, qxl->bytes_per_pixel);

        pScreen->ModifyPixmapHeader(pPixmap,
                                    qxl->primary_mode.x_res, qxl->primary_mode.y_res,
                                    -1, -1,
                                    qxl->pScrn->displayWidth * qxl->bytes_per_pixel,
                                    NULL);
    } else
        ErrorF("pix: %p;\n", pPixmap);
}

static Bool
qxl_resize_primary_to_virtual(qxl_screen_t *qxl)
{
    ScreenPtr pScreen;
    long new_surface0_size;

    if ((qxl->primary_mode.x_res == qxl->virtual_x &&
         qxl->primary_mode.y_res == qxl->virtual_y) &&
        qxl->device_primary == QXL_DEVICE_PRIMARY_CREATED) {
        return TRUE; /* empty Success */
    }

    ErrorF("resizing primary to %dx%d\n", qxl->virtual_x, qxl->virtual_y);

    new_surface0_size =
        qxl->virtual_x * qxl->pScrn->bitsPerPixel / 8 * qxl->virtual_y;

    if (new_surface0_size > qxl->surface0_size) {
        if (!qxl_resize_surface0(qxl, new_surface0_size)) {
            ErrorF("not resizing primary to virtual, leaving old virtual\n");
            return FALSE;
        }
    }

    if (qxl->primary) {
        qxl_surface_kill(qxl->primary);
        qxl_surface_cache_sanity_check(qxl->surface_cache);
        qxl_io_destroy_primary(qxl);
    }

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
    }
    qxl->primary = qxl_surface_cache_create_primary(qxl->surface_cache, &qxl->primary_mode);
    qxl->bytes_per_pixel = (qxl->pScrn->bitsPerPixel + 7) / 8;

    pScreen = qxl->pScrn->pScreen;
    if (pScreen) {
        PixmapPtr root = pScreen->GetScreenPixmap(pScreen);
        qxl_surface_t *surf;

        if ((surf = get_surface(root)))
            qxl_surface_kill(surf);

        set_surface(root, qxl->primary);
    }

    ErrorF("primary is %p\n", qxl->primary);
    return TRUE;
}

static Bool
qxl_resize_primary(qxl_screen_t *qxl, uint32_t width, uint32_t height)
{
    qxl->virtual_x = width;
    qxl->virtual_y = height;

    if (qxl->vt_surfaces) {
        ErrorF("%s: ignoring resize due to not being in control of VT\n",
               __FUNCTION__);
        return FALSE;
    }
    return qxl_resize_primary_to_virtual(qxl);
}

static Bool
qxl_switch_mode(SWITCH_MODE_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;

    ErrorF("Ignoring display mode, ensuring recreation of primary\n");

    return qxl_resize_primary_to_virtual(qxl);
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

static int
check_crtc(qxl_screen_t *qxl)
{
    int i, count = 0;
    xf86CrtcPtr crtc;

    for (i = 0 ; i < qxl->num_heads; ++i) {
        crtc = qxl->crtcs[i];
        if (!crtc->enabled || crtc->mode.CrtcHDisplay == 0 ||
            crtc->mode.CrtcVDisplay == 0)
            continue;
        count++;
    }

#if 0
    if (count == 0) {
        ErrorF("check crtc failed, count == 0!!\n");
        BREAKPOINT();
    }
#endif

    return count;
}

static void
qxl_update_monitors_config(qxl_screen_t *qxl)
{
    int i;
    QXLHead *head;
    xf86CrtcPtr crtc;
    QXLRam *ram = get_ram_header(qxl);

    check_crtc(qxl);

    qxl->monitors_config->count = 0;
    qxl->monitors_config->max_allowed = qxl->num_heads;
    for (i = 0 ; i < qxl->num_heads; ++i) {
        head = &qxl->monitors_config->heads[qxl->monitors_config->count];
        crtc = qxl->crtcs[i];
        head->id = i;
        head->surface_id = 0;
        head->flags = 0;
        if (!crtc->enabled || crtc->mode.CrtcHDisplay == 0 ||
            crtc->mode.CrtcVDisplay == 0) {
            head->width = head->height = head->x = head->y = 0;
        } else {
            head->width = crtc->mode.CrtcHDisplay;
            head->height = crtc->mode.CrtcVDisplay;
            head->x = crtc->x;
            head->y = crtc->y;
            qxl->monitors_config->count++;
        }
    }
    /* initialize when actually used, memslots should be initialized by now */
    if (ram->monitors_config == 0)
        ram->monitors_config = physical_address(qxl, qxl->monitors_config,
                                                qxl->main_mem_slot);

    qxl_io_monitors_config_async(qxl);
}

static Bool
crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
                    Rotation rotation, int x, int y)
{
    qxl_crtc_private *crtc_private = crtc->driver_private;
    qxl_screen_t *qxl = crtc_private->qxl;

    if (crtc == qxl->crtcs[0] && mode == NULL) {
        /* disallow disabling of monitor 0 mode */
        ErrorF("%s: not allowing crtc 0 disablement\n", __func__);
        return FALSE;
    }

    crtc->mode = *mode;
    crtc->x = x;
    crtc->y = y;
    crtc->rotation = rotation;
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,5,99,0,0)
    crtc->transformPresent = FALSE;
#endif
    qxl_output_edid_set(crtc_private->output, crtc_private->head, mode);

    return TRUE;
}

static Bool
qxl_create_desired_modes(qxl_screen_t *qxl)
{
    int i;
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(qxl->pScrn);
    CHECK_POINT();

    for (i = 0 ; i < config->num_crtc; ++i) {
        xf86CrtcPtr crtc = config->crtc[i];
        if (!crtc->enabled)
            continue;

        if (!crtc_set_mode_major(crtc, &crtc->desiredMode, crtc->desiredRotation,
                                 crtc->desiredX, crtc->desiredY))
            return FALSE;
    }

    qxl_update_monitors_config(qxl);
    return TRUE;
}

static void
qxl_update_edid(qxl_screen_t *qxl)
{
    int i;
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(qxl->pScrn);

    for (i = 0 ; i < config->num_crtc; ++i) {
        xf86CrtcPtr crtc = config->crtc[i];
        if (!crtc->enabled)
            continue;

        qxl_output_edid_set(qxl->outputs[i], i, &crtc->desiredMode);
    }
}

static Bool
qxl_create_screen_resources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool ret;
    PixmapPtr pPixmap;
    qxl_surface_t *surf;
    int i;

    pScreen->CreateScreenResources = qxl->create_screen_resources;
    ret = pScreen->CreateScreenResources (pScreen);
    pScreen->CreateScreenResources = qxl_create_screen_resources;

    if (!ret)
        return FALSE;

    pPixmap = pScreen->GetScreenPixmap (pScreen);

    set_screen_pixmap_header (pScreen);

    if ((surf = get_surface (pPixmap)))
        qxl_surface_kill (surf);

    set_surface(pPixmap, qxl->primary);

    /* HACK - I don't want to enable any crtcs other then the first at the beginning */
    for (i = 1; i < qxl->num_heads; ++i) {
        qxl->crtcs[i]->enabled = 0;
    }

    qxl_create_desired_modes(qxl);
    qxl_update_edid(qxl);

    return TRUE;
}

#if HAS_DEVPRIVATEKEYREC
DevPrivateKeyRec uxa_pixmap_index;
#else
int uxa_pixmap_index;
#endif

static Bool
unaccel(void)
{
    return FALSE;
}

static Bool
qxl_prepare_access(PixmapPtr pixmap, RegionPtr region, uxa_access_t access)
{
    return qxl_surface_prepare_access(get_surface(pixmap),
                                      pixmap, region, access);
}

static void
qxl_finish_access(PixmapPtr pixmap)
{
    qxl_surface_finish_access(get_surface(pixmap), pixmap);
}

static Bool
qxl_pixmap_is_offscreen(PixmapPtr pixmap)
{
    return !!get_surface(pixmap);
}


static Bool
good_alu_and_pm(DrawablePtr drawable, int alu, Pixel planemask)
{
    if (!UXA_PM_IS_SOLID(drawable, planemask))
        return FALSE;

    if (alu != GXcopy)
        return FALSE;

    return TRUE;
}

/*
 * Solid fill
 */
static Bool
qxl_check_solid(DrawablePtr drawable, int alu, Pixel planemask)
{
    if (!good_alu_and_pm(drawable, alu, planemask))
        return FALSE;

    return TRUE;
}

static Bool
qxl_prepare_solid(PixmapPtr pixmap, int alu, Pixel planemask, Pixel fg)
{
    qxl_surface_t *surface;

    if (!(surface = get_surface(pixmap)))
        return FALSE;

    return qxl_surface_prepare_solid(surface, fg);
}

static void
qxl_solid(PixmapPtr pixmap, int x1, int y1, int x2, int y2)
{
    qxl_surface_solid(get_surface(pixmap), x1, y1, x2, y2);
}

static void
qxl_done_solid(PixmapPtr pixmap)
{
}

/*
 * Copy
 */
static Bool
qxl_check_copy(PixmapPtr source, PixmapPtr dest,
               int alu, Pixel planemask)
{
    if (!good_alu_and_pm((DrawablePtr)source, alu, planemask))
        return FALSE;

    if (source->drawable.bitsPerPixel != dest->drawable.bitsPerPixel)
        {
            ErrorF("differing bitsperpixel - this shouldn't happen\n");
            return FALSE;
        }

    return TRUE;
}

static Bool
qxl_prepare_copy(PixmapPtr source, PixmapPtr dest,
                 int xdir, int ydir, int alu,
                 Pixel planemask)
{
    return qxl_surface_prepare_copy(get_surface(dest), get_surface(source));
}

static void
qxl_copy(PixmapPtr dest,
         int src_x1, int src_y1,
         int dest_x1, int dest_y1,
         int width, int height)
{
    qxl_surface_copy(get_surface(dest),
                     src_x1, src_y1,
                     dest_x1, dest_y1,
                     width, height);
}

static void
qxl_done_copy(PixmapPtr dest)
{
}

static Bool
qxl_put_image(PixmapPtr pDst, int x, int y, int w, int h,
               char *src, int src_pitch)
{
    qxl_surface_t *surface = get_surface(pDst);

    if (surface)
        return qxl_surface_put_image(surface, x, y, w, h, src, src_pitch);

    return FALSE;
}

static void
qxl_set_screen_pixmap(PixmapPtr pixmap)
{
    pixmap->drawable.pScreen->devPrivate = pixmap;
}

static PixmapPtr
qxl_create_pixmap(ScreenPtr screen, int w, int h, int depth, unsigned usage)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    PixmapPtr pixmap;
    qxl_screen_t *qxl = scrn->driverPrivate;
    qxl_surface_t *surface;

    if (w > 32767 || h > 32767)
        return NULL;

    qxl_surface_cache_sanity_check(qxl->surface_cache);

#if 0
    ErrorF("Create pixmap: %d %d @ %d (usage: %d)\n", w, h, depth, usage);
#endif

    if (uxa_swapped_out(screen))
        goto fallback;

    surface = qxl_surface_create(qxl->surface_cache, w, h, depth);

    if (surface) {
        /* ErrorF("   Successfully created surface in video memory\n"); */

        pixmap = fbCreatePixmap(screen, 0, 0, depth, usage);

        screen->ModifyPixmapHeader(pixmap, w, h,
                                   -1, -1, -1,
                                   NULL);

#if 0
        ErrorF("Create pixmap %p with surface %p\n", pixmap, surface);
#endif
        set_surface(pixmap, surface);
        qxl_surface_set_pixmap(surface, pixmap);

        qxl_surface_cache_sanity_check(qxl->surface_cache);
    } else {
#if 0
        ErrorF("   Couldn't allocate %d x %d @ %d surface in video memory\n",
               w, h, depth);
#endif
fallback:
        pixmap = fbCreatePixmap(screen, w, h, depth, usage);

#if 0
        ErrorF("Create pixmap %p without surface\n", pixmap);
#endif
    }

    return pixmap;
}

static Bool
qxl_destroy_pixmap(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    qxl_screen_t *qxl = scrn->driverPrivate;
    qxl_surface_t *surface = NULL;

    qxl_surface_cache_sanity_check(qxl->surface_cache);

    if (pixmap->refcnt == 1) {
        surface = get_surface(pixmap);

#if 0
        ErrorF("- Destroy %p (had surface %p)\n", pixmap, surface);
#endif

        if (surface) {
            qxl_surface_kill(surface);
            set_surface(pixmap, NULL);

            qxl_surface_cache_sanity_check(qxl->surface_cache);
        }
    }

    fbDestroyPixmap(pixmap);
    return TRUE;
}

static Bool
setup_uxa(qxl_screen_t *qxl, ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
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

    if (!uxa_driver_init(screen, qxl->uxa)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "UXA initialization failed\n");
        free(qxl->uxa);
        return FALSE;
    }

#if 0
    uxa_set_fallback_debug(screen, FALSE);
#endif

#if 0
    if (!uxa_driver_init(screen, qxl->uxa))
        return FALSE;
#endif

    return TRUE;
}

#ifdef XSPICE

static void
spiceqxl_screen_init(ScrnInfoPtr pScrn, qxl_screen_t *qxl)
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
qxl_fb_init(qxl_screen_t *qxl, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = qxl->pScrn;

#if 0
    ErrorF("allocated %d x %d  %p\n", pScrn->virtualX, pScrn->virtualY, qxl->fb);
#endif

    if (!fbScreenInit(pScreen, NULL,
                      pScrn->virtualX, pScrn->virtualY,
                      pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
                      pScrn->bitsPerPixel))
            return FALSE;

    fbPictureInit(pScreen, NULL, 0);
    return TRUE;
}

static Bool
qxl_screen_init(SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    qxl_screen_t *qxl = pScrn->driverPrivate;
    struct QXLRam *ram_header;
    VisualPtr visual;

    CHECK_POINT();

    assert(qxl->pScrn == pScrn);

    if (!qxl_map_memory(qxl, pScrn->scrnIndex))
        return FALSE;

#ifdef XSPICE
    spiceqxl_screen_init(pScrn, qxl);
#endif
    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);

    printf("ram_header at %d\n", qxl->rom->ram_header_offset);
    printf("surf0 size: %d\n", qxl->rom->surface0_area_size);

    qxl_save_state(pScrn);
    qxl_blank_screen(pScreen, SCREEN_SAVER_ON);

    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
                          pScrn->rgbBits, pScrn->defaultVisual))
        goto out;
    if (!miSetPixmapDepths())
        goto out;
    pScrn->displayWidth = pScrn->virtualX;

    qxl->fb = calloc(pScrn->virtualY * pScrn->displayWidth, 4);
    if (!qxl->fb)
        goto out;

#if 0
    ErrorF("allocated %d x %d  %p\n", pScrn->virtualX, pScrn->virtualY, qxl->fb);
#endif

    pScrn->virtualX = pScrn->currentMode->HDisplay;
    pScrn->virtualY = pScrn->currentMode->VDisplay;

    if (!qxl_fb_init(qxl, pScreen))
        goto out;

    visual = pScreen->visuals + pScreen->numVisuals;
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

    qxl->uxa = uxa_driver_alloc();

    /* Set up resources */
    qxl_reset_and_create_mem_slots(qxl);
    ErrorF("done reset\n");

#ifndef XSPICE
    qxl->io_pages = (void *)((unsigned long)qxl->ram);
    qxl->io_pages_physical = (void *)((unsigned long)qxl->ram_physical);
#endif

    qxl->command_ring = qxl_ring_create((struct qxl_ring_header *)&(ram_header->cmd_ring),
                                        sizeof (struct QXLCommand),
                                        QXL_COMMAND_RING_SIZE, QXL_IO_NOTIFY_CMD, qxl);
    qxl->cursor_ring = qxl_ring_create((struct qxl_ring_header *)&(ram_header->cursor_ring),
                                       sizeof (struct QXLCommand),
                                       QXL_CURSOR_RING_SIZE, QXL_IO_NOTIFY_CURSOR, qxl);
    qxl->release_ring = qxl_ring_create((struct qxl_ring_header *)&(ram_header->release_ring),
                                        sizeof (uint64_t),
                                        QXL_RELEASE_RING_SIZE, 0, qxl);

    qxl->surface_cache = qxl_surface_cache_create(qxl);

    /* xf86DPMSInit(pScreen, xf86DPMSSet, 0); */

    pScreen->SaveScreen = qxl_blank_screen;

    setup_uxa(qxl, pScreen);

    DamageSetup(pScreen);

    /* We need to set totalPixmapSize after setup_uxa and Damage,
       as the privatessize is not computed correctly until then */
    pScreen->totalPixmapSize = BitmapBytePad((sizeof(PixmapRec) + dixPrivatesSize(PRIVATE_PIXMAP) ) * 8);

    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());
    if (!miCreateDefColormap(pScreen))
        goto out;

    qxl->create_screen_resources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = qxl_create_screen_resources;

    qxl->close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = qxl_close_screen;

    qxl_cursor_init(pScreen);

    CHECK_POINT();

    pScreen->width = pScrn->currentMode->HDisplay;
    pScreen->height = pScrn->currentMode->VDisplay;

    if (!xf86CrtcScreenInit(pScreen))
        return FALSE;

    if (!qxl_resize_primary_to_virtual(qxl))
        return FALSE;

    /* Note: this must be done after DamageSetup() because it calls
     * _dixInitPrivates. And if that has been called, DamageSetup()
     * will assert.
     */
    if (!uxa_resources_init(pScreen))
        return FALSE;
    CHECK_POINT();

    return TRUE;

 out:
    return FALSE;
}

static Bool
qxl_enter_vt(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;

    qxl_save_state(pScrn);

    qxl_reset_and_create_mem_slots(qxl);

    if (!qxl_resize_primary_to_virtual(qxl))
        return FALSE;

    if (qxl->mem) {
        qxl_mem_free_all(qxl->mem);
        qxl_drop_image_cache(qxl);
    }

    if (qxl->surf_mem)
        qxl_mem_free_all(qxl->surf_mem);

    if (qxl->vt_surfaces)  {
        qxl_surface_cache_replace_all(qxl->surface_cache, qxl->vt_surfaces);

        qxl->vt_surfaces = NULL;
    }

    qxl_create_desired_modes(qxl);

    pScrn->EnableDisableFBAccess(XF86_SCRN_ARG(pScrn), TRUE);

    return TRUE;
}

static void
qxl_leave_vt(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    qxl_screen_t *qxl = pScrn->driverPrivate;

    xf86_hide_cursors(pScrn);

    pScrn->EnableDisableFBAccess(XF86_SCRN_ARG(pScrn), FALSE);

    qxl->vt_surfaces = qxl_surface_cache_evacuate_all(qxl->surface_cache);

    ioport_write(qxl, QXL_IO_RESET, 0);

    qxl_restore_state(pScrn);
    qxl->device_primary = QXL_DEVICE_PRIMARY_NONE;
}

static Bool
qxl_color_setup(ScrnInfoPtr pScrn)
{
    int scrnIndex = pScrn->scrnIndex;
    Gamma gzeros = { 0.0, 0.0, 0.0 };
    rgb rzeros = { 0, 0, 0 };

    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb))
        return FALSE;

    if (pScrn->depth != 15 && pScrn->depth != 24) {
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
print_modes(qxl_screen_t *qxl, int scrnIndex)
{
    int i;

    for (i = 0; i < qxl->num_modes; ++i) {
        struct QXLMode *m = qxl->modes + i;

        xf86DrvMsg(scrnIndex, X_INFO,
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

static DisplayModePtr qxl_add_mode(qxl_screen_t *qxl, ScrnInfoPtr pScrn,
                                   int width, int height, int type)
{
    DisplayModePtr mode;

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
    xf86SetModeCrtc(mode, pScrn->adjustFlags); /* needed? xf86-video-modesetting does this */
    qxl->x_modes = xf86ModesAdd(qxl->x_modes, mode);
    return mode;
}

static DisplayModePtr
qxl_output_get_modes(xf86OutputPtr output)
{
    qxl_output_private *qxl_output = output->driver_private;

    /* xf86ProbeOutputModes owns this memory */
    return xf86DuplicateModes(qxl_output->qxl->pScrn, qxl_output->qxl->x_modes);
}

static void
qxl_output_destroy(xf86OutputPtr output)
{
    qxl_output_private *qxl_output = output->driver_private;

    xf86DrvMsg(qxl_output->qxl->pScrn->scrnIndex, X_INFO,
               "%s", __func__);
}

static void
qxl_output_dpms(xf86OutputPtr output, int mode)
{
}

static void
qxl_output_create_resources(xf86OutputPtr output)
{
}

static Bool
qxl_output_set_property(xf86OutputPtr output, Atom property,
                        RRPropertyValuePtr value)
{
    /* EDID data is stored in the "EDID" atom property, we must return
     * TRUE here for that. No penalty to say ok to everything else. */
    return TRUE;
}

static Bool
qxl_output_get_property(xf86OutputPtr output, Atom property)
{
    return TRUE;
}

static xf86OutputStatus
qxl_output_detect(xf86OutputPtr output)
{
    // TODO - how do I query this? do I add fields and let the host set this instead
    // of the guest agent? or can I set this via the guest agent? I could just check
    // some files / anything in userspace, settable by the guest agent. dbus even.
    return XF86OutputStatusConnected;
}

static Bool
qxl_output_mode_valid(xf86OutputPtr output, DisplayModePtr pModes)
{
    return MODE_OK;
}

static const xf86OutputFuncsRec qxl_output_funcs = {
    .dpms = qxl_output_dpms,
    .create_resources = qxl_output_create_resources,
#ifdef RANDR_12_INTERFACE
    .set_property = qxl_output_set_property,
    .get_property = qxl_output_get_property,
#endif
    .detect = qxl_output_detect,
    .mode_valid = qxl_output_mode_valid,

    .get_modes = qxl_output_get_modes,
    .destroy = qxl_output_destroy
};

static void
qxl_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
}

static Bool
qxl_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
                        Rotation rotation, int x, int y)
{
    qxl_crtc_private *crtc_private = crtc->driver_private;
    qxl_screen_t *qxl = crtc_private->qxl;
    CHECK_POINT();

    if (!crtc_set_mode_major(crtc, mode, rotation, x, y))
        return FALSE;

    check_crtc(qxl);
    qxl_update_monitors_config(qxl);

    return TRUE;
}

static void
qxl_crtc_set_cursor_colors(xf86CrtcPtr crtc, int bg, int fg)
{
}

static void
qxl_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
}

static void
qxl_crtc_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
}

static void
qxl_crtc_hide_cursor(xf86CrtcPtr crtc)
{
}

static void
qxl_crtc_show_cursor(xf86CrtcPtr crtc)
{
}

static void
qxl_crtc_gamma_set(xf86CrtcPtr crtc, uint16_t *red, uint16_t *green,
                   uint16_t *blue, int size)
{
}

static void
qxl_crtc_destroy(xf86CrtcPtr crtc)
{
    qxl_crtc_private *crtc_private = crtc->driver_private;
    qxl_screen_t *qxl = crtc_private->qxl;

    xf86DrvMsg(qxl->pScrn->scrnIndex, X_INFO, "%s\n", __func__);
}

static Bool
qxl_crtc_lock(xf86CrtcPtr crtc)
{
    qxl_crtc_private *crtc_private = crtc->driver_private;
    qxl_screen_t *qxl = crtc_private->qxl;

    xf86DrvMsg(qxl->pScrn->scrnIndex, X_INFO, "%s\n", __func__);
    return TRUE;
}

static void
qxl_crtc_unlock(xf86CrtcPtr crtc)
{
    qxl_crtc_private *crtc_private = crtc->driver_private;
    qxl_screen_t *qxl = crtc_private->qxl;

    xf86DrvMsg(qxl->pScrn->scrnIndex, X_INFO, "%s\n", __func__);
    qxl_update_monitors_config(qxl);
}

static const xf86CrtcFuncsRec qxl_crtc_funcs = {
    .dpms = qxl_crtc_dpms,
    .set_mode_major = qxl_crtc_set_mode_major,
    .set_cursor_colors = qxl_crtc_set_cursor_colors,
    .set_cursor_position = qxl_crtc_set_cursor_position,
    .show_cursor = qxl_crtc_show_cursor,
    .hide_cursor = qxl_crtc_hide_cursor,
    .load_cursor_argb = qxl_crtc_load_cursor_argb,
    .lock = qxl_crtc_lock,
    .unlock = qxl_crtc_unlock,

    .gamma_set = qxl_crtc_gamma_set,
    .destroy = qxl_crtc_destroy,
};

static Bool
qxl_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height)
{
    qxl_screen_t *qxl = scrn->driverPrivate;

    xf86DrvMsg(scrn->scrnIndex, X_INFO, "%s: Placeholder resize %dx%d\n",
               __func__, width, height);
    if (!qxl_resize_primary(qxl, width, height))
        return FALSE;

    scrn->virtualX = width;
    scrn->virtualY = height;

    // when starting, no monitor is enabled, and count == 0
    // we want to avoid server/client freaking out with temporary config
    if (check_crtc(qxl) != 0)
        qxl_update_monitors_config(qxl);

    return TRUE;
}

static const xf86CrtcConfigFuncsRec qxl_xf86crtc_config_funcs = {
    qxl_xf86crtc_resize
};

static void
qxl_init_randr(ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    char name[32];
    qxl_output_private *qxl_output;
    qxl_crtc_private *qxl_crtc;
    int i;
    xf86OutputPtr output;

    xf86CrtcConfigInit(pScrn, &qxl_xf86crtc_config_funcs);

    /* CHECKME: This is actually redundant, it's overwritten by a later call via
     * xf86InitialConfiguration */
    xf86CrtcSetSizeRange(pScrn, 320, 200, 8192, 8192);

    qxl->crtcs = xnfcalloc(sizeof(xf86CrtcPtr), qxl->num_heads);
    qxl->outputs = xnfcalloc(sizeof(xf86OutputPtr), qxl->num_heads);

    for (i = 0 ; i < qxl->num_heads; ++i) {
        qxl->crtcs[i] = xf86CrtcCreate(pScrn, &qxl_crtc_funcs);
        if (!qxl->crtcs[i])
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "failed to create Crtc %d", i);

        qxl_crtc = xnfcalloc(sizeof(qxl_crtc_private), 1);
        qxl->crtcs[i]->driver_private = qxl_crtc;
        qxl_crtc->head = i;
        qxl_crtc->qxl = qxl;
        snprintf(name, sizeof(name), "qxl-%d", i);
        qxl->outputs[i] = output = xf86OutputCreate(pScrn, &qxl_output_funcs, name);
        if (!output)
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "failed to create Output %d", i);

        output->possible_crtcs = (1 << i); /* bitrange of allowed outputs - do a 1:1 */
        output->possible_clones = 0; /* TODO: not? */
        qxl_output = xnfcalloc(sizeof(qxl_output_private), 1);
        output->driver_private = qxl_output;
        qxl_output->head = i;
        qxl_output->qxl = qxl;
        qxl_crtc->output = output;
    }

    qxl->virtual_x = 1024;
    qxl->virtual_y = 768;

    pScrn->display->virtualX = qxl->virtual_x;
    pScrn->display->virtualY = qxl->virtual_y;

    xf86InitialConfiguration(pScrn, TRUE);
    /* all crtcs are enabled here, but their mode is 0,
       resulting monitor config empty atm */
}

static void
qxl_initialize_x_modes(qxl_screen_t *qxl, ScrnInfoPtr pScrn,
                       unsigned int *max_x, unsigned int *max_y)
{
    int i;
    int size;

    *max_x = *max_y = 0;
    /* Create a list of modes used by the qxl_output_get_modes */
    for (i = 0; i < qxl->num_modes; i++) {
        if (qxl->modes[i].orientation == 0) {
            size = qxl->modes[i].x_res * qxl->modes[i].y_res * 4;
            if (size > qxl->surface0_size) {
                ErrorF("skipping mode %dx%d not fitting in surface0",
                       qxl->modes[i].x_res, qxl->modes[i].y_res);
                continue;
            }

            qxl_add_mode(qxl, pScrn, qxl->modes[i].x_res, qxl->modes[i].y_res,
                         M_T_DRIVER);
            if (qxl->modes[i].x_res > *max_x)
                *max_x = qxl->modes[i].x_res;
            if (qxl->modes[i].y_res > *max_y)
                *max_y = qxl->modes[i].y_res;
        }
    }
}

static Bool
qxl_pre_init(ScrnInfoPtr pScrn, int flags)
{
    int scrnIndex = pScrn->scrnIndex;
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

    CHECK_POINT();

    mspace_set_abort_func(qxl_mspace_abort_func);
    mspace_set_print_func(qxl_mspace_print_func);

    /* zaphod mode is for suckers and i choose not to implement it */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
        xf86DrvMsg(scrnIndex, X_ERROR, "No Zaphod mode for you\n");
        return FALSE;
    }

    if (!pScrn->driverPrivate)
        pScrn->driverPrivate = xnfcalloc(sizeof(qxl_screen_t), 1);

    qxl = pScrn->driverPrivate;
    memset(qxl, 0, sizeof(qxl));
    qxl->device_primary = QXL_DEVICE_PRIMARY_UNDEFINED;
    qxl->pScrn = pScrn;
    qxl->x_modes = NULL;
    qxl->entity = xf86GetEntityInfo(pScrn->entityList[0]);

#ifndef XSPICE
    qxl->pci = xf86GetPciInfoForEntity(qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    qxl->pci_tag = pciTag(qxl->pci->bus, qxl->pci->device, qxl->pci->func);
#endif
#endif /* XSPICE */
    if (qxl->pci->revision < 4) {
        ErrorF ("Ignoring monitor config, device revision < 4\n");
    }
    pScrn->monitor = pScrn->confScreen->monitor;

    if (!qxl_color_setup(pScrn))
        goto out;

    /* option parsing and card differentiation */
    xf86CollectOptions(pScrn, NULL);
    memcpy(qxl->options, DefaultOptions, sizeof(DefaultOptions));
    xf86ProcessOptions(scrnIndex, pScrn->options, qxl->options);

    qxl->enable_image_cache =
        xf86ReturnOptValBool(qxl->options, OPTION_ENABLE_IMAGE_CACHE, TRUE);
    qxl->enable_fallback_cache =
        xf86ReturnOptValBool(qxl->options, OPTION_ENABLE_FALLBACK_CACHE, TRUE);
    qxl->enable_surfaces =
        xf86ReturnOptValBool(qxl->options, OPTION_ENABLE_SURFACES, TRUE);
    qxl->num_heads =
        get_int_option(qxl->options, OPTION_NUM_HEADS, "QXL_NUM_HEADS");

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

    qxl_initialize_x_modes(qxl, pScrn, &max_x, &max_y);

#if 0
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
#endif

    CHECK_POINT();

    xf86PruneDriverModes(pScrn);

    qxl_init_randr(pScrn, qxl);
#if 0
    /* If no modes are specified in xorg.conf, default to 1024x768 */
    if (pScrn->display->modes == NULL || pScrn->display->modes[0] == NULL)
        for (mode = pScrn->modes; mode; mode = mode->next)
            if (mode->HDisplay == 1024 && mode->VDisplay == 768) {
                pScrn->currentMode = mode;
                break;
            }
#endif

    //xf86PrintModes(pScrn);
    xf86SetDpi(pScrn, 0, 0);

    if (!xf86LoadSubModule(pScrn, "fb")
#ifndef XSPICE
        || !xf86LoadSubModule(pScrn, "ramdac")
        || !xf86LoadSubModule(pScrn, "vgahw")
#endif
        ) {
        goto out;
    }

    print_modes(qxl, scrnIndex);

#ifndef XSPICE
    /* VGA hardware initialisation */
    if (!vgaHWGetHWRec(pScrn))
        return FALSE;
    vgaHWSetStdFuncs(VGAHWPTR(pScrn));
#endif

    /* hate */
    qxl_unmap_memory(qxl);

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
enum qxl_class {
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
qxl_probe(struct _DriverRec *drv, int flags)
{
    ScrnInfoPtr pScrn;
    int entityIndex;
    EntityInfoPtr pEnt;
    GDevPtr* device;

    if (flags & PROBE_DETECT)
        return TRUE;

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
