/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2013 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "showtime.h"
#include "arch/atomic.h"
#include "pixmap.h"
#include "misc/minmax.h"
#include "misc/jpeg.h"
#include "backend/backend.h"

#if ENABLE_LIBAV
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/mem.h>
#include <libavutil/common.h>
#include <libavutil/pixdesc.h>
#endif

pixmap_t *(*accel_pixmap_decode)(pixmap_t *pm, const image_meta_t *im,
				 char *errbuf, size_t errlen);

//#define DIV255(x) ((x) / 255)
//#define DIV255(x) ((x) >> 8)
#define DIV255(x) (((((x)+255)>>8)+(x))>>8)

/**
 *
 */
int
color_is_not_gray(uint32_t rgb)
{
  uint8_t r = rgb;
  uint8_t g = rgb >> 8;
  uint8_t b = rgb >> 16;
  return (r != g) || (g != b);
}




/**
 *
 */
pixmap_t *
pixmap_dup(pixmap_t *pm)
{
  atomic_add(&pm->pm_refcount, 1);
  return pm;
}


/**
 *
 */
pixmap_t *
pixmap_alloc_coded(const void *data, size_t size, pixmap_type_t type)
{
  int pad = 32;
  pixmap_t *pm = calloc(1, sizeof(pixmap_t));
  pm->pm_refcount = 1;
  pm->pm_size = size;

  pm->pm_width = -1;
  pm->pm_height = -1;

  pm->pm_data = malloc(size + pad);
  if(pm->pm_data == NULL) {
    free(pm);
    return NULL;
  }
  if(data != NULL)
    memcpy(pm->pm_data, data, size);

  memset(pm->pm_data + size, 0, pad);

  pm->pm_type = type;
  return pm;
}


/**
 *
 */
pixmap_t *
pixmap_create(int width, int height, pixmap_type_t type, int margin)
{
  int bpp = bytes_per_pixel(type);
  const int rowalign = PIXMAP_ROW_ALIGN - 1;

  pixmap_t *pm = calloc(1, sizeof(pixmap_t));
  pm->pm_refcount = 1;
  pm->pm_width = width + margin*2;
  pm->pm_height = height + margin*2;
  pm->pm_linesize = ((pm->pm_width * bpp) + rowalign) & ~rowalign;
  pm->pm_type = type;
  pm->pm_margin = margin;

  if(pm->pm_linesize > 0) {
    /* swscale can write a bit after the buffer in its optimized algo
       therefore we need to allocate a bit extra 
    */
    pm->pm_data = mymemalign(PIXMAP_ROW_ALIGN,
                             pm->pm_linesize * pm->pm_height + 8);
    if(pm->pm_data == NULL) {
      free(pm);
      return NULL;
    }
    memset(pm->pm_data, 0, pm->pm_linesize * pm->pm_height);
  }

  pm->pm_aspect = (float)width / (float)height;
  return pm;
}


/**
 *
 */
pixmap_t *
pixmap_create_vector(int width, int height)
{
  pixmap_t *pm = calloc(1, sizeof(pixmap_t));
  pm->pm_refcount = 1;
  pm->pm_capacity = 256;
  pm->pm_width = width;
  pm->pm_height = height;
  pm->pm_data = malloc(pm->pm_capacity * sizeof(int32_t));
  if(pm->pm_data == NULL) {
    free(pm);
    return NULL;
  }
  pm->pm_type = PIXMAP_VECTOR;
  return pm;
}


/**
 *
 */
static const char vec_cmd_len[] = {
  [VC_SET_FILL_ENABLE] = 1,
  [VC_SET_FILL_COLOR] = 1,
  [VC_SET_STROKE_WIDTH] = 1,
  [VC_SET_STROKE_COLOR] = 1,
  [VC_MOVE_TO] = 2,
  [VC_LINE_TO] = 2,
  [VC_CUBIC_TO] = 6,
};


/**
 *
 */
static void
vec_resize(pixmap_t *pm, vec_cmd_t cmd)
{
  int len = vec_cmd_len[cmd] + 1;
  if(pm->pm_used + len > pm->pm_capacity) {
    pm->pm_capacity = 2 * pm->pm_capacity + len + 16;
    pm->pm_data = realloc(pm->pm_data, pm->pm_capacity * sizeof(float));
  }
}


/**
 *
 */
void
vec_emit_0(pixmap_t *pm, vec_cmd_t cmd)
{
  vec_resize(pm, cmd);
  pm->pm_int[pm->pm_used++] = cmd;
}


/**
 *
 */
void
vec_emit_i1(pixmap_t *pm, vec_cmd_t cmd, int32_t i)
{
  vec_resize(pm, cmd);

  switch(cmd) {
  case VC_SET_FILL_COLOR:
  case VC_SET_STROKE_COLOR:
    pm->pm_flags |= PIXMAP_COLORIZED;
    break;
  default:
    break;
  }

  pm->pm_int[pm->pm_used++] = cmd;
  pm->pm_int[pm->pm_used++] = i;
}


/**
 *
 */
void
vec_emit_f1(pixmap_t *pm, vec_cmd_t cmd, const float *a)
{
  vec_resize(pm, cmd);
  pm->pm_int[pm->pm_used++] = cmd;
  pm->pm_flt[pm->pm_used++] = a[0];
  pm->pm_flt[pm->pm_used++] = a[1];
}


