/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * -------------------------------------------------------------------
 * This code is Based on Virtual Box OSE edid.c, with the following copyright
 * notice:
 *
 * Copyright (C) 2006-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 * --------------------------------------------------------------------
 *
 * This code is based on drmmode_display.c from the X.Org xf86-video-intel
 * driver with the following copyright notice:
 *
 * Copyright Â© 2007 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#include <misc.h>
#include <xf86DDC.h>
#include <xf86Crtc.h>

#include "qxl.h"

enum { EDID_SIZE = 128 };

typedef struct __attribute__ ((__packed__)) {
	unsigned char header[8];
	unsigned char manufacturer[2];
	unsigned char product_code[2];
	unsigned char serial[4];
	unsigned char week;
	unsigned char year;
	unsigned char version[2];
	unsigned char capabilities;
	unsigned char horizontal_resolution;
	unsigned char vertical_resolution;
	unsigned char gamma;
	unsigned char features;
	unsigned char chromaticity[10];
	unsigned char default_timings[3];
	unsigned char standard_timings[16];
	unsigned char descriptor1[18];
	unsigned char descriptor2[18];
	unsigned char descriptor3[18];
	unsigned char descriptor4[18];
	unsigned char num_extensions;
	unsigned char neg_checksum;
} EDIDv13;

int qxl_compile_time_test_edid_size[(sizeof(EDIDv13) == EDID_SIZE) - 1];

static const EDIDv13 edid_base =
{
   .header = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00},
   /* hex(sum([(ord(x) - ord('A') + 1) * 2**(5*i) for i,x in enumerate(reversed('QXL'))])) */
   .manufacturer = {0x47, 0x0c}, /* (QXL) 5 bit per char (A-Z), last bit 0 */
   .product_code = {0x00, 0x00},
   .serial = {0x00, 0x00, 0x00, 0x00}, /* set differently per mode */
   .year = 0x01,
   .week = 0x00,
   .version = {0x01, 0x03},
   .capabilities = 0x80, /* digital */
   .horizontal_resolution = 0x00, /* horiz. res in cm, zero for projectors */
   .vertical_resolution = 0x00, /* vert. res in cm */
   .gamma = 0x78, /* display gamma (120 == 2.2).  Should we ask the host for this? */
   .features = 0xEE, /* features (standby, suspend, off, RGB, standard colour space,
                      * preferred timing mode) */
   .chromaticity = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54},
       /* chromaticity for standard colour space - should we ask the host? */
   .default_timings = {0x00, 0x00, 0x00}, /* no default timings */
   .standard_timings = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                        0x01, 0x01, 0x01, 0x01}, /* no standard timings */
   .descriptor1 = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		   0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* descriptor block 1 goes here */
   .descriptor2 = {0x00, 0x00, 0x00, 0xFD, 0x00, /* descriptor block 2, monitor ranges */
		   0x00, 0xC8, 0x00, 0xC8, 0x64, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20},
		   /* 0-200Hz vertical, 0-200KHz horizontal, 1000MHz pixel clock */
   .descriptor3 = {0x00, 0x00, 0x00, 0xFC, 0x00, /* descriptor block 3, monitor name */
		   'Q', 'X', 'L', ' ', '1', '\n', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
   .descriptor4 = {0x00, 0x00, 0x00, 0x10, 0x00, /* descriptor block 4: dummy data */
		   0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20},
   .num_extensions = 0x00, /* number of extensions to follow */
   .neg_checksum = 0x00 /* checksum goes here */
};

