/* KI-M BOX-3 voice image. Muse SDK BOX-3 board adapter was the hardware reference;
 * the Muse pairing, network, tunneling, updater and Jollybot are not linked. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pixel_avatar.h"
#include "nvs_flash.h"
#include "kim_settings.h"
#include "kim_wifi.h"
#include "kim_menu.h"
#include "kim_ota.h"
#include "kim_runtime.h"
#include "kim_ble.h"
#if __has_include("config_local.h")
#include "config_local.h"
#else
#include "config_local.example.h"
#endif

#define ART_SIZE 192
extern const uint8_t sprites_start[] asm("_binary_sprites_bin_start");
extern const uint8_t sprites_end[] asm("_binary_sprites_bin_end");
extern const uint8_t masks_start[] asm("_binary_sprites_mask_bin_start");
extern const uint8_t masks_end[] asm("_binary_sprites_mask_bin_end");
static const char *TAG = "ki-m";
static uint16_t *pixels;
static lv_obj_t *image;
static lv_obj_t *status;
static lv_image_dsc_t descriptor;

#include "voice.h"
#include "box_3_microphone.h"
static const char *const names[] = {"Neutral", "Idle", "Blink", "Listening", "Thinking", "Speaking", "Happy", "Offline", "Sleep"};

/* BSP display_start wires a real GT911/TT21100 touch indev to LVGL. This
 * transparent top-level target covers every screen coordinate, including
 * outside the sprite. Never do codec or network work on the LVGL task. */
static void touch_event(lv_event_t *event) {
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && kim_menu_recording_allowed()) kim_touch_press(&kim_touch_ptt);
    else if (code == LV_EVENT_RELEASED) kim_touch_release(&kim_touch_ptt);
    else if (code == LV_EVENT_PRESS_LOST) kim_touch_lost(&kim_touch_ptt);
}


