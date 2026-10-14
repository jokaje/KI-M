/* KI-M BOX-3 local settings UI. UI callbacks enqueue work; Wi-Fi/OTA/NVS
 * operations run only on the worker, never from the LVGL event dispatcher. */
#include "kim_menu.h"
#include <stdint.h>
#include <string.h>

static bool menu_open, asleep, release_gate;
static uint32_t last_activity;
#ifndef KIM_MENU_GATE_TEST
static uint32_t sleep_started, wake_started, emotion_started;
static int emotion;
static bool waking;
static bool dimmed;
#endif
static void gate_open(bool open) { menu_open = open; release_gate = true; }
static void gate_sleep(bool sleep) { asleep = sleep; release_gate = true; }
static void gate_step(uint32_t now, bool touch, bool boot) {
    (void)now;
    if (!touch && !boot && !menu_open && !asleep) release_gate = false;
}
static bool sleep_due(uint32_t now, uint32_t last, int seconds, bool voice_busy,
                      bool touch, bool boot) {
    return seconds > 0 && !menu_open && !asleep && !voice_busy && !touch && !boot &&
           (uint32_t)(now - last) >= (uint32_t)seconds * 1000U;
}
bool kim_menu_open(void) { return menu_open; }
bool kim_menu_recording_allowed(void) { return !menu_open && !asleep && !release_gate; }

#ifdef KIM_MENU_GATE_TEST
void kim_menu_gate_test_reset(void) { menu_open = asleep = release_gate = false; last_activity = 0; }
void kim_menu_gate_test_open(bool open) { gate_open(open); }
void kim_menu_gate_test_sleep(bool sleep) { gate_sleep(sleep); }
void kim_menu_gate_test_step(uint32_t now, bool touch, bool boot) { gate_step(now, touch, boot); }
bool kim_menu_gate_test_sleep_due(uint32_t now, uint32_t last, int seconds, bool busy,
                                  bool touch, bool boot) {
    return sleep_due(now, last, seconds, busy, touch, boot);
}
#else
#include <stdio.h>
#include "voice.h"
#include "kim_runtime.h"
#ifndef KIM_MENU_HOST_TEST
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "kim_settings.h"
#include "kim_wifi.h"
#include "kim_ota.h"
#include "kim_trial.h"
#include "kim_ble.h"
#endif

static const char *MENU_TAG = "kim-menu";

#define MAX_APS 12
#define PANEL_BG 0x16263A
#define TEXT 0xFFFFFF
#define ACCENT 0x9CDBEE

typedef enum { OP_SCAN, OP_SAVE, OP_FORGET, OP_VOLUME, OP_BRIGHT, OP_SLEEP, OP_CHECK, OP_INSTALL } op_t;
typedef struct { op_t op; int value; char ssid[33]; char password[64]; } job_t;
typedef struct {
    op_t op; esp_err_t err; int value;
    int count; kim_wifi_ap_t aps[MAX_APS];
    char status[72];
} result_t;
static QueueHandle_t jobs, results;
static bool worker_ready;
static lv_obj_t *overlay, *menu_button, *page, *notice, *wake_cover, *ota_line, *wifi_line;
static lv_obj_t *password_box, *keyboard;
static lv_obj_t *volume_slider, *bright_slider, *sleep_slider;
static lv_obj_t *volume_value, *bright_value, *sleep_value;
static char selected_ssid[33];
static bool selected_saved, selected_secure;
static kim_wifi_ap_t aps[MAX_APS];
static int ap_count, current_brightness = 65, sleep_seconds;
static bool last_pressed;
static lv_obj_t *ble_line;
extern kim_trial_t kim_ota_trial;

typedef enum { PAGE_HOME, PAGE_SETTINGS, PAGE_WIFI, PAGE_PASSWORD, PAGE_CONFIRM_FORGET, PAGE_OTA, PAGE_CONFIRM_INSTALL, PAGE_BLE, PAGE_ANIMATION } page_t;
static page_t page_id;
int kim_menu_emotion(void) {
    if (menu_open || asleep || kim_runtime_voice_busy()) emotion = 0;
    if (emotion && (uint32_t)(lv_tick_get() - emotion_started) >= 6016) emotion = 0;
    return emotion;
}
int kim_menu_sleep_phase(uint32_t *elapsed_ms) {
    if (!asleep) return 0;
    *elapsed_ms = (uint32_t)(lv_tick_get() - (waking ? wake_started : sleep_started));
    return waking ? 2 : 1;
}

