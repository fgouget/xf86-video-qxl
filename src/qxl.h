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

#ifndef QXL_H
#define QXL_H

#include <stdint.h>

#include <spice/qxl_dev.h>
#ifdef XSPICE
#include <spice.h>
#endif

#include "compiler.h"
#include "xf86.h"
#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#endif
#include "xf86Cursor.h"
#include "xf86_OSproc.h"
#include "xf86xv.h"
#include "shadow.h"
#include "micmap.h"
#include "uxa/uxa.h"

#ifndef XSPICE
#ifdef XSERVER_PCIACCESS
#include "pciaccess.h"
#endif
#include "fb.h"
#include "vgaHW.h"
#endif /* XSPICE */

#include "compat-api.h"
#define hidden _X_HIDDEN

#ifdef XSPICE
#define QXL_NAME		"spiceqxl"
#define QXL_DRIVER_NAME		"spiceqxl"
#else
#define QXL_NAME		"qxl"
#define QXL_DRIVER_NAME		"qxl"
#endif
#define PCI_VENDOR_RED_HAT	0x1b36

#define PCI_CHIP_QXL_0100	0x0100
#define PCI_CHIP_QXL_01FF	0x01ff

#pragma pack(push,1)

struct qxl_ring_header {
    uint32_t num_items;
    uint32_t prod;
    uint32_t notify_on_prod;
    uint32_t cons;
    uint32_t notify_on_cons;
};

#pragma pack(pop)
typedef struct surface_cache_t surface_cache_t;

typedef struct _qxl_screen_t qxl_screen_t;

typedef struct
{
    uint8_t	generation;
    uint64_t	start_phys_addr;
    uint64_t	end_phys_addr;
    uint64_t	start_virt_addr;
    uint64_t	end_virt_addr;
    uint64_t	high_bits;
} qxl_memslot_t;

typedef struct qxl_surface_t qxl_surface_t;

/*
 * Config Options
 */

enum {
    OPTION_ENABLE_IMAGE_CACHE = 0,
    OPTION_ENABLE_FALLBACK_CACHE,
    OPTION_ENABLE_SURFACES,
#ifdef XSPICE
    OPTION_SPICE_PORT,
    OPTION_SPICE_TLS_PORT,
    OPTION_SPICE_ADDR,
    OPTION_SPICE_X509_DIR,
    OPTION_SPICE_SASL,
    OPTION_SPICE_AGENT_MOUSE,
    OPTION_SPICE_DISABLE_TICKETING,
    OPTION_SPICE_PASSWORD,
    OPTION_SPICE_X509_KEY_FILE,
    OPTION_SPICE_STREAMING_VIDEO,
    OPTION_SPICE_PLAYBACK_COMPRESSION,
    OPTION_SPICE_ZLIB_GLZ_WAN_COMPRESSION,
    OPTION_SPICE_JPEG_WAN_COMPRESSION,
    OPTION_SPICE_IMAGE_COMPRESSION,
    OPTION_SPICE_DISABLE_COPY_PASTE,
    OPTION_SPICE_IPV4_ONLY,
    OPTION_SPICE_IPV6_ONLY,
    OPTION_SPICE_X509_CERT_FILE,
    OPTION_SPICE_X509_KEY_PASSWORD,
    OPTION_SPICE_TLS_CIPHERS,
    OPTION_SPICE_CACERT_FILE,
    OPTION_SPICE_DH_FILE,
#endif
    OPTION_COUNT,
};

enum {
    QXL_DEVICE_PRIMARY_UNDEFINED,
    QXL_DEVICE_PRIMARY_NONE,
    QXL_DEVICE_PRIMARY_CREATED,
};

struct _qxl_screen_t
{
    /* These are the names QXL uses */
    void *			ram;	/* Command RAM */
    void *			ram_physical;
    void *			vram;	/* Surface RAM */
    void *			vram_physical;
    struct QXLRom *		rom;    /* Parameter RAM */
    
    struct qxl_ring *		command_ring;
    struct qxl_ring *		cursor_ring;
    struct qxl_ring *		release_ring;

    int                         device_primary;
    
    int				num_modes;
    struct QXLMode *		modes;
    int				io_base;
    void *			surface0_area;
    long			surface0_size;
    long			vram_size;

    int				virtual_x;
    int				virtual_y;
    void *			fb;
    int				stride;
    struct QXLMode *		current_mode;
    qxl_surface_t *		primary;
    
    int				bytes_per_pixel;

    /* Commands */
    struct qxl_mem *		mem;   /* Context for qxl_alloc/free */

    /* Surfaces */
    struct qxl_mem *		surf_mem;  /* Context for qxl_surf_alloc/free */
    
    EntityInfoPtr		entity;

#ifndef XSPICE
    void *			io_pages;
    void *			io_pages_physical;

#ifdef XSERVER_LIBPCIACCESS
    struct pci_device *		pci;
#else
    pciVideoPtr			pci;
    PCITAG			pci_tag;
#endif
    vgaRegRec                   vgaRegs;
#endif /* XSPICE */

