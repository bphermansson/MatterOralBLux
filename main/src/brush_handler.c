/*
 * Turn Oral-B state changes into Telegram messages.
 *
 * SPDX-License-Identifier: MIT
 */

#include "brush_handler.h"

#include <stdio.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oralb_ble.h"
#include "telegram.h"

void brush_handler_task(void *arg)
{
    (void)arg;

    uint8_t prev_state = ORALB_STATE_UNKNOWN;
    bool have_state = false;
    int last_percent = -1;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();

        oralb_ble_adv_t adv;
        const bool fresh =
            oralb_ble_get_adv(&adv) && (now_us - adv.seen_us < 30LL * 1000 * 1000);

        oralb_ble_status_t status;
        const bool have_battery =
            oralb_ble_get_status(&status) && status.battery.has_percent;
        const int percent = have_battery ? (int)status.battery.percent : -1;

        if (fresh) {
            if (have_state && adv.adv.state != prev_state) {
                char msg[96];
                if (percent >= 0) {
                    snprintf(msg, sizeof(msg), "Brush: %s, battery %d%%",
                             oralb_state_name(adv.adv.state), percent);
                } else {
                    snprintf(msg, sizeof(msg), "Brush: %s",
                             oralb_state_name(adv.adv.state));
                }
                telegram_send(msg);
                last_percent = percent;
            }
            prev_state = adv.adv.state;
            have_state = true;
        }

        /* Battery changes slowly; only report a real change. The first
         * reading is included in an idle message when idle, otherwise it is
         * folded into the next state message instead of sent at boot. */
        if (percent >= 0 && percent != last_percent) {
            if (fresh && adv.adv.state == 2) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Brush: idle, battery %d%%", percent);
                telegram_send(msg);
            } else if (last_percent >= 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Brush battery: %d%%", percent);
                telegram_send(msg);
            }
            last_percent = percent;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