/**
 *
 */
void
vec_emit_f3(pixmap_t *pm, vec_cmd_t cmd, const float *a, const float *b, const float *c)
{
  vec_resize(pm, cmd);
  int ptr = pm->pm_used;
  pm->pm_int[ptr+0] = cmd;
  pm->pm_flt[ptr+1] = a[0];
  pm->pm_flt[ptr+2] = a[1];
  pm->pm_flt[ptr+3] = b[0];
  pm->pm_flt[ptr+4] = b[1];
  pm->pm_flt[ptr+5] = c[0];
  pm->pm_flt[ptr+6] = c[1];
  pm->pm_used = ptr + 7;
}


/**
 *
 */
void
pixmap_release(pixmap_t *pm)
{
  if(atomic_add(&pm->pm_refcount, -1) > 1)
    return;

  if(!pixmap_is_coded(pm)) {
    free(pm->pm_pixels);
    free(pm->pm_charpos);
  } else {
    free(pm->pm_data);
  }
  free(pm);
}


/**
 *
 */
static void
horizontal_gradient_rgb24(pixmap_t *pm, const int *top, const int *bottom)
{
  int y;
  const int h = pm->pm_height - pm->pm_margin * 2;
  const int w = pm->pm_width - pm->pm_margin * 2;
  unsigned int X=123456789, Y=362436069, Z=521288629, T;

  for(y = 0; y < h; y++) {
    uint8_t *d = pm_pixel(pm, 0, y);

    int r = 255 * top[0] + (255 * (bottom[0] - top[0]) * y / h);
    int g = 255 * top[1] + (255 * (bottom[1] - top[1]) * y / h);
    int b = 255 * top[2] + (255 * (bottom[2] - top[2]) * y / h);
    int x;
    for(x = 0; x < w; x++) {
      // Marsaglia's xorshf generator
      X ^= X << 16;
      X ^= X >> 5;
      X ^= X << 1;
      
      T = X;
      X = Y;
      Y = Z;
      Z = T ^ X ^ Y;
      *d++ = (r + (Z & 0xff)) >> 8;
      *d++ = (g + (Z & 0xff)) >> 8;
      *d++ = (b + (Z & 0xff)) >> 8;
    }
  }
}



/**
 *
 */
static void
horizontal_gradient_bgr32(pixmap_t *pm, const int *top, const int *bottom)
{
  int y;
  const int h = pm->pm_height - pm->pm_margin * 2;
  const int w = pm->pm_width - pm->pm_margin * 2;

  unsigned int X=123456789, Y=362436069, Z=521288629, T;

  for(y = 0; y < h; y++) {
    uint32_t *d = pm_pixel(pm, 0, y);

    int r = 255 * top[0] + (255 * (bottom[0] - top[0]) * y / h);
    int g = 255 * top[1] + (255 * (bottom[1] - top[1]) * y / h);
    int b = 255 * top[2] + (255 * (bottom[2] - top[2]) * y / h);
    int x;
    for(x = 0; x < w; x++) {
      // Marsaglia's xorshf generator
      X ^= X << 16;
      X ^= X >> 5;
      X ^= X << 1;
      
      T = X;
      X = Y;
      Y = Z;
      Z = T ^ X ^ Y;
      uint8_t R = (r + (Z & 0xff)) >> 8;
      uint8_t G = (g + (Z & 0xff)) >> 8;
      uint8_t B = (b + (Z & 0xff)) >> 8;
      *d++ = 0xff000000 | B << 16 | G << 8 | R; 
    }
  }
}



/**
 *
 */
void
pixmap_horizontal_gradient(pixmap_t *pm, const int *top, const int *bottom)
{
  switch(pm->pm_type) {
  case PIXMAP_RGB24:
    horizontal_gradient_rgb24(pm, top, bottom);
    break;
  case PIXMAP_BGR32:
    horizontal_gradient_bgr32(pm, top, bottom);
    break;
  default:
    break;
  }
}


/**
 *
 */
static pixmap_t *
rgb24_to_bgr32(pixmap_t *src)
{
  pixmap_t *dst = pixmap_create(src->pm_width, src->pm_height, PIXMAP_BGR32,
				src->pm_margin);
  int y;
  
  for(y = 0; y < src->pm_height; y++) {
    const uint8_t *s = src->pm_pixels + y * src->pm_linesize;
    uint32_t *d = (uint32_t *)(dst->pm_pixels + y * dst->pm_linesize);
    int x;
    for(x = 0; x < src->pm_width; x++) {
      *d++ = 0xff000000 | s[2] << 16 | s[1] << 8 | s[0]; 
      s+= 3;
    }
  }
  return dst;
}



/**
 *
 */
