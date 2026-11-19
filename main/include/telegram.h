#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the Telegram delivery task. Messages are queued and sent once the
 * Matter-provisioned Wi-Fi station has an IP address, so this can be called
 * before the network is up.
 */
esp_err_t telegram_init(void);

/*
 * Queue a message for delivery. Returns false if the queue is full (messages
 * are intentionally dropped instead of blocking the caller).
 */
bool telegram_send(const char *message);

#ifdef __cplusplus
}
#endif