static void worker(void *unused) {
    (void)unused;
    job_t j;
    while (xQueueReceive(jobs, &j, portMAX_DELAY) == pdTRUE) {
        result_t r = {.op = j.op, .err = ESP_OK};
        switch (j.op) {
        case OP_SCAN:
            /* Wait for a NEW completed radio generation; never show stale APs
             * or claim that enqueueing a request means a completed scan. */
            unsigned generation = kim_wifi_scan_generation();
            r.err = kim_wifi_scan();
            if (r.err == ESP_OK) {
                TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(16000);
                while (kim_wifi_scan_generation() == generation && kim_wifi_scan_busy() &&
                       (int32_t)(xTaskGetTickCount() - until) < 0)
                    vTaskDelay(pdMS_TO_TICKS(100));
                if (kim_wifi_scan_generation() == generation) r.err = ESP_ERR_TIMEOUT;
            }
            if (r.err == ESP_OK) {
                r.count = kim_wifi_scan_results(r.aps, MAX_APS);
                kim_settings_network_t saved;
                for (int i = 0; i < kim_settings_network_count() && r.count < MAX_APS; ++i) {
                    if (!kim_settings_network_get(i, &saved)) continue;
                    bool present = false;
                    for (int n = 0; n < r.count; ++n)
                        if (!strcmp(saved.ssid, r.aps[n].ssid)) present = true;
                    if (!present) {
                        snprintf(r.aps[r.count].ssid, sizeof r.aps[r.count].ssid, "%s", saved.ssid);
                        r.aps[r.count].saved = true;
                        r.aps[r.count].secure = true;
                        ++r.count;
                    }
                    memset(&saved, 0, sizeof saved);
                }
            }
            break;
        case OP_SAVE:
        case OP_FORGET: {
            unsigned id = 0;
            r.err = j.op == OP_SAVE ? kim_wifi_save_tracked(j.ssid, j.password, &id) :
                                     kim_wifi_forget_tracked(j.ssid, &id);
            if (r.err == ESP_OK) {
                TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                while (!kim_wifi_operation_result(id, &r.err) &&
                       (int32_t)(xTaskGetTickCount() - until) < 0)
                    vTaskDelay(pdMS_TO_TICKS(20));
                if (!kim_wifi_operation_result(id, &r.err)) r.err = ESP_ERR_TIMEOUT;
            }
            break;
        }
        case OP_VOLUME: r.err = kim_settings_set_volume(j.value); break;
        case OP_BRIGHT: r.err = kim_settings_set_brightness(j.value); break;
        case OP_SLEEP: r.err = kim_settings_set_sleep_s(j.value); break;
        case OP_CHECK: r.err = kim_ota_check(); break;
        case OP_INSTALL: r.err = kim_ota_install(); break;
        }
        r.value = j.value;
        memset(j.password, 0, sizeof j.password);
        (void)xQueueSend(results, &r, portMAX_DELAY);
    }
}
static bool submit(op_t op, int value, const char *ssid, const char *password) {
    job_t job = {.op = op, .value = value};
    if (ssid) snprintf(job.ssid, sizeof job.ssid, "%s", ssid);
    if (password) snprintf(job.password, sizeof job.password, "%s", password);
    bool ok = worker_ready && xQueueSend(jobs, &job, 0) == pdTRUE;
    memset(job.password, 0, sizeof job.password);
    if (!ok && notice) lv_label_set_text(notice, "Beschaeftigt - erneut versuchen");
    return ok;
}
static void activity(void) { last_activity = lv_tick_get(); }
static void show(page_t p);
static void set_notice(const char *text) { if (notice) lv_label_set_text(notice, text); }
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_color(o, lv_color_hex(TEXT), 0);
    lv_obj_set_pos(o, x, y);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w,
                        lv_event_cb_t cb, intptr_t arg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 36);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)arg);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}
