/* Render actual converted 192x192 JPEG-derived RGB565 pixels with the same
 * LVGL descriptor, coordinates and status/menu chrome as the device. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "lvgl.h"
static uint8_t rgb[320*240*3];
static uint16_t draw[320*240], avatar[192*192];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *buf) {
    for (int y=a->y1;y<=a->y2;y++) for (int x=a->x1;x<=a->x2;x++) {
        uint16_t v=((uint16_t*)buf)[(y-a->y1)*(a->x2-a->x1+1)+x-a->x1];
        unsigned i=(y*320+x)*3;
        rgb[i]=((v>>11)&31)*255/31;
        rgb[i+1]=((v>>5)&63)*255/63;
        rgb[i+2]=(v&31)*255/31;
    }
    lv_display_flush_ready(d);
}
int main(int argc,char **argv) {
    assert(argc==4);
    FILE *in=fopen(argv[1],"rb"); assert(in);
    assert(fread(avatar,1,sizeof avatar,in)==sizeof avatar);
    assert(fgetc(in)==EOF); fclose(in);
    lv_init();
    lv_display_t *d=lv_display_create(320,240);
    lv_display_set_buffers(d,draw,NULL,sizeof draw,LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d,flush);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x101828),0);
    static lv_image_dsc_t desc;
    desc.header.cf=LV_COLOR_FORMAT_RGB565;
    desc.header.w=192; desc.header.h=192; desc.header.stride=384;
    desc.data_size=sizeof avatar;desc.data=(uint8_t*)avatar;
    lv_obj_t *image=lv_image_create(screen);
    lv_image_set_src(image,&desc);
    lv_obj_align(image,LV_ALIGN_TOP_MID,0,0);
    lv_obj_t *status=lv_label_create(screen);
    lv_label_set_text(status,argv[3]);
    lv_obj_set_style_text_color(status,lv_color_hex(0xffffff),0);
    lv_obj_align(status,LV_ALIGN_BOTTOM_MID,0,-22);
    lv_obj_t *hint=lv_label_create(screen);
    lv_label_set_text(hint,"Hold screen or BOOT | Mute respected");
    lv_obj_set_style_text_color(hint,lv_color_hex(0xbec9d8),0);
    lv_obj_align(hint,LV_ALIGN_BOTTOM_MID,0,-3);
    lv_obj_t *menu=lv_button_create(screen);
    lv_obj_set_pos(menu,254,2);lv_obj_set_size(menu,64,36);
    lv_obj_t *menu_label=lv_label_create(menu);
    lv_label_set_text(menu_label,"Menue");lv_obj_center(menu_label);
    lv_tick_inc(50);lv_timer_handler();
    FILE *out=fopen(argv[2],"wb");assert(out);
    fprintf(out,"P6\n320 240\n255\n");
    assert(fwrite(rgb,1,sizeof rgb,out)==sizeof rgb);
    fclose(out);
    lv_area_t a;lv_obj_get_coords(image,&a);
    assert(a.x1==64 && a.y1==0 && a.x2==255 && a.y2==191);
    return 0;
}
