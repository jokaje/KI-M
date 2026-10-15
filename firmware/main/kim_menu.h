#pragma once
#include <stdbool.h>
#ifndef KIM_MENU_GATE_TEST
#include "lvgl.h"
/* Initialize under the BSP display lock, after the full-screen PTT target exists.
 * Menu-open callback immediately cancels the shared PTT under the LVGL writer;
 * parent also gates touch and BOOT via kim_menu_recording_allowed() and uses
 * kim_settings_volume() when opening output. Tick precedes BOOT sampling. */
void kim_menu_init(lv_obj_t *screen, lv_obj_t *ptt_target);
/* True while the settings overlay is open. */
#endif
bool kim_menu_open(void);
/* Call under the BSP display lock at ~10 ms cadence, from the input owner. */
#ifndef KIM_MENU_GATE_TEST
void kim_menu_tick(bool voice_busy);
/* Only under LVGL writer; remote state is a visual hint, not a power command. */
void kim_menu_remote_sleep(void);
#endif
bool kim_menu_recording_allowed(void);
#ifndef KIM_MENU_GATE_TEST
int kim_menu_emotion(void);
int kim_menu_sleep_phase(uint32_t *elapsed_ms);
#endif