    uxa_driver_t *		uxa;
    
    CreateScreenResourcesProcPtr create_screen_resources;
    CloseScreenProcPtr		close_screen;
    CreateGCProcPtr		create_gc;
    CopyWindowProcPtr		copy_window;
    
    int16_t			cur_x;
    int16_t			cur_y;
    int16_t			hot_x;
    int16_t			hot_y;
    
    ScrnInfoPtr			pScrn;

    qxl_memslot_t *		mem_slots;
    uint8_t			n_mem_slots;

    uint8_t			main_mem_slot;
    uint8_t			slot_id_bits;
    uint8_t			slot_gen_bits;
    uint64_t			va_slot_mask;

    uint8_t			vram_mem_slot;

    surface_cache_t *		surface_cache;

    /* Evacuated surfaces are stored here during VT switches */
    void *			vt_surfaces;

    OptionInfoRec	options[OPTION_COUNT + 1];

    int				enable_image_cache;
    int				enable_fallback_cache;
    int				enable_surfaces;
    
#ifdef XSPICE
    /* XSpice specific */
    struct QXLRom		shadow_rom;    /* Parameter RAM */
    SpiceServer *       spice_server;
    QXLWorker *         worker;
    int                 worker_running;
    QXLInstance         display_sin;
    /* XSpice specific, dragged from the Device */
    QXLReleaseInfo     *last_release;

    uint32_t           cmdflags;
    uint32_t           oom_running;
    uint32_t           num_free_res; /* is having a release ring effective
                                        for Xspice? */
    /* This is only touched from red worker thread - do not access
     * from Xorg threads. */
    struct guest_primary {
        QXLSurfaceCreate surface;
        uint32_t       commands;
        uint32_t       resized;
        int32_t        stride;
        uint32_t       bits_pp;
        uint32_t       bytes_pp;
        uint8_t        *data, *flipped;
    } guest_primary;
#endif /* XSPICE */
};

static inline uint64_t
physical_address (qxl_screen_t *qxl, void *virtual, uint8_t slot_id)
{
    qxl_memslot_t *p_slot = &(qxl->mem_slots[slot_id]);

    return p_slot->high_bits | ((unsigned long)virtual - p_slot->start_virt_addr);
}

static inline void *
virtual_address (qxl_screen_t *qxl, void *physical, uint8_t slot_id)
{
    qxl_memslot_t *p_slot = &(qxl->mem_slots[slot_id]);
    unsigned long virt;

    virt = ((unsigned long)physical) & qxl->va_slot_mask;
    virt += p_slot->start_virt_addr;

    return (void *)virt;
}

static inline void *
u64_to_pointer (uint64_t u)
{
    return (void *)(unsigned long)u;
}

static inline uint64_t
pointer_to_u64 (void *p)
{
    return (uint64_t)(unsigned long)p;
}

struct qxl_ring;

/*
 * HW cursor
 */
void              qxl_cursor_init        (ScreenPtr               pScreen);



/*
 * Rings
 */
struct qxl_ring * qxl_ring_create      (struct qxl_ring_header *header,
					int                     element_size,
					int                     n_elements,
					int                     prod_notify,
					qxl_screen_t            *qxl);
void              qxl_ring_push        (struct qxl_ring        *ring,
					const void             *element);
Bool              qxl_ring_pop         (struct qxl_ring        *ring,
					void                   *element);
void              qxl_ring_wait_idle   (struct qxl_ring        *ring);


/*
 * Surface
 */
surface_cache_t *   qxl_surface_cache_create (qxl_screen_t *qxl);
qxl_surface_t *	    qxl_surface_cache_create_primary (surface_cache_t *qxl,
						struct QXLMode *mode);
qxl_surface_t *	    qxl_surface_create (surface_cache_t *qxl,
					int	      width,
					int	      height,
					int	      bpp);
void
qxl_surface_cache_sanity_check (surface_cache_t *qxl);
void *
qxl_surface_cache_evacuate_all (surface_cache_t *qxl);
void
qxl_surface_cache_replace_all (surface_cache_t *qxl, void *data);

void		    qxl_surface_set_pixmap (qxl_surface_t *surface,
					    PixmapPtr      pixmap);
/* Call this to indicate that the server is done with the surface */
void		    qxl_surface_kill (qxl_surface_t *surface);
/* Call this when a notification comes back from the device
 * that the surface has been destroyed
 */
void		    qxl_surface_recycle (surface_cache_t *cache, uint32_t id);

/* send anything pending to the other side */
void		    qxl_surface_flush (qxl_surface_t *surface);

/* access */
Bool		    qxl_surface_prepare_access (qxl_surface_t *surface,
						PixmapPtr      pixmap,
						RegionPtr      region,
						uxa_access_t   access);
