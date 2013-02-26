#ifndef QXL_SURFACE_H
#define QXL_SURFACE_H

struct evacuated_surface_t;

struct qxl_surface_t
{
    surface_cache_t    *cache;

    qxl_screen_t *qxl;
    uint32_t	        id;

    pixman_image_t *	dev_image;
    pixman_image_t *	host_image;

    uxa_access_t	access_type;
    RegionRec		access_region;

    void *		address;
    void *		end;
    
    struct qxl_surface_t *	next;
    struct qxl_surface_t *	prev;	/* Only used in the 'live'
				 * chain in the surface cache
				 */

    int			in_use;
    int			bpp;		/* bpp of the pixmap */
    int			ref_count;

    PixmapPtr		pixmap;

    struct evacuated_surface_t *evacuated;

    union
    {
	struct qxl_surface_t *copy_src;
	Pixel	       solid_pixel;

	struct
	{
	    int			op;
	    PicturePtr		src_picture;
	    PicturePtr		mask_picture;
	    PicturePtr		dest_picture;
	    struct qxl_surface_t	*src;
	    struct qxl_surface_t	*mask;
	    struct qxl_surface_t	*dest;
	} composite;
    } u;
};

#endif