pixmap_t *
pixmap_rounded_corners(pixmap_t *pm, int r, int which)
{
  pixmap_t *tmp;
  switch(pm->pm_type) {

  default:
    return pm;

  case PIXMAP_BGR32:
    break;

  case PIXMAP_RGB24:
    tmp = rgb24_to_bgr32(pm);
    pixmap_release(pm);
    pm = tmp;
    break;
  }


  r = MIN(pm->pm_height / 2, r);

  int r2 = r * r;
  int i;
  uint32_t *dst;
  for(i = 0; i < r; i++) {
    float x = r - sqrtf(r2 - i*i);
    int len = x;
    int alpha = 255 - (x - len) * 255;
    int y = r - i - 1;

    dst = pm_pixel(pm, 0, y);

    if(which & PIXMAP_CORNER_TOPLEFT) {
      memset(dst, 0, len * sizeof(uint32_t));
      dst[len] = (dst[len] & 0x00ffffff) | alpha << 24;
    }

    if(which & PIXMAP_CORNER_TOPRIGHT) {
      dst += pm->pm_width - pm->pm_margin * 2;
      memset(dst - len, 0, len * sizeof(uint32_t));
      dst[-len-1] = (dst[-len-1] & 0x00ffffff) | alpha << 24;
    }


    dst = pm_pixel(pm, 0, pm->pm_height - 1 - y - pm->pm_margin*2);

    if(which & PIXMAP_CORNER_BOTTOMLEFT) {
      memset(dst, 0, len * sizeof(uint32_t));
      dst[len] = (dst[len] & 0x00ffffff) | alpha << 24;
    }

    if(which & PIXMAP_CORNER_BOTTOMRIGHT) {
      dst += pm->pm_width - pm->pm_margin * 2;
      memset(dst - len, 0, len * sizeof(uint32_t));
      dst[-len-1] = (dst[-len-1] & 0x00ffffff) | alpha << 24;
    }
  }
  return pm;
}


#define FIXMUL(a, b) (((a) * (b) + 255) >> 8)
#define FIX3MUL(a, b, c) (((a) * (b) * (c) + 65535) >> 16)



static void
composite_GRAY8_on_IA(uint8_t *dst, const uint8_t *src,
			 int i0, int foo_, int bar_, int a0,
			 int width)
{
  int i, a, pa, y;
  int x;
  for(x = 0; x < width; x++) {

    if(*src) {
      i = dst[0];
      a = dst[1];

      pa = a;
      y = FIXMUL(a0, *src);
      a = y + FIXMUL(a, 255 - y);

      if(a) {
	i = ((FIXMUL(i0, y) + FIX3MUL(i, pa, (255 - y))) * 255) / a;
      } else {
	i = 0;
      }
      dst[0] = i;
      dst[1] = a;
    }
    src++;
    dst += 2;
  }
}



static void
composite_GRAY8_on_IA_full_alpha(uint8_t *dst, const uint8_t *src,
				    int i0, int b0_, int g0_, int a0_,
				    int width)
{
  int i, a, pa, y;
  int x;
  for(x = 0; x < width; x++) {

    if(*src == 255) {
      dst[0] = i0;
      dst[1] = 255;
    } else if(*src) {
      i = dst[0];
      a = dst[1];

      pa = a;
      y = *src;
      a = y + FIXMUL(a, 255 - y);

      if(a) {
	i = ((FIXMUL(i0, y) + FIX3MUL(i, pa, (255 - y))) * 255) / a;
      } else {
	i = 0;
      }
      dst[0] = i;
      dst[1] = a;
    }
    src++;
    dst += 2;
  }
}



#if 0

static void
composite_GRAY8_on_BGR32(uint8_t *dst_, const uint8_t *src,
			 int r0, int g0, int b0, int a0,
			 int width)
{
  int x;
  uint32_t *dst = (uint32_t *)dst_;
  int a, r, g, b, pa, y;
  uint32_t u32;

  for(x = 0; x < width; x++) {
    
    u32 = *dst;

    r = u32 & 0xff;
    g = (u32 >> 8) & 0xff;
    b = (u32 >> 16) & 0xff;
    a = (u32 >> 24) & 0xff;

    pa = a;
    y = FIXMUL(a0, *src);
    a = y + FIXMUL(a, 255 - y);

    if(a) {
      r = ((FIXMUL(r0, y) + FIX3MUL(r, pa, (255 - y))) * 255) / a;
      g = ((FIXMUL(g0, y) + FIX3MUL(g, pa, (255 - y))) * 255) / a;
      b = ((FIXMUL(b0, y) + FIX3MUL(b, pa, (255 - y))) * 255) / a;
    } else {
      r = g = b = 0;
    }
    u32 = a << 24 | b << 16 | g << 8 | r;
    *dst = u32;
    src++;
    dst++;
  }
}


#else



static void
composite_GRAY8_on_BGR32(uint8_t *dst_, const uint8_t *src,
			 int CR, int CG, int CB, int CA,
			 int width)
{
  int x;
  uint32_t *dst = (uint32_t *)dst_;
  uint32_t u32;

  for(x = 0; x < width; x++) {

    int SA = DIV255(*src * CA);
    int SR = CR;
    int SG = CG;
    int SB = CB;

    u32 = *dst;
    
    int DR =  u32        & 0xff;
    int DG = (u32 >> 8)  & 0xff;
    int DB = (u32 >> 16) & 0xff;
    int DA = (u32 >> 24) & 0xff;

    int FA = SA + DIV255((255 - SA) * DA);

    if(FA == 0) {
      SA = 0;
      u32 = 0;
    } else {
      if(FA != 255)
	SA = SA * 255 / FA;

      DA = 255 - SA;
      
      DB = DIV255(SB * SA + DB * DA);
      DG = DIV255(SG * SA + DG * DA);
      DR = DIV255(SR * SA + DR * DA);
      
      u32 = FA << 24 | DB << 16 | DG << 8 | DR;
    }
    *dst = u32;

    src++;
    dst++;
  }
}
#endif


