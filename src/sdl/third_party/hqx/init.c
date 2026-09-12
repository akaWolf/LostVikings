/*
 * Copyright (C) 2010 Cameron Zemek ( grom@zeminvaders.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdint.h>
#include "hqx.h"

uint32_t   RGBtoYUV[32768];   /* Lost Vikings: RGB555-indexed (see NOTES.txt) */
uint32_t   YUV1, YUV2;

HQX_API void HQX_CALLCONV hqxInit(void)
{
    /* Initalize RGB to YUV lookup table */
    uint32_t c, r, g, b, y, u, v;
    for (c = 0; c < 32768; c++) {   /* Lost Vikings: the 5-bit channel's centre value */
        r = ((c >> 10) & 0x1F) << 3 | 4;
        g = ((c >> 5) & 0x1F) << 3 | 4;
        b = (c & 0x1F) << 3 | 4;
        y = (uint32_t)(0.299*r + 0.587*g + 0.114*b);
        u = (uint32_t)(-0.169*r - 0.331*g + 0.5*b) + 128;
        v = (uint32_t)(0.5*r - 0.419*g - 0.081*b) + 128;
        RGBtoYUV[c] = (y << 16) + (u << 8) + v;
    }
}
