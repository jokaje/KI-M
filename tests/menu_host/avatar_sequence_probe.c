/* Real LVGL 9 RGB565 rendering driven by production avatar_player.c handoff. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "avatar_player.h"
static uint16_t draw[320*240], screen_pixels[320*240];
static uint16_t fallback[192*192], a[192*192], b[192*192];
static uint8_t sleep_hold_start[192*192*2];
static kim_avatar_player_t p;
static lv_image_dsc_t desc;
static lv_obj_t *image;
static unsigned flushes;

static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *src) {
    ++flushes;
    for (int y=area->y1;y<=area->y2;y++)
        memcpy(screen_pixels+y*320+area->x1,
               (uint16_t *)src+(y-area->y1)*(area->x2-area->x1+1),
               (area->x2-area->x1+1)*sizeof(uint16_t));
    lv_display_flush_ready(d);
}
/* Same source-swap/release sequence as main.c, with a completed decode
 * written to the actual back buffer only by the simulated worker. */
static void tick(unsigned clip,unsigned ms,unsigned epoch,int completion) {
    kim_avatar_request(&p,clip,ms,epoch);
    if(completion >= 0) {
        unsigned i,e;
        assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&i,&e));
        if(completion) {
            for(unsigned k=0;k<192*192;k++) p.back[k]=(uint16_t)completion;
        }
        kim_avatar_finish_decode(&p,i,e,completion != 0);
    }
    bool changed=kim_avatar_present(&p);
    const uint8_t *source=(const uint8_t *)kim_avatar_display_data(&p,fallback);
    if(changed || desc.data != source) {
        desc.data=source;
        lv_image_set_src(image,&desc);
    }
    if(changed) kim_avatar_release(&p);
    lv_tick_inc(100);lv_timer_handler();
}
static void visible(uint16_t value) {
    static unsigned step;
    ++step;
    if(screen_pixels[100*320+120]!=value)
        fprintf(stderr,"step=%u got=%04x expected=%04x source=%p fallback=%p shown=%u desired=%u\n",
                step,screen_pixels[100*320+120],value,(void *)desc.data,(void *)fallback,
                atomic_load(&p.shown),atomic_load(&p.desired));
    assert(screen_pixels[100*320+120]==value);
    assert(desc.data == (const uint8_t *)kim_avatar_display_data(&p,fallback));
}
int main(void) {
    memset(sleep_hold_start,0x11,sizeof sleep_hold_start);
    p.front=a;p.back=b;

    lv_init();
    lv_display_t *d=lv_display_create(320,240);
    lv_display_set_buffers(d,draw,NULL,sizeof draw,LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d,flush);
    lv_obj_t *root=lv_screen_active();
    lv_obj_set_style_bg_color(root,lv_color_hex(0x101828),0);
    desc.header.cf=LV_COLOR_FORMAT_RGB565;desc.header.w=192;desc.header.h=192;
    desc.header.stride=384;desc.data_size=sizeof fallback;
#define descriptor desc
#define pixels fallback
#define screen root
#define avatar p
#define ART_SIZE 192
#include "avatar_main_boot.inc" /* extracted verbatim from production main.c */
#undef ART_SIZE

#undef avatar
#undef screen
#undef pixels
#undef descriptor
    tick(0,0,1,-1);visible(0x1111); /* never display an uninitialized front */
    tick(0,0,1,0);visible(0x1111); /* decode failure */
    tick(1,0,2,0xaaaa);visible(0xaaaa); /* non-idle authored frame */
    tick(0,0,3,-1);visible(0xaaaa); /* latest validated frame */
    tick(0,0,3,0x2222);visible(0x2222);
    unsigned unchanged_flushes=flushes;
    tick(0,100,3,-1);visible(0x2222); /* 100ms UI poll before next frame */
    tick(0,200,3,-1);visible(0x2222); /* desired advances, decode delayed */
    assert(flushes==unchanged_flushes); /* no unconditional LVGL invalidate */
    tick(0,300,3,0x3333);visible(0x3333);
    assert(flushes>unchanged_flushes); /* conditional source publication renders */
    tick(0,400,3,-1);visible(0x3333);
    /* Decode begins at t=500; frame selection advances before it completes. */
    kim_avatar_request(&p,0,500,3);
    unsigned i,e;assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&i,&e));
    for(unsigned k=0;k<192*192;k++) p.back[k]=0x4444;
    kim_avatar_request(&p,0,700,3);
    kim_avatar_finish_decode(&p,i,e,true);
    tick(0,700,3,-1);visible(0x4444);
    tick(6,0,4,-1);visible(0x4444); /* latest validated moving frame */
    tick(6,0,4,0x5555);visible(0x5555);
    tick(6,4000,4,-1);visible(0x5555); /* held sleep frame while pending */
    tick(6,4000,4,0x6666);visible(0x6666);
    tick(6,4100,5,-1);visible(0x6666); /* retain sleep until wake decode */
    tick(6,4100,5,0x7777);visible(0x7777);
    tick(0,0,6,-1);visible(0x7777); /* reentry into idle */
    tick(0,0,6,0x8888);visible(0x8888);
    /* Offline retains the most recent validated frame across epochs. */
    tick(0,0,7,-1);visible(0x8888);
    tick(0,0,7,0x9999);visible(0x9999);
    /* A late completion from the previous generation cannot replace front. */
    kim_avatar_request(&p,1,0,8);
    unsigned late,le;assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&late,&le));
    kim_avatar_request(&p,2,0,9);
    kim_avatar_finish_decode(&p,late,le,true);
    tick(2,0,9,-1);visible(0x9999);
    assert(atomic_load(&p.discarded)==1);
    /* Permanent decode failure retains the latest validated video frame. */
    tick(2,0,9,0);visible(0x9999);
    return 0;
}