static void discard_page(void) {
    /* The keyboard belongs to the overlay, not to page: detach before its
     * textarea is deleted, and do not leave an off-screen input alive. */
    if (keyboard) {
        lv_keyboard_set_textarea(keyboard, NULL);
        lv_obj_delete(keyboard);
        keyboard = NULL;
    }
    if (page) { lv_obj_delete(page); page = NULL; }
    password_box = NULL;
}
static void close_menu(void) {
#ifndef KIM_MENU_HOST_TEST
    kim_ble_close();
#endif
    discard_page();
    kim_trial_menu(&kim_ota_trial, false);
    gate_open(false);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    activity();
}
static void menu_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(MENU_TAG, "menu_cb open=%d", menu_open);
    activity();
    if (menu_open) close_menu();
    else {
        /* The callback runs under LVGL's writer context. Invalidate even a
         * released turn before opening the overlay; the next main tick can
         * be too late to prevent a voice submit. */
        kim_ptt_cancel(&kim_touch_ptt);
        gate_open(true);
        kim_trial_menu(&kim_ota_trial, true);
        show(PAGE_HOME);
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay);
    }
}
static void nav_cb(lv_event_t *e) {
    activity();
    intptr_t action = (intptr_t)lv_event_get_user_data(e);
    if (action < 0) close_menu();
    else show((page_t)action);
}
static void slider_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(s);
    if (s == sleep_slider) {
        v = v == 0 ? 0 : ((v + 15) / 30) * 30;
        lv_label_set_text_fmt(sleep_value, v ? "%d s" : "Aus", v);
        submit(OP_SLEEP, v, NULL, NULL);
    } else if (s == volume_slider) {
        lv_label_set_text_fmt(volume_value, "%d%%", v);
        submit(OP_VOLUME, v, NULL, NULL);
    } else if (s == bright_slider) {
        lv_label_set_text_fmt(bright_value, "%d%%", v);
        submit(OP_BRIGHT, v, NULL, NULL);
    }
    activity();
}
static void wifi_cb(lv_event_t *e) {
    intptr_t n = (intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(MENU_TAG, "wifi_cb n=%d", (int)n);
    activity();
    if (n == -1) {
        if (submit(OP_SCAN, 0, NULL, NULL)) set_notice("Suche laeuft...");
        return;
    }
    if (n < 0 || n >= ap_count) return;
    snprintf(selected_ssid, sizeof selected_ssid, "%s", aps[n].ssid);
    selected_saved = aps[n].saved;
    selected_secure = aps[n].secure;
    show(PAGE_PASSWORD);
}
static void password_cb(lv_event_t *e) {
    intptr_t action = (intptr_t)lv_event_get_user_data(e);
    activity();
    if (action == 1) { show(PAGE_CONFIRM_FORGET); return; }
    const char *pass = selected_secure ? lv_textarea_get_text(password_box) : "";
    if (selected_secure && (!pass || !pass[0])) { set_notice("Passwort eingeben"); return; }
    if (submit(OP_SAVE, 0, selected_ssid, pass)) {
        lv_textarea_set_text(password_box, "");
        set_notice("Speichern laeuft...");
    }
}
static void forget_cb(lv_event_t *e) {
    activity();
    if ((intptr_t)lv_event_get_user_data(e) == 1) {
        if (submit(OP_FORGET, 0, selected_ssid, NULL)) set_notice("Entfernen laeuft...");
    } else show(PAGE_PASSWORD);
}
static void ota_cb(lv_event_t *e) {
    activity();
    if ((intptr_t)lv_event_get_user_data(e) == 1) {
        if (kim_ota_busy()) { set_notice("Update beschaeftigt"); return; }
        if (submit(OP_CHECK, 0, NULL, NULL)) set_notice("Pruefung gestartet...");
    } else show(PAGE_CONFIRM_INSTALL);
}
static void install_cb(lv_event_t *e) {
    activity();
    if ((intptr_t)lv_event_get_user_data(e) == 0) { show(PAGE_OTA); return; }
    if (kim_ota_busy() || !kim_ota_available()) { set_notice("Kein Update bereit"); return; }
    if (submit(OP_INSTALL, 0, NULL, NULL)) set_notice("Installation gestartet...");
}
static void ble_cb(lv_event_t *e) {
    activity();
    if ((intptr_t)lv_event_get_user_data(e) == 1) {
        if (kim_ble_open() != ESP_OK) set_notice("BLE nicht bereit / beschaeftigt");
    } else kim_ble_close();
}
static void emotion_cb(lv_event_t *e) {
    int chosen = (int)(intptr_t)lv_event_get_user_data(e);
    if (chosen < 1 || chosen > 2 || kim_runtime_voice_busy()) return;
    close_menu();
    emotion = chosen;
    emotion_started = lv_tick_get();
}
static void row(const char *title, int y, int min, int max, int val,
                lv_obj_t **slider, lv_obj_t **value) {
    label(page, title, 8, y);
    *slider = lv_slider_create(page);
    lv_obj_set_pos(*slider, 106, y + 3);
    lv_obj_set_size(*slider, 133, 15);
    lv_slider_set_range(*slider, min, max);
    lv_slider_set_value(*slider, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(*slider, slider_cb, LV_EVENT_RELEASED, NULL);
    *value = label(page, "", 243, y);
}
static void show(page_t p) {
    if (page_id == PAGE_BLE && p != PAGE_BLE) kim_ble_close();
    page_id = p;
    ESP_LOGI(MENU_TAG, "show page=%d", (int)p);
    discard_page();
    ota_line = wifi_line = NULL;
    ble_line = NULL;
    page = lv_obj_create(overlay);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 320, 215);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_style_bg_color(page, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    label(page, p == PAGE_HOME ? "KI-M Menue" : p == PAGE_SETTINGS ? "Einstellungen" :
          p == PAGE_WIFI ? "WLAN" : p == PAGE_PASSWORD ? "WLAN verbinden" :
          p == PAGE_CONFIRM_FORGET ? "Netz entfernen?" :
          p == PAGE_OTA ? "Software-Update" : p == PAGE_BLE ? "Bluetooth Einrichtung" :
          p == PAGE_ANIMATION ? "Animationen" : "Update installieren?", 8, 5);
    button(page, "Zurueck", 238, 1, 78, nav_cb, p == PAGE_HOME ? -1 : PAGE_HOME);
    switch (p) {
    case PAGE_HOME:
        button(page, "Ton / Display", 14, 54, 138, nav_cb, PAGE_SETTINGS);
        button(page, "WLAN", 165, 54, 138, nav_cb, PAGE_WIFI);
        button(page, "Update", 14, 108, 138, nav_cb, PAGE_OTA);
        button(page, "Bluetooth", 165, 108, 138, nav_cb, PAGE_BLE);
        button(page, "Schliessen", 165, 158, 138, nav_cb, -1);
        button(page, "Animationen", 14, 158, 138, nav_cb, PAGE_ANIMATION);
        break;
    case PAGE_ANIMATION:
        label(page, "Manuell, keine erkannten Gefuehle", 8, 42);
        button(page, "Lachen", 8, 98, 142, emotion_cb, 1);
        button(page, "Weinen", 162, 98, 142, emotion_cb, 2);
        break;
    case PAGE_SETTINGS:
        row("Lautst.", 47, 10, 80, kim_settings_volume(), &volume_slider, &volume_value);
        row("Helligk.", 93, 10, 100, kim_settings_brightness(), &bright_slider, &bright_value);
        row("Ruhezeit", 139, 0, 1800, kim_settings_sleep_s(), &sleep_slider, &sleep_value);
        lv_label_set_text_fmt(volume_value, "%d%%", kim_settings_volume());
        lv_label_set_text_fmt(bright_value, "%d%%", kim_settings_brightness());
        if (kim_settings_sleep_s()) lv_label_set_text_fmt(sleep_value, "%d s", kim_settings_sleep_s());
        else lv_label_set_text(sleep_value, "Aus");
        break;
    case PAGE_WIFI: {
        wifi_line = label(page, "WLAN...", 8, 39);
        button(page, "Suchen", 225, 37, 85, wifi_cb, -1);
        for (int i = 0; i < ap_count; ++i) {
            char text[46];
            snprintf(text, sizeof text, "%.25s %s", aps[i].ssid, aps[i].saved ? "[bekannt]" : "");
            button(page, text, 8, 80 + i * 39, 298, wifi_cb, i);
        }
        break;
    }
    case PAGE_PASSWORD:
        label(page, selected_ssid, 8, 38);
        if (selected_saved) button(page, "Entfernen", 225, 37, 86, password_cb, 1);
        password_box = lv_textarea_create(page);
        lv_obj_set_pos(password_box, 8, 68);
        lv_obj_set_size(password_box, 212, 35);
        lv_textarea_set_one_line(password_box, true);
        lv_textarea_set_max_length(password_box, 63);
        lv_textarea_set_password_mode(password_box, true);
        button(page, "Sichern", 226, 68, 85, password_cb, 0);

        /* LVGL keyboard constructor installs BOTTOM_MID alignment. A raw
         * set_pos(0,116) offsets that baseline below the 240px display;
         * align explicitly to TOP_LEFT so keys stay on-screen. */
        keyboard = lv_keyboard_create(overlay);
        lv_obj_set_size(keyboard, 320, 100);
        lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 0, 116);
        lv_keyboard_set_textarea(keyboard, password_box);
        lv_obj_move_foreground(keyboard);

        break;
    case PAGE_CONFIRM_FORGET:
        label(page, selected_ssid, 8, 54);
        button(page, "Abbrechen", 8, 112, 140, forget_cb, 0);
        button(page, "Ja, entfernen", 160, 112, 149, forget_cb, 1);
        break;
    case PAGE_OTA: {
        char state[72];
        kim_ota_status(state, sizeof state);
        ota_line = label(page, state, 8, 45);
        button(page, "Pruefen", 8, 100, 140, ota_cb, 1);
        button(page, "Installieren", 160, 100, 149, ota_cb, 2);
        break;
    }
    case PAGE_CONFIRM_INSTALL:
        label(page, "Jetzt installieren? Neustart moeglich.", 8, 50);
        button(page, "Abbrechen", 8, 110, 140, install_cb, 0);
        button(page, "Ja, starten", 160, 110, 149, install_cb, 1);
        break;
    case PAGE_BLE:
        label(page, "Nur 180 s nach Start sichtbar", 8, 40);
        ble_line = label(page, "BLE aus", 8, 95);
        button(page, "Start", 8, 153, 138, ble_cb, 1);
        button(page, "Abbrechen", 164, 153, 146, ble_cb, 0);
        break;
    }
    if (notice) lv_label_set_text(notice, "");
}
static void wake_cb(lv_event_t *e) {
    (void)e;
    if (!asleep) return;
    if (!waking && bsp_display_brightness_set(current_brightness) == ESP_OK) {
        waking = true;
        dimmed = false;
        wake_started = lv_tick_get();
        activity();
    }
}
void kim_menu_init(lv_obj_t *screen, lv_obj_t *ptt_target) {
    (void)ptt_target; /* Created after PTT target: foreground UI intercepts input. */
    menu_open = asleep = release_gate = waking = dimmed = false;
    current_brightness = kim_settings_brightness();
    sleep_seconds = kim_settings_sleep_s();
    activity();
    jobs = xQueueCreate(4, sizeof(job_t));
    results = xQueueCreate(4, sizeof(result_t));
    worker_ready = jobs && results &&
        xTaskCreate(worker, "kim_menu_io", 4096, NULL, 3, NULL) == pdPASS;
    menu_button = button(screen, "Menue", 254, 2, 64, menu_cb, 0);
    lv_obj_move_foreground(menu_button);
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 320, 240);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    notice = lv_label_create(overlay);
    lv_obj_set_style_text_color(notice, lv_color_hex(ACCENT), 0);
    lv_obj_set_pos(notice, 5, 218);
    lv_label_set_text(notice, "");
    wake_cover = lv_obj_create(screen);
    lv_obj_remove_style_all(wake_cover);
    lv_obj_set_size(wake_cover, 320, 240);
    lv_obj_add_flag(wake_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wake_cover, wake_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
}
void kim_menu_tick(bool voice_busy) {
    uint32_t now = lv_tick_get();
#ifndef KIM_MENU_HOST_TEST
    int configured_brightness = kim_settings_brightness();
    if (configured_brightness != current_brightness &&
        bsp_display_brightness_set(configured_brightness) == ESP_OK)
        current_brightness = configured_brightness;
#endif
    bool pressed = false;
    for (lv_indev_t *dev = lv_indev_get_next(NULL); dev; dev = lv_indev_get_next(dev)) {
        if (lv_indev_get_type(dev) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(dev) == LV_INDEV_STATE_PRESSED) pressed = true;
    }
    bool boot = gpio_get_level(GPIO_NUM_0) == 0;
    if (pressed || boot || voice_busy || menu_open) activity();
    if (pressed != last_pressed) { last_pressed = pressed; activity(); }
    gate_step(now, pressed, boot);
    if (menu_open && page_id == PAGE_BLE && ble_line) {
        char pin[7];
        if (kim_ble_passkey(pin)) lv_label_set_text_fmt(ble_line, "Code: %s", pin);
        else {
            switch (kim_ble_state()) {
            case KIM_BLE_PENDING: lv_label_set_text(ble_line, "BLE startet..."); break;
            case KIM_BLE_ACTIVE: lv_label_set_text(ble_line, "KI-M Setup sichtbar"); break;
            case KIM_BLE_STOPPING: lv_label_set_text(ble_line, "BLE wird beendet..."); break;
            case KIM_BLE_FAILED: lv_label_set_text(ble_line, "BLE Fehler / beschaeftigt"); break;
            default: lv_label_set_text(ble_line, "BLE aus / abgelaufen"); break;
            }
        }
    }
    result_t r;
    while (results && xQueueReceive(results, &r, 0) == pdTRUE) {
        if (r.err != ESP_OK) {
            if (menu_open && page_id == PAGE_SETTINGS) {
                if (r.op == OP_VOLUME) {
                    lv_slider_set_value(volume_slider, kim_settings_volume(), LV_ANIM_OFF);
                    lv_label_set_text_fmt(volume_value, "%d%%", kim_settings_volume());
                } else if (r.op == OP_BRIGHT) {
                    lv_slider_set_value(bright_slider, kim_settings_brightness(), LV_ANIM_OFF);
                    lv_label_set_text_fmt(bright_value, "%d%%", kim_settings_brightness());
                } else if (r.op == OP_SLEEP) {
                    lv_slider_set_value(sleep_slider, kim_settings_sleep_s(), LV_ANIM_OFF);
                    if (kim_settings_sleep_s()) lv_label_set_text_fmt(sleep_value, "%d s", kim_settings_sleep_s());
                    else lv_label_set_text(sleep_value, "Aus");
                }
            }
            if (menu_open) lv_label_set_text_fmt(notice, "Fehler %s: %s", r.op == OP_SCAN || r.op == OP_SAVE || r.op == OP_FORGET ? "WLAN" :
                r.op == OP_CHECK || r.op == OP_INSTALL ? "Update" : "Speichern", esp_err_to_name(r.err));
        } else {
            if (r.op == OP_SCAN) {
                ap_count = r.count > MAX_APS ? MAX_APS : r.count;
                memcpy(aps, r.aps, (size_t)ap_count * sizeof aps[0]);
                if (menu_open && page_id == PAGE_WIFI) show(PAGE_WIFI);
            } else if (r.op == OP_BRIGHT) {
                current_brightness = r.value;
                if (bsp_display_brightness_set(current_brightness) != ESP_OK) set_notice("Fehler: Display");
            } else if (r.op == OP_SLEEP) sleep_seconds = r.value;
            if (menu_open && notice && r.op != OP_SCAN)
                set_notice(r.op == OP_SAVE || r.op == OP_FORGET ?
                           "WLAN dauerhaft gespeichert" : "Auftrag angenommen");
        }
    }
    if (menu_open && page_id == PAGE_WIFI && wifi_line) {
        char state[72];
        kim_wifi_status(state, sizeof state);
        lv_label_set_text(wifi_line, state);
    }
    if (menu_open && page_id == PAGE_OTA && ota_line) {
        char state[72];
        kim_ota_status(state, sizeof state);
        lv_label_set_text(ota_line, state);
    }
    if (sleep_due(now, last_activity, sleep_seconds, voice_busy, pressed, boot)) {
        gate_sleep(true);
        dimmed = false;
        sleep_started = now;
        lv_obj_remove_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(wake_cover);
    }
    if (asleep && !waking && !dimmed && (uint32_t)(now - sleep_started) >= 4000)
        dimmed = bsp_display_brightness_set(2) == ESP_OK;
    if (waking && (uint32_t)(now - wake_started) >= 4000) {
        waking = false;
        gate_sleep(false);
        lv_obj_add_flag(wake_cover, LV_OBJ_FLAG_HIDDEN);
        activity();
    }
    /* Wake with BOOT without ever passing that same hold through to recording. */
    if (asleep && boot) wake_cb(NULL);
}
#endif