void app_main(void) {
    ESP_LOGI(TAG, "KI-M pixel-avatar candidate; touch/BOOT PTT; Wi-Fi/settings/OTA/BLE");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(kim_settings_init());
    if ((size_t)(sprites_end - sprites_start) != KIM_SPRITE_TOTAL_FRAMES * KIM_SPRITE_FRAME_BYTES ||
        (size_t)(masks_end - masks_start) != KIM_SPRITE_TOTAL_FRAMES * 512) {
        ESP_LOGE(TAG, "pixel sprite pack size mismatch");
        return;
    }
    pixels = heap_caps_malloc(ART_SIZE * ART_SIZE * sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) { ESP_LOGE(TAG, "PSRAM allocation failed"); return; }


    ESP_ERROR_CHECK(bsp_i2c_init());
    lv_display_t *disp = bsp_display_start();
    if (!disp) { ESP_LOGE(TAG, "LCD start failed"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(kim_settings_brightness()));
    if (!bsp_display_lock(1000)) { ESP_LOGE(TAG, "LCD lock failed"); return; }
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101828), 0);
    /* Initialize the exact source before LVGL first installs it. */
    kim_pixel_compose(pixels, (const uint16_t *)(sprites_start + 45 * KIM_SPRITE_FRAME_BYTES),
                      masks_start + 45 * 512, 0x10c5);
    descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor.header.w = ART_SIZE;
    descriptor.header.h = ART_SIZE;
    descriptor.header.stride = ART_SIZE * 2;
    descriptor.data_size = ART_SIZE * ART_SIZE * 2;
    descriptor.data = (const uint8_t *)pixels;
    image = lv_image_create(screen);
    lv_image_set_src(image, &descriptor);
    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);
    status = lv_label_create(screen);
    lv_label_set_text(status, "KI-M | Offline");
    lv_obj_set_style_text_color(status, lv_color_hex(0xffffff), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Hold screen or BOOT | Mute respected");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xbec9d8), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_t *touch = lv_obj_create(screen);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, lv_pct(100), lv_pct(100));
    lv_obj_align(touch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch, touch_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch, touch_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(touch, touch_event, LV_EVENT_PRESS_LOST, NULL);
    kim_menu_init(screen, touch);
    bsp_display_unlock();
    /* GPIO0 is the active-low BOOT button. No interrupt/task writes PTT;
     * polling below holds the same LVGL lock as touch event callbacks. */
    gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_config));
    ESP_ERROR_CHECK(box_3_microphone_init());
    ESP_ERROR_CHECK(kim_wifi_start(KIM_WIFI_SSID, KIM_WIFI_PASSWORD));
    /* The BLE controller and host are started only by the physical menu Start. */
    esp_err_t ble_err = kim_ble_init();
    if (ble_err != ESP_OK) ESP_LOGW(TAG, "BLE setup unavailable: %s", esp_err_to_name(ble_err));
    esp_err_t ota_err = kim_ota_init();
    if (ota_err != ESP_OK) ESP_LOGW(TAG, "Manual OTA unavailable (unprovisioned or partition invalid): %s", esp_err_to_name(ota_err));
    BaseType_t task_result = xTaskCreatePinnedToCore(kim_voice_task, "kim_voice", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "voice task create=%ld", (long)task_result);
    int last_mute = -1;
    uint32_t label_at = 0, bank_started = 0, next_blink = 2800;
    int previous_bank = -1;
    unsigned shown_frame = 45;
    while (1) {
        kim_voice_poll_mute();
        int mute = gpio_get_level(BSP_BUTTON_MUTE_IO);
        if (mute != last_mute) {
            ESP_LOGI(TAG, "mute GPIO1=%d online=%d (0=muted)", mute, kim_is_online);
            last_mute = mute;
        }
        if (bsp_display_lock(1000)) {
            kim_menu_tick(kim_runtime_voice_busy());
            kim_boot_sample(&kim_touch_ptt, gpio_get_level(GPIO_NUM_0) == 0,
                            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
            if (!kim_menu_recording_allowed()) kim_ptt_cancel(&kim_touch_ptt);

            /* Device state always outranks reply emotion. Render at bounded
             * frame changes only; LVGL owns this buffer under its writer lock. */
            int state = kim_voice_display_state();
            if (state == KIM_IDLE && !atomic_load(&kim_backend_healthy)) state = KIM_OFFLINE;
            if (state < KIM_NEUTRAL || state > KIM_SLEEP) state = KIM_IDLE;
            uint32_t now_ms = lv_tick_get();
            uint32_t sleep_elapsed = 0;
            int sleep_phase = kim_menu_sleep_phase(&sleep_elapsed);
            int visual_state = sleep_phase ? KIM_SLEEP : state;
            int emotion = kim_menu_emotion();
            if (!emotion && state == KIM_IDLE) emotion = kim_voice_display_emotion();
            int blink = state == KIM_IDLE && !emotion && (int32_t)(now_ms - next_blink) >= 0;
            kim_anim_bank_t bank = kim_pixel_bank(visual_state, emotion, blink);
            if (bank != previous_bank) { previous_bank = bank; bank_started = now_ms; }
            if (blink && now_ms - bank_started >= 220) next_blink = now_ms + 2500 + (now_ms % 2000);
            unsigned frame = kim_pixel_frame(bank, now_ms - bank_started,
                                             state == KIM_SPEAKING ? kim_voice_display_mouth() : 0);
            if (frame != shown_frame) {
                kim_pixel_compose(pixels, (const uint16_t *)(sprites_start + frame * KIM_SPRITE_FRAME_BYTES),
                                  masks_start + frame * 512, 0x10c5);
                /* The descriptor pointer is unchanged: explicitly invalidate
                 * the image after writing while holding the LVGL writer lock. */
                lv_obj_invalidate(image);
                shown_frame = frame;
            }
            /* Status label updates on a bounded 100-ms cadence. */
            if ((uint32_t)(now_ms - label_at) >= 100) {
                label_at = now_ms;
            if (atomic_load(&kim_touch_ptt.held) && mute == 0)
                lv_label_set_text(status, "KI-M | Muted - unmute to talk");
            else if (atomic_load(&kim_touch_ptt.held) && !kim_is_online)
                lv_label_set_text(status, "KI-M | Offline - cannot record");
            else if (atomic_load(&kim_touch_ptt.held) && !atomic_load(&kim_backend_healthy))
                lv_label_set_text(status, "KI-M | Health check failed - cannot record");
            else if (atomic_load(&kim_touch_ptt.held) && !atomic_load(&kim_audio_ready))
                lv_label_set_text(status, "KI-M | Audio starting");
            else if (kim_reply_failed && state == KIM_IDLE)
                lv_label_set_text(status, gpio_get_level(BSP_BUTTON_MUTE_IO) == 0 ?
                                  "KI-M | Reply interrupted | Muted" : "KI-M | Reply interrupted");
            else if (kim_is_online && !atomic_load(&kim_backend_healthy))
                lv_label_set_text(status, "KI-M | Health check failed");
            else {
                const char *state_name = sleep_phase ? "Ruhe" :
                    (emotion == 1 && state == KIM_IDLE) ? "Lachen" :
                    (emotion == 2 && state == KIM_IDLE) ? "Weinen" : names[state];
                lv_label_set_text_fmt(status, "KI-M | %s%s", state_name,
                                      gpio_get_level(BSP_BUTTON_MUTE_IO) == 0 ? " | Muted" : "");
            }
            }
            bsp_display_unlock();
        }
        kim_ota_trial_poll(disp != NULL, atomic_load(&kim_audio_ready),
                           kim_wifi_connected(), atomic_load(&kim_backend_healthy));
        kim_ble_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