void		    qxl_surface_finish_access (qxl_surface_t *surface,
					       PixmapPtr      pixmap);

/* solid */
Bool		    qxl_surface_prepare_solid (qxl_surface_t *destination,
					       Pixel	      fg);
void		    qxl_surface_solid         (qxl_surface_t *destination,
					       int	      x1,
					       int	      y1,
					       int	      x2,
					       int	      y2);

/* copy */
Bool		    qxl_surface_prepare_copy (qxl_surface_t *source,
					      qxl_surface_t *dest);
void		    qxl_surface_copy	     (qxl_surface_t *dest,
					      int  src_x1, int src_y1,
					      int  dest_x1, int dest_y1,
					      int width, int height);
Bool		    qxl_surface_put_image    (qxl_surface_t *dest,
					      int x, int y, int width, int height,
					      const char *src, int src_pitch);
void		    qxl_surface_unref        (surface_cache_t *cache,
					      uint32_t surface_id);
					      
#if HAS_DEVPRIVATEKEYREC
extern DevPrivateKeyRec uxa_pixmap_index;
#else
extern int uxa_pixmap_index;
#endif

static inline qxl_surface_t *get_surface (PixmapPtr pixmap)
{
#if HAS_DEVPRIVATEKEYREC
    return dixGetPrivate(&pixmap->devPrivates, &uxa_pixmap_index);
#else
    return dixLookupPrivate(&pixmap->devPrivates, &uxa_pixmap_index);
#endif
}

static inline void set_surface (PixmapPtr pixmap, qxl_surface_t *surface)
{
    dixSetPrivate(&pixmap->devPrivates, &uxa_pixmap_index, surface);
}

static inline struct QXLRam *
get_ram_header (qxl_screen_t *qxl)
{
    return (struct QXLRam *)
	((uint8_t *)qxl->ram + qxl->rom->ram_header_offset);
}

/*
 * Images
 */
struct QXLImage *qxl_image_create     (qxl_screen_t           *qxl,
				       const uint8_t          *data,
				       int                     x,
				       int                     y,
				       int                     width,
				       int                     height,
				       int                     stride,
				       int                     Bpp,
				       Bool		       fallback);
void              qxl_image_destroy    (qxl_screen_t           *qxl,
					struct QXLImage       *image);
void		  qxl_drop_image_cache (qxl_screen_t	       *qxl);


/*
 * Malloc
 */
int		  qxl_handle_oom (qxl_screen_t *qxl);
struct qxl_mem *  qxl_mem_create       (void                   *base,
					unsigned long           n_bytes);
void              qxl_mem_dump_stats   (struct qxl_mem         *mem,
					const char             *header);
void *            qxl_alloc            (struct qxl_mem         *mem,
					unsigned long           n_bytes);
void              qxl_free             (struct qxl_mem         *mem,
					void                   *d);
void              qxl_mem_free_all     (struct qxl_mem         *mem);
void *            qxl_allocnf          (qxl_screen_t           *qxl,
					unsigned long           size);
int		   qxl_garbage_collect (qxl_screen_t *qxl);

#ifdef DEBUG_QXL_MEM
void qxl_mem_unverifiable(struct qxl_mem *mem);
#else
static inline void qxl_mem_unverifiable(struct qxl_mem *mem) {}
#endif

/*
 * I/O port commands
 */
void qxl_update_area(qxl_screen_t *qxl);
void qxl_io_memslot_add(qxl_screen_t *qxl, uint8_t id);
void qxl_io_create_primary(qxl_screen_t *qxl);
void qxl_io_notify_oom(qxl_screen_t *qxl);
void qxl_io_flush_surfaces(qxl_screen_t *qxl);
void qxl_io_destroy_all_surfaces (qxl_screen_t *qxl);

#ifdef XSPICE
/* device to spice-server, now xspice to spice-server */
void ioport_write(qxl_screen_t *qxl, uint32_t io_port, uint32_t val);
#else
static inline void ioport_write(qxl_screen_t *qxl, int port, int val)
{
    outb(qxl->io_base + port, val);
}
#endif

#ifdef XSPICE

#define MEMSLOT_GROUP 0
#define NUM_MEMSLOTS_GROUPS 1

// Taken from qemu's qxl.c, not sure the values make sense? we
// only have a single slot, and it is never changed after being added,
// so not a problem?
#define NUM_MEMSLOTS 8
#define MEMSLOT_GENERATION_BITS 8
#define MEMSLOT_SLOT_BITS 1

// qemu/cpu-all.h
#define TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
// qemu/target-i386/cpu.h
#define TARGET_PAGE_BITS 12

#define NUM_SURFACES 1024

/* initializes if required and returns the server singleton */
SpiceServer *xspice_get_spice_server(void);

#endif /* XSPICE */

#endif // QXL_H
