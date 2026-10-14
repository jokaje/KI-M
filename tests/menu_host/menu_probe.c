/* Real LVGL 9.5.0 display/indev harness, no ESP I/O or secrets. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stubs.h"
#include "../../firmware/main/voice.h"
kim_touch_ptt_t kim_touch_ptt;
bool kim_runtime_voice_busy(void) { return false; }
#include "../../firmware/managed_components/lvgl__lvgl/src/widgets/keyboard/lv_keyboard_private.h"
#define KIM_MENU_HOST_TEST 1
#include "../../firmware/main/kim_menu.c"

static unsigned char rgb[320 * 240 * 3];
static int px, py;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *buf) {
    for (int y=a->y1; y<=a->y2; ++y) for (int x=a->x1; x<=a->x2; ++x) {
        unsigned v = ((uint16_t *)buf)[(y-a->y1)*(a->x2-a->x1+1)+(x-a->x1)];
        size_t i = (size_t)(y*320+x)*3;
        rgb[i] = ((v>>11)&31)*255/31;
        rgb[i+1] = ((v>>5)&63)*255/63;
        rgb[i+2] = (v&31)*255/31;
    }
    lv_display_flush_ready(d);
}
static void input(lv_indev_t *d, lv_indev_data_t *data) {
    (void)d;
    data->point.x=px; data->point.y=py; data->state=pointer_state;
}
static void frame(void) { lv_tick_inc(40); lv_timer_handler(); }
static void touch(int x, int y) {
    px=x; py=y; pointer_state=LV_INDEV_STATE_PRESSED; frame();frame();
    pointer_state=LV_INDEV_STATE_RELEASED; frame();frame();
}
static void save(const char *path) {
    FILE *f=fopen(path,"wb"); assert(f);
    fprintf(f,"P6\n320 240\n255\n"); assert(fwrite(rgb,1,sizeof rgb,f)==sizeof rgb);
    fclose(f);
}
static int colored(int x1,int y1,int x2,int y2) {
    int n=0;
    for(int y=y1;y<y2;y++)for(int x=x1;x<x2;x++) {
        size_t i=(size_t)(y*320+x)*3;
        if(rgb[i] > 45 && rgb[i+1] > 45 && rgb[i+2] > 45) n++;
    }
    return n;
}
int main(int argc, char **argv) {
    assert(argc==2);
    lv_init();
    lv_display_t *d=lv_display_create(320,240);
    static uint16_t draw[320*240];
    lv_display_set_buffers(d,draw,NULL,sizeof draw,LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d,flush);
    lv_indev_t *in=lv_indev_create(); lv_indev_set_type(in,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in,input); lv_indev_set_display(in,d);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_t *ptt=lv_obj_create(screen); lv_obj_set_size(ptt,320,240);
    kim_menu_init(screen,ptt);
    frame();
    /* A released touch can already have a submitted turn in flight. The
     * menu callback must cancel it before the next main-loop tick. */
    kim_touch_press(&kim_touch_ptt);
    unsigned touch_turn=atomic_load(&kim_touch_ptt.epoch);
    kim_touch_release(&kim_touch_ptt);
    assert(kim_touch_submit(&kim_touch_ptt,touch_turn,3200,false,true));
    touch(275,15); assert(menu_open);
    assert(kim_touch_cancelled(&kim_touch_ptt,touch_turn));
    assert(!kim_touch_submit(&kim_touch_ptt,touch_turn,3200,false,true));
    touch(200,70); assert(page_id==PAGE_WIFI);
    /* Inject a nonsecret synthetic scan result, as the real worker would. */
    ap_count=1; strcpy(aps[0].ssid,"TEST-NET"); aps[0].secure=true;
    show(PAGE_WIFI); frame();
    touch(70,98); assert(page_id==PAGE_PASSWORD);
    frame(); save(argv[1]);
    lv_area_t a; lv_obj_get_coords(page,&a); printf("page_coords=%d,%d,%d,%d scroll=%d ",a.x1,a.y1,a.x2,a.y2,lv_obj_get_scroll_y(page));
    for(unsigned i=0;i<lv_obj_get_child_count(page);i++) {lv_obj_t *c=lv_obj_get_child(page,i);lv_obj_get_coords(c,&a);printf("child%u=[%d,%d-%d,%d] ",i,a.x1,a.y1,a.x2,a.y2);} puts("");
    printf("display=%dx%d keyboard_hidden=%d keyboard_parent_overlay=%d ",lv_display_get_horizontal_resolution(d),lv_display_get_vertical_resolution(d),lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN),lv_obj_get_parent(keyboard)==overlay);
    printf("keyboard_region_light_pixels=%d\n",colored(0,116,320,216));
    assert(colored(0,116,320,216)>10000);
    lv_obj_get_coords(keyboard,&a); lv_buttonmatrix_t *matrix=(lv_buttonmatrix_t *)keyboard; printf("keyboard_coords=%d,%d-%d,%d button_count=%u key1=%s\n",a.x1,a.y1,a.x2,a.y2,matrix->btn_cnt,lv_buttonmatrix_get_button_text(keyboard,1));
    assert(a.y1 >= 105 && a.y2 < 217);
    assert(!lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    lv_area_t ka=matrix->button_areas[1]; printf("key1_area=%d,%d-%d,%d\n",ka.x1,ka.y1,ka.x2,ka.y2);
    touch(a.x1+(ka.x1+ka.x2)/2,a.y1+(ka.y1+ka.y2)/2);
    printf("typed_nonsecret_chars=%zu\n",strlen(lv_textarea_get_text(password_box)));
    assert(!lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    assert(strlen(lv_textarea_get_text(password_box))>0);
    lv_obj_t *old_password=password_box;
    show(PAGE_HOME); frame();
    assert(keyboard==NULL && password_box==NULL);
    assert(!lv_obj_is_valid(old_password));
    show(PAGE_SETTINGS); frame(); assert(keyboard==NULL);
    show(PAGE_BLE); frame(); assert(page_id==PAGE_BLE && ble_line && menu_open);
    touch(70,170); assert(ble_queued==1 && ble_state==KIM_BLE_PENDING);
    kim_menu_tick(false); frame();
    assert(strstr(lv_label_get_text(ble_line),"startet"));
    printf("BLE pending LVGL click=1 deferred_radio=1 label=%s\n",lv_label_get_text(ble_line));
    assert(!kim_menu_recording_allowed());
    show(PAGE_HOME); frame(); assert(page_id==PAGE_HOME && ble_line==NULL && ble_closes>0);
    touch(70,175); assert(page_id==PAGE_ANIMATION);
    frame(); assert(colored(0,70,320,150)>1000);
    touch(70,112); assert(!menu_open && kim_menu_emotion()==1);
    lv_tick_inc(6100); assert(kim_menu_emotion()==0);
    gate_step(lv_tick_get(),false,false);
    touch(275,15); assert(menu_open);
    show(PAGE_ANIMATION); frame();
    touch(210,112); assert(!menu_open && kim_menu_emotion()==2);
    gate_sleep(true);
    sleep_started=lv_tick_get();
    uint32_t phase_elapsed=0;
    assert(kim_menu_sleep_phase(&phase_elapsed)==1 && phase_elapsed==0);
    assert(kim_menu_emotion()==0);
    gate_sleep(false);
    gate_step(lv_tick_get(),false,false);
    touch(275,15); assert(menu_open);
    show(PAGE_PASSWORD); frame(); assert(keyboard && password_box);
    close_menu(); assert(!menu_open && keyboard==NULL && page==NULL);
    assert(!kim_menu_recording_allowed()); /* stable release still required */
    gate_step(lv_tick_get(),false,false);
    assert(kim_menu_recording_allowed());
    /* A held BOOT input also loses ownership immediately on menu entry;
     * its later stable release cannot submit the cancelled turn. */
    kim_boot_sample(&kim_touch_ptt,false,100);
    kim_boot_sample(&kim_touch_ptt,false,130);
    kim_boot_sample(&kim_touch_ptt,true,140);
    kim_boot_sample(&kim_touch_ptt,true,170);
    unsigned boot_turn=atomic_load(&kim_touch_ptt.epoch);
    assert(kim_touch_recording(&kim_touch_ptt,boot_turn));
    touch(275,15); assert(menu_open);
    assert(kim_touch_cancelled(&kim_touch_ptt,boot_turn));
    assert(!atomic_load(&kim_touch_ptt.held));
    kim_boot_sample(&kim_touch_ptt,false,180);
    kim_boot_sample(&kim_touch_ptt,false,210);
    assert(!kim_touch_submit(&kim_touch_ptt,boot_turn,3200,false,true));
    close_menu();
    assert(!kim_menu_recording_allowed());
    puts("PASS: visible keyboard, key touch, lifecycle, immediate touch/BOOT PTT cancellation, release gate");
    return 0;
}
