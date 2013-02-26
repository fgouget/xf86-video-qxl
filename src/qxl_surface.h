#ifndef QXL_SURFACE_H
#define QXL_SURFACE_H

struct evacuated_surface_t;

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

	struct
	{
	    int			op;
	    PicturePtr		src_picture;
	    PicturePtr		mask_picture;
	    PicturePtr		dest_picture;
	    qxl_surface_t	*src;
	    qxl_surface_t	*mask;
	    qxl_surface_t	*dest;
	} composite;
    } u;
};

#endif