static void fillDescBlockTimings(unsigned char *pchDescBlock,
                                 DisplayModePtr mode)
{
    struct detailed_timings timing;

    timing.clock = mode->Clock * 1000;
    timing.h_active = mode->HDisplay;
    timing.h_blanking = mode->HTotal - mode->HDisplay;
    timing.v_active = mode->VDisplay;
    timing.v_blanking = mode->VTotal - mode->VDisplay;
    timing.h_sync_off = mode->HSyncStart - mode->HDisplay;
    timing.h_sync_width = mode->HSyncEnd - mode->HSyncStart;
    timing.v_sync_off = mode->VSyncStart - mode->VDisplay;
    timing.v_sync_width = mode->VSyncEnd - mode->VSyncStart;
    pchDescBlock[0]   = (timing.clock / 10000) & 0xff;
    pchDescBlock[1]   = (timing.clock / 10000) >> 8;
    pchDescBlock[2]   = timing.h_active & 0xff;
    pchDescBlock[3]   = timing.h_blanking & 0xff;
    pchDescBlock[4]   = (timing.h_active >> 4) & 0xf0;
    pchDescBlock[4]  |= (timing.h_blanking >> 8) & 0xf;
    pchDescBlock[5]   = timing.v_active & 0xff;
    pchDescBlock[6]   = timing.v_blanking & 0xff;
    pchDescBlock[7]   = (timing.v_active >> 4) & 0xf0;
    pchDescBlock[7]  |= (timing.v_blanking >> 8) & 0xf;
    pchDescBlock[8]   = timing.h_sync_off & 0xff;
    pchDescBlock[9]   = timing.h_sync_width & 0xff;
    pchDescBlock[10]  = (timing.v_sync_off << 4) & 0xf0;
    pchDescBlock[10] |= timing.v_sync_width & 0xf;
    pchDescBlock[11]  = (timing.h_sync_off >> 2) & 0xC0;
    pchDescBlock[11] |= (timing.h_sync_width >> 4) & 0x30;
    pchDescBlock[11] |= (timing.v_sync_off >> 2) & 0xC;
    pchDescBlock[11] |= (timing.v_sync_width >> 4) & 0x3;
    pchDescBlock[12] = pchDescBlock[13] = pchDescBlock[14]
                     = pchDescBlock[15] = pchDescBlock[16]
                     = pchDescBlock[17] = 0;
}

static void setEDIDChecksum(EDIDv13 *edid)
{
    unsigned i, sum = 0;
    unsigned char *p = (unsigned char *)edid;

    for (i = 0; i < EDID_SIZE - 1; ++i)
        sum += p[i];
    edid->neg_checksum = (0x100 - (sum & 0xFF)) & 0xFF;
}

/**
 * Construct an EDID for an output given a preferred mode.  The main reason for
 * doing this is to confound gnome-settings-deamon which tries to reset the
 * last mode configuration if the same monitors are plugged in again, which is
 * a reasonable thing to do but not what we want in a VM.  We evily store
 * the (empty) raw EDID data at the end of the structure so that it gets
 * freed automatically along with the structure.
 */
Bool qxl_output_edid_set(xf86OutputPtr output, int head, DisplayModePtr mode)
{
    unsigned char *pch;
    EDIDv13 *edid;
    xf86MonPtr edid_mon;
    int eol_pos;

    pch = calloc(1, sizeof(xf86Monitor) + EDID_SIZE);
    if (!pch)
    {
        xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
            "Can't allocate memory for EDID structure.\n");
        return FALSE;
    }
    edid = (EDIDv13 *)(pch + sizeof(xf86Monitor));
    *edid = edid_base;
    edid->serial[0] = mode ? mode->HDisplay & 0xff : 0x00;
    edid->serial[1] = mode ? mode->HDisplay >> 8 : 0x00;
    edid->serial[2] = mode ? mode->VDisplay & 0xff : 0x00;
    edid->serial[3] = mode ? mode->VDisplay >> 8 : 0x00;
    snprintf((char *)&edid->descriptor3[5], 12, "QXL %d\n%n", head + 1, &eol_pos);
    edid->descriptor3[5 + eol_pos] = ' ';

    if (mode) {
        fillDescBlockTimings(edid->descriptor1, mode);
    }
    setEDIDChecksum(edid);
    edid_mon = xf86InterpretEDID(output->scrn->scrnIndex, (unsigned char *)edid);
    if (!edid_mon)
    {
        free(pch);
        return FALSE;
    }
    memcpy(pch, edid_mon, sizeof(xf86Monitor));
    free(edid_mon);
    edid_mon = (xf86MonPtr)pch;
    xf86OutputSetEDID(output, edid_mon);
    return TRUE;
}
