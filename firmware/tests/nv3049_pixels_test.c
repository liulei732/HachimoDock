#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_nv3049_pixels.h"
#include "pet_p4_touch_core.h"

static void color(const uint8_t *fb, unsigned x, unsigned y, unsigned r, unsigned g, unsigned b) {
  const uint8_t *p=fb+(y*480+x)*3;
  assert(p[0]==b && p[1]==g && p[2]==r);
}

int main(void) {
  uint8_t *fb=malloc(480*640*3);
  assert(fb);
  memset(fb,0xCC,480*640*3);
  const uint16_t colors[]={0x0000,0xF800,0x07E0,0x001F,0xFFFF,0xFFE0,0x07FF,0xF81F};
  const unsigned expected[][3]={{0,0,0},{255,0,0},{0,255,0},{0,0,255},
    {255,255,255},{255,255,0},{0,255,255},{255,0,255}};
  assert(pet_nv3049_blit_rgb565(fb,0,0,8,1,colors));
  for (unsigned n=0;n<8;++n) color(fb,n,0,expected[n][0],expected[n][1],expected[n][2]);
  color(fb,8,0,0xCC,0xCC,0xCC);color(fb,0,1,0xCC,0xCC,0xCC);
  // Existing renderer's top-left logical pixel is native (479,0).
  assert(pet_nv3049_blit_rgb565(fb,479,0,1,1,&colors[1]));
  color(fb,479,0,255,0,0);
  assert(pet_nv3049_blit_rgb565(fb,0,639,1,1,&colors[3]));
  color(fb,0,639,0,0,255);
  assert(!pet_nv3049_blit_rgb565(fb,-1,0,1,1,colors));
  assert(!pet_nv3049_blit_rgb565(fb,479,639,2,1,colors));
  assert(!pet_nv3049_blit_rgb565(fb,0,0,480,641,colors));
  assert(!pet_nv3049_blit_rgb565(NULL,0,0,1,1,colors));
  int x=-1,y=-1;
  assert(pet_p4_touch_cst328_to_ui(0,319,240,320,&x,&y) && x==639 && y==479);
  assert(pet_p4_touch_cst328_to_ui(0,0,240,320,&x,&y) && x==0 && y==479);
  assert(pet_p4_touch_cst328_to_ui(239,0,240,320,&x,&y) && x==0 && y==0);
  assert(pet_p4_touch_cst328_to_ui(239,319,240,320,&x,&y) && x==639 && y==0);
  // The sensor has even dimensions: its two center pixels straddle the center.
  assert(pet_p4_touch_cst328_to_ui(120,160,240,320,&x,&y) && x==321 && y==238);
  assert(pet_p4_touch_cst328_to_ui(119,159,240,320,&x,&y) && x==318 && y==241);
  assert(!pet_p4_touch_cst328_to_ui(240,0,240,320,&x,&y));
  assert(!pet_p4_touch_cst328_to_ui(0,320,240,320,&x,&y));
  assert(!pet_p4_touch_cst328_to_ui(-1,0,240,320,&x,&y));
  assert(!pet_p4_touch_cst328_to_ui(0,0,1,320,&x,&y));
  assert(x==318 && y==241);
  free(fb);
  puts("NV3049 colors, orientation, partial writes, bounds and CST328 coordinate fixtures passed");
  return 0;
}