/**
 *
 */
void
pixmap_composite(pixmap_t *dst, const pixmap_t *src,
		 int xdisp, int ydisp, int rgba)
{
  int y, wy;
  uint8_t *d0;
  const uint8_t *s0;
  void (*fn)(uint8_t *dst, const uint8_t *src,
	     int red, int green, int blue, int alpha,
	     int width);

  int readstep = 0;
  int writestep = 0;
  uint8_t r = rgba;
  uint8_t g = rgba >> 8;
  uint8_t b = rgba >> 16;
  uint8_t a = rgba >> 24;

  if(src->pm_type == PIXMAP_I && dst->pm_type == PIXMAP_IA && 
     a == 255)
    fn = composite_GRAY8_on_IA_full_alpha;
  else if(src->pm_type == PIXMAP_I && dst->pm_type == PIXMAP_IA)
    fn = composite_GRAY8_on_IA;
  else if(src->pm_type == PIXMAP_I && dst->pm_type == PIXMAP_BGR32)
    fn = composite_GRAY8_on_BGR32;
  else
    return;
  
  readstep  = bytes_per_pixel(src->pm_type);
  writestep = bytes_per_pixel(dst->pm_type);

  s0 = src->pm_pixels;
  d0 = dst->pm_pixels;

  int xx = src->pm_width;

  if(xdisp < 0) {
    // Painting left of dst image
    s0 += readstep * -xdisp;
    xx += xdisp;
    xdisp = 0;
	
  } else if(xdisp > 0) {
    d0 += writestep * xdisp;
  }
      
  if(xx + xdisp > dst->pm_width) {
    xx = dst->pm_width - xdisp;
  }
  

  for(y = 0; y < src->pm_height; y++) {
    wy = y + ydisp;
    if(wy >= 0 && wy < dst->pm_height)
      fn(d0 + wy * dst->pm_linesize, s0 + y * src->pm_linesize, r, g, b, a, xx);
  }
}



static void
box_blur_line_2chan(uint8_t *d, const uint32_t *a, const uint32_t *b,
		    int width, int boxw, int m)
{
  int x;
  unsigned int v;
  for(x = 0; x < boxw; x++) {
    const int x1 = 2 * MIN(x + boxw, width - 1);
    const int x2 = 0;

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
  }

  for(; x < width - boxw; x++) {
    const int x1 = 2 * (x + boxw);
    const int x2 = 2 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
  }

  for(; x < width; x++) {
    const int x1 = 2 * (width - 1);
    const int x2 = 2 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
  }
}



static void
box_blur_line_4chan(uint8_t *d, const uint32_t *a, const uint32_t *b,
		    int width, int boxw, int m)
{
  int x;
  unsigned int v;
  for(x = 0; x < boxw; x++) {
    const int x1 = 4 * MIN(x + boxw, width - 1);
    const int x2 = 0;

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
    v = b[x1 + 2] + a[x2 + 2] - b[x2 + 2] - a[x1 + 2];
    *d++ = (v * m) >> 16;
    v = b[x1 + 3] + a[x2 + 3] - b[x2 + 3] - a[x1 + 3];
    *d++ = (v * m) >> 16;
  }

  for(; x < width - boxw; x++) {
    const int x1 = 4 * (x + boxw);
    const int x2 = 4 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
    v = b[x1 + 2] + a[x2 + 2] - b[x2 + 2] - a[x1 + 2];
    *d++ = (v * m) >> 16;
    v = b[x1 + 3] + a[x2 + 3] - b[x2 + 3] - a[x1 + 3];
    *d++ = (v * m) >> 16;
  }

  for(; x < width; x++) {
    const int x1 = 4 * (width - 1);
    const int x2 = 4 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    *d++ = (v * m) >> 16;
    v = b[x1 + 1] + a[x2 + 1] - b[x2 + 1] - a[x1 + 1];
    *d++ = (v * m) >> 16;
    v = b[x1 + 2] + a[x2 + 2] - b[x2 + 2] - a[x1 + 2];
    *d++ = (v * m) >> 16;
    v = b[x1 + 3] + a[x2 + 3] - b[x2 + 3] - a[x1 + 3];
    *d++ = (v * m) >> 16;
  }
}


/**
 *
 */
void
pixmap_box_blur(pixmap_t *pm, int boxw, int boxh)
{
  unsigned int *tmp, *t;
  int x, y, i;
  const uint8_t *s;
  const int w = pm->pm_width;
  const int h = pm->pm_height;
  const int ls = pm->pm_linesize;
  const int z = bytes_per_pixel(pm->pm_type);

  boxw = MIN(boxw, w);

  void (*fn)(uint8_t *dst, const uint32_t *a, const uint32_t *b, int width,
	     int boxw, int m);

  switch(z) {
  case 2:
    fn = box_blur_line_2chan;
    break;
  case 4:
    fn = box_blur_line_4chan;
    break;


  default:
    return;
  }



  tmp = mymalloc(ls * h * sizeof(unsigned int));
  if(tmp == NULL)
    return;

  s = pm->pm_data;
  t = tmp;

  for(i = 0; i < z; i++)
    *t++ = *s++;

  for(x = 0; x < (w-1)*z; x++) {
    t[0] = *s++ + t[-z];
    t++;
  }

  for(y = 1; y < h; y++) {

    s = pm->pm_data + y * ls;
    t = tmp + y * ls;

    for(i = 0; i < z; i++) {
      t[0] = *s++ + t[-ls];
      t++;
    }

    for(x = 0; x < (w-1)*z; x++) {
      t[0] = *s++ + t[-z] + t[-ls] - t[-ls - z];
      t++;
    }
  }

  int m = 65536 / ((boxw * 2 + 1) * (boxh * 2 + 1));



  for(y = 0; y < h; y++) {
    uint8_t *d = pm->pm_data + y * ls;

    const unsigned int *a = tmp + ls * MAX(0, y - boxh);
    const unsigned int *b = tmp + ls * MIN(h - 1, y + boxh);
    fn(d, a, b, w, boxw, m);
  }

  free(tmp);
}



/**
 *
 */
static uint32_t
mix_bgr32(uint32_t src, uint32_t dst)
{
  int SR =  src        & 0xff;
  int SG = (src >> 8)  & 0xff;
  int SB = (src >> 16) & 0xff;
  int SA = (src >> 24) & 0xff;
  
  int DR =  dst        & 0xff;
  int DG = (dst >> 8)  & 0xff;
  int DB = (dst >> 16) & 0xff;
  int DA = (dst >> 24) & 0xff;
  
  int FA = SA + DIV255((255 - SA) * DA);

  if(FA == 0) {
    dst = 0;
  } else {
    if(FA != 255)
      SA = SA * 255 / FA;
    
    DA = 255 - SA;
    
    DB = DIV255(SB * SA + DB * DA);
    DG = DIV255(SG * SA + DG * DA);
    DR = DIV255(SR * SA + DR * DA);
    
    dst = FA << 24 | DB << 16 | DG << 8 | DR;
  }
  return dst;
}



/**
 *
 */
static void
drop_shadow_rgba(uint8_t *D, const uint32_t *a, const uint32_t *b,
                 int width, int boxw, int m)
{
  uint32_t *d = (uint32_t *)D;

  int x;
  unsigned int v;
  int s;
  for(x = 0; x < boxw; x++) {
    const int x1 = MIN(x + boxw, width - 1);
    const int x2 = 0;

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;

    *d = mix_bgr32(*d, s << 24);
    d++;
  }

  for(; x < width - boxw; x++) {
    const int x1 = (x + boxw);
    const int x2 = (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;
    *d = mix_bgr32(*d, s << 24);
    d++;
  }

  for(; x < width; x++) {
    const int x1 = (width - 1);
    const int x2 = (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;
    *d = mix_bgr32(*d, s << 24);
    d++;
  }
}


/**
 *
 */
static void
mix_ia(uint8_t *src, uint8_t *dst, int DR, int DA)
{
  int SR = src[0];
  int SA = src[1];
  
  //  int DR = dst[0];
  //  int DA = dst[1];
  
  int FA = SA + DIV255((255 - SA) * DA);

  if(FA == 0) {
    dst[0] = 0;
    dst[1] = 0;
  } else {
    if(FA != 255)
      SA = SA * 255 / FA;
    
    DA = 255 - SA;
    
    DR = DIV255(SR * SA + DR * DA);
    dst[0] = DR;
    dst[1] = FA;
  }
}

/**
 *
 */
static void
drop_shadow_ia(uint8_t *d, const uint32_t *a, const uint32_t *b,
               int width, int boxw, int m)
{

  int x;
  unsigned int v;
  int s;
  for(x = 0; x < boxw; x++) {
    const int x1 = 2 * MIN(x + boxw, width - 1);
    const int x2 = 0;

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;

    mix_ia(d, d, 0, s);
    d+=2;
  }

  for(; x < width - boxw; x++) {
    const int x1 = 2 * (x + boxw);
    const int x2 = 2 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;
    mix_ia(d, d, 0, s);
    d+=2;
  }

  for(; x < width; x++) {
    const int x1 = 2 * (width - 1);
    const int x2 = 2 * (x - boxw);

    v = b[x1 + 0] + a[x2 + 0] - b[x2 + 0] - a[x1 + 0];
    s = (v * m) >> 16;
    mix_ia(d, d, 0, s);
    d+=2;
  }
}


/**
 *
 */
void
pixmap_drop_shadow(pixmap_t *pm, int boxw, int boxh)
{
  const uint8_t *s;
  unsigned int *tmp, *t;
  int ach;   // Alpha channel
  int z;
  int w = pm->pm_width;
  int h = pm->pm_height;
  int ls = pm->pm_linesize;
  int x, y;

  assert(boxw > 0);
  assert(boxh > 0);

  boxw = MIN(boxw, w);

  void (*fn)(uint8_t *dst, const uint32_t *a, const uint32_t *b, int width,
	     int boxw, int m);

  switch(pm->pm_type) {
  case PIXMAP_BGR32:
    ach = 3;
    z = 4;
    fn = drop_shadow_rgba;
    break;

  case PIXMAP_IA:
    ach = 1;
    z = 2;
    fn = drop_shadow_ia;
    break;

  default:
    return;
  }

  tmp = mymalloc(pm->pm_width * pm->pm_height * sizeof(unsigned int));
  if(tmp == NULL)
    return;

  s = pm->pm_data + ach;
  t = tmp;

  for(y = 0; y < boxh; y++)
    for(x = 0; x < w; x++)
      *t++ = 0;
    
  for(; y < h; y++) {

    s = pm->pm_data + (y - boxh) * ls + ach;
    for(x = 0; x < boxw; x++)
      *t++ = 0;
    
    for(; x < w; x++) {
      t[0] = *s + t[-1] + t[-w] - t[-w - 1];
      s += z;
      t++;
    }
  }
  
  int m = 65536 / ((boxw * 2 + 1) * (boxh * 2 + 1));

  for(y = 0; y < h; y++) {
    uint8_t *d = pm->pm_data + y * ls;

    const unsigned int *a = tmp + pm->pm_width * MAX(0, y - boxh);
    const unsigned int *b = tmp + pm->pm_width * MIN(h - 1, y + boxh);
    fn(d, a, b, w, boxw, m);
  }
  free(tmp);
}



#if ENABLE_LIBAV


/**
 * Rescaling with FFmpeg's swscaler
 */
static pixmap_t *
pixmap_rescale_swscale(const AVPicture *pict, int src_pix_fmt, 
		       int src_w, int src_h,
		       int dst_w, int dst_h,
		       int with_alpha, int margin)
{
  AVPicture pic;
  int dst_pix_fmt;
  struct SwsContext *sws;
  const uint8_t *ptr[4];
  int strides[4];
  pixmap_t *pm;

  switch(src_pix_fmt) {
  case PIX_FMT_Y400A:
  case PIX_FMT_BGRA:
  case PIX_FMT_RGBA:
  case PIX_FMT_ABGR:
  case PIX_FMT_ARGB:
#ifdef __PPC__
    return NULL;
#endif
    dst_pix_fmt = PIX_FMT_BGR32;
    break;

  default:
    if(with_alpha)
      dst_pix_fmt = PIX_FMT_BGR32;
    else
      dst_pix_fmt = PIX_FMT_RGB24;
    break;
  }

  const int swscale_debug = 0;

  if(swscale_debug)
    TRACE(TRACE_DEBUG, "info", "Converting %d x %d [%s] to %d x %d [%s]",
	  src_w, src_h, av_get_pix_fmt_name(src_pix_fmt),
	  dst_w, dst_h, av_get_pix_fmt_name(dst_pix_fmt));
  
  sws = sws_getContext(src_w, src_h, src_pix_fmt, 
		       dst_w, dst_h, dst_pix_fmt,
		       SWS_LANCZOS | 
		       (swscale_debug ? SWS_PRINT_INFO : 0), NULL, NULL, NULL);
  if(sws == NULL)
    return NULL;

  ptr[0] = pict->data[0];
  ptr[1] = pict->data[1];
  ptr[2] = pict->data[2];
  ptr[3] = pict->data[3];

  strides[0] = pict->linesize[0];
  strides[1] = pict->linesize[1];
  strides[2] = pict->linesize[2];
  strides[3] = pict->linesize[3];

  switch(dst_pix_fmt) {
  case PIX_FMT_RGB24:
    pm = pixmap_create(dst_w, dst_h, PIXMAP_RGB24, margin);
    break;

  default:
    pm = pixmap_create(dst_w, dst_h, PIXMAP_BGR32, margin);
    break;
  }

  if(pm == NULL) {
    sws_freeContext(sws);
    return NULL;
  }

  // Set scale destination with respect to margin
  pic.data[0] = pm_pixel(pm, 0, 0);
  pic.linesize[0] = pm->pm_linesize;
  pic.linesize[1] = 0;
  pic.linesize[2] = 0;
  pic.linesize[3] = 0;
  sws_scale(sws, ptr, strides, 0, src_h, pic.data, pic.linesize);
#if 0  
  if(pm->pm_type == PIXMAP_BGR32) {
    uint32_t *dst = pm->pm_data;
    int i;

    for(i = 0; i < pm->pm_linesize * pm->pm_height; i+= 4) {
      *dst |= 0xff000000;
      dst++;
    }

  }
#endif

  sws_freeContext(sws);
  return pm;
}


static void   __attribute__((unused))

swizzle_xwzy(uint32_t *dst, const uint32_t *src, int len)
{
  int i;
  uint32_t u32;
  for(i = 0; i < len; i++) {
    u32 = *src++;
    *dst++ = (u32 & 0xff00ff00) | (u32 & 0xff) << 16 | (u32 & 0xff0000) >> 16;
  }
}

static pixmap_t *
pixmap_32bit_swizzle(AVPicture *pict, int pix_fmt, int w, int h, int m)
{
#if defined(__BIG_ENDIAN__)
  void (*fn)(uint32_t *dst, const uint32_t *src, int len);
  // go to BGR32 which is ABGR on big endian.
  switch(pix_fmt) {
  case PIX_FMT_ARGB:
    fn = swizzle_xwzy;
    break;
  default:
    return NULL;
  }

  int y;
  pixmap_t *pm = pixmap_create(w, h, PIXMAP_BGR32, m);
  if(pm == NULL)
    return NULL;

  for(y = 0; y < h; y++) {
    fn(pm_pixel(pm, 0, y),
       (uint32_t *)(pict->data[0] + y * pict->linesize[0]),
       w);
  }
  return pm;
#else
  return NULL;
#endif
}



/**
 *
 */
static pixmap_t *
pixmap_from_avpic(AVPicture *pict, int pix_fmt, 
		  int src_w, int src_h,
		  int req_w0, int req_h0,
		  const image_meta_t *im)
{
  int i;
  int need_format_conv = 0;
  int want_rescale = 0; // Want rescaling cause it looks better
  uint32_t *palette;
  pixmap_type_t fmt = 0;
  pixmap_t *pm;

  assert(pix_fmt != -1);

  switch(pix_fmt) {
  default:
    need_format_conv = 1;
    break;

  case PIX_FMT_RGB24:
    if(im->im_no_rgb24 || im->im_corner_radius)
      need_format_conv = 1;
    else
      fmt = PIXMAP_RGB24;
    break;

  case PIX_FMT_BGR32:
    fmt = PIXMAP_BGR32;
    break;
    
  case PIX_FMT_Y400A:
    if(!im->im_can_mono) {
      need_format_conv = 1;
      break;
    }

    fmt = PIXMAP_IA;
    break;

  case PIX_FMT_GRAY8:
    if(!im->im_can_mono) {
      need_format_conv = 1;
      break;
    }

    fmt = PIXMAP_I;
    break;

  case PIX_FMT_PAL8:
    palette = (uint32_t *)pict->data[1];

    for(i = 0; i < 256; i++) {
      if((palette[i] >> 24) == 0)
	palette[i] = 0;
    }

    need_format_conv = 1;
    break;
  }

  int req_w = req_w0, req_h = req_h0;

  want_rescale = req_w != src_w || req_h != src_h;

  if(want_rescale || need_format_conv) {
    int want_alpha = im->im_no_rgb24 || im->im_corner_radius;

    pm = pixmap_rescale_swscale(pict, pix_fmt, src_w, src_h, req_w, req_h,
				want_alpha, im->im_margin);
    if(pm != NULL)
      return pm;

    if(need_format_conv) {
      pm = pixmap_rescale_swscale(pict, pix_fmt, src_w, src_h, src_w, src_h,
				  want_alpha, im->im_margin);
      if(pm != NULL)
	return pm;

      return pixmap_32bit_swizzle(pict, pix_fmt, src_w, src_h, im->im_margin);
    }
  }

  pm = pixmap_create(src_w, src_h, fmt, im->im_margin);
  if(pm == NULL)
    return NULL;

  uint8_t *dst = pm_pixel(pm, 0,0);
  uint8_t *src = pict->data[0];
  int h = src_h;

  if(pict->linesize[0] != pm->pm_linesize) {
    while(h--) {
      memcpy(dst, src, pm->pm_linesize);
      src += pict->linesize[0];
      dst +=  pm->pm_linesize;
    }
  } else {
    memcpy(dst, src, pm->pm_linesize * src_h);
  }
  return pm;
}


/**
 *
 */
void
pixmap_compute_rescale_dim(const image_meta_t *im,
			   int src_width, int src_height,
			   int *dst_width, int *dst_height)
{
  int w;
  int h;
  if(im->im_want_thumb) {
    w = 160;
    h = 160 * src_height / src_width;
  } else {
    w = src_width;
    h = src_height;
  }

  if(im->im_req_width != -1 && im->im_req_height != -1) {
    w = im->im_req_width;
    h = im->im_req_height;

  } else if(im->im_req_width != -1) {
    w = im->im_req_width;
    h = im->im_req_width * src_height / src_width;

  } else if(im->im_req_height != -1) {
    w = im->im_req_height * src_width / src_height;
    h = im->im_req_height;

  } else if(w > 64 && h > 64) {

    if(im->im_max_width && w > im->im_max_width) {
      h = h * im->im_max_width / w;
      w = im->im_max_width;
    }

    if(im->im_max_height && h > im->im_max_height) {
      w = w * im->im_max_height / h;
      h = im->im_max_height;
    }
  }
  *dst_width  = w;
  *dst_height = h;
}

/**
 *
 */
pixmap_t *
pixmap_decode(pixmap_t *pm, const image_meta_t *im,
	      char *errbuf, size_t errlen)
{
  AVCodecContext *ctx;
  AVCodec *codec;
  AVFrame *frame;
  int got_pic, w, h;
  int orientation = pm->pm_orientation;
  jpeg_meminfo_t mi;
  int lowres = 0;
  jpeginfo_t ji = {0};

  if(!pixmap_is_coded(pm)) {
    pm->pm_aspect = (float)pm->pm_width / (float)pm->pm_height;
    return pm;
  }

  if(accel_pixmap_decode != NULL) {
    pixmap_t *r = accel_pixmap_decode(pm, im, errbuf, errlen);
    if(r != NULL) 
      return r;
  }

  switch(pm->pm_type) {
  case PIXMAP_SVG:
    return svg_decode(pm, im, errbuf, errlen);
  case PIXMAP_PNG:
    codec = avcodec_find_decoder(CODEC_ID_PNG);
    break;
  case PIXMAP_JPEG:

    mi.data = pm->pm_data;
    mi.size = pm->pm_size;
    
    if(jpeg_info(&ji, jpeginfo_mem_reader, &mi, 
		 JPEG_INFO_DIMENSIONS,
		 pm->pm_data, pm->pm_size, errbuf, errlen)) {
      pixmap_release(pm);
      return NULL;
    }

    if((im->im_req_width > 0  && ji.ji_width  > im->im_req_width * 16) ||
       (im->im_req_height > 0 && ji.ji_height > im->im_req_height * 16))
      lowres = 2;
    else if((im->im_req_width  > 0 && ji.ji_width  > im->im_req_width * 8) ||
	    (im->im_req_height > 0 && ji.ji_height > im->im_req_height * 8))
      lowres = 1;
    else if(ji.ji_width > 4096 || ji.ji_height > 4096)
      lowres = 1; // swscale have problems with dimensions > 4096

    codec = avcodec_find_decoder(CODEC_ID_MJPEG);
    break;
  case PIXMAP_GIF:
    codec = avcodec_find_decoder(CODEC_ID_GIF);
    break;
  default:
    codec = NULL;
    break;
  }

  if(codec == NULL) {
    pixmap_release(pm);
    snprintf(errbuf, errlen, "No codec for image format");
    return NULL;
  }

  ctx = avcodec_alloc_context3(codec);
  ctx->lowres = lowres;

  if(avcodec_open2(ctx, codec, NULL) < 0) {
    av_free(ctx);
    pixmap_release(pm);
    snprintf(errbuf, errlen, "Unable to open codec");
    return NULL;
  }
  
  frame = avcodec_alloc_frame();

  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = pm->pm_data;
  avpkt.size = pm->pm_size;
  int r = avcodec_decode_video2(ctx, frame, &got_pic, &avpkt);

  if(r < 0 || ctx->width == 0 || ctx->height == 0) {
    pixmap_release(pm);
    avcodec_close(ctx);
    av_free(ctx);
    av_free(frame);
    snprintf(errbuf, errlen, "Unable to decode image of size (%d x %d)",
             ctx->width, ctx->height);
    return NULL;
  }

#if 0
  printf("%d x %d => %d x %d (lowres=%d) req = %d x %d%s%s\n",
	 ji.ji_width, ji.ji_height,
	 ctx->width, ctx->height, lowres,
	 im->im_req_width, im->im_req_height,
	 im->im_want_thumb ? ", want thumb" : "",
	 pm->pm_flags & PIXMAP_THUMBNAIL ? ", is thumb" : "");
#endif

  pixmap_compute_rescale_dim(im, ctx->width, ctx->height, &w, &h);

  pixmap_release(pm);

  pm = pixmap_from_avpic((AVPicture *)frame, 
			 ctx->pix_fmt, ctx->width, ctx->height, w, h, im);

  if(pm != NULL) {
    pm->pm_orientation = orientation;
    // Compute correct aspect ratio based on orientation
    if(pm->pm_orientation < LAYOUT_ORIENTATION_TRANSPOSE) {
      pm->pm_aspect = (float)w / (float)h;
    } else {
      pm->pm_aspect = (float)h / (float)w;
    }
  } else {
    snprintf(errbuf, errlen, "Out of memory");
  }
  av_free(frame);

  avcodec_close(ctx);
  av_free(ctx);
  return pm;
}

#endif // LIBAV_ENABLE

// gcc -O3 src/misc/pixmap.c -o /tmp/pixmap -Isrc -DLOCAL_MAIN

#ifdef LOCAL_MAIN

static int64_t
get_ts(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

int
main(int argc, char **argv)
{
  pixmap_t *dst = pixmap_create(2048, 2048, PIXMAP_BGR32);
  pixmap_t *src = pixmap_create(2048, 2048, PIXMAP_GRAY8);

  memset(src->pm_pixels, 0xff, src->pm_linesize * src->pm_height);

  int64_t a = get_ts();
  pixmap_composite(dst, src, 0, 0, 0xffffffff);
  printf("Compositing in %dµs\n",(int)( get_ts() - a));
  printf("dst pixel(0,0) = 0x%x\n", dst->pm_pixels[0]);
  return 0;
}

#endif


/**
 *
 */
static pixmap_t *
be_showtime_pixmap_loader(const char *url, const image_meta_t *im,
			  const char **vpaths, char *errbuf, size_t errlen,
			  int *cache_control, cancellable_t *c)
{
  pixmap_t *pm;
  int w = im->im_req_width, h = im->im_req_height;
  const char *s;
  if((s = mystrbegins(url, "showtime:pixmap:gradient:")) != NULL) {
    if(w == -1)
      w = 128;
    if(h == -1)
      h = 128;
    int t[4] = {0,0,0,255};
    int b[4] = {0,0,0,255};
    if(sscanf(s, "%d,%d,%d:%d,%d,%d",
	      &t[0], &t[1], &t[2], &b[0], &b[1], &b[2]) != 6) {
      snprintf(errbuf, errlen, "Invalid RGB codes");
      return NULL;
    }

    pm = pixmap_create(w, h, PIXMAP_BGR32, im->im_margin);
    pixmap_horizontal_gradient(pm, t, b);
  } else {
    snprintf(errbuf, errlen, "Invalid URL");
    return NULL;
  }
  return pm;
}


/**
 *
 */
static int
be_showtime_pixmap_canhandle(const char *url)
{
  if(!strncmp(url, "showtime:pixmap:", strlen("showtime:pixmap:")))
    return 1;
  return 0;
}

/**
 *
 */
static backend_t be_showtime_pixmap = {
  .be_canhandle   = be_showtime_pixmap_canhandle,
  .be_imageloader = be_showtime_pixmap_loader,
};

BE_REGISTER(showtime_pixmap);
