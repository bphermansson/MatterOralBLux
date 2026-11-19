/*
 * Minimal Telegram Bot sender.
 *
 * Wi-Fi itself is owned by Matter (ESP-Matter provisions the station and
 * stores the credentials in NVS). We therefore never call esp_wifi_init()
 * here; messages simply wait until the Matter-managed station has an IP.
 *
 * SPDX-License-Identifier: MIT
 */

#include "telegram.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "telegram";

/* Supplied by EMBED_TXTFILES in main/CMakeLists.txt. */
extern const char telegram_certificate_pem_start[] asm("_binary_telegram_certificate_pem_start");
extern const char telegram_certificate_pem_end[] asm("_binary_telegram_certificate_pem_end");

#define TELEGRAM_QUEUE_LEN 8
#define TELEGRAM_MAX_TEXT 160
#define TELEGRAM_TASK_STACK 8192
#define TELEGRAM_HTTP_TIMEOUT_MS 15000

typedef struct {
    char text[TELEGRAM_MAX_TEXT];
} telegram_msg_t;

static QueueHandle_t s_queue;

static bool wifi_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL || !esp_netif_is_netif_up(netif)) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }
    return ip_info.ip.addr != 0;
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= out_len) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n' || c == '\r') {
            out[o++] = ' ';
        } else if (c < 0x20) {
            /* Keep it simple: control characters are not expected. */
            out[o++] = ' ';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static esp_err_t telegram_post(const char *text)
{
    char url[256];
    char escaped[TELEGRAM_MAX_TEXT * 2];
    char post_data[512];

    json_escape(text, escaped, sizeof(escaped));

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage",
             CONFIG_ESP_TELEGRAM_TOKEN);

    const int len = snprintf(post_data, sizeof(post_data),
                             "{\"chat_id\":%s,\"text\":\"%s\"}",
                             CONFIG_ESP_TELEGRAM_CHAT_ID, escaped);
    if (len < 0 || len >= (int)sizeof(post_data)) {
        ESP_LOGE(TAG, "message too long");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = telegram_certificate_pem_start,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "could not create HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "Telegram returned HTTP %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static bool send_with_retry(const char *text)
{
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (telegram_post(text) == ESP_OK) {
            ESP_LOGI(TAG, "sent: %s", text);
            return true;
        }
        ESP_LOGW(TAG, "send attempt %d failed", attempt);
        if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(5000 * attempt));
        }
    }
    return false;
}

static bool wifi_ip_string(char *out, size_t out_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL || !esp_netif_is_netif_up(netif)) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    return esp_ip4addr_ntoa(&ip_info.ip, out, (int)out_len) != NULL;
}

static void telegram_task(void *arg)
{
    (void)arg;
    telegram_msg_t msg;
    bool online_reported = false;

    for (;;) {
        /* Report connectivity once per boot. This runs after commissioning
         * has provisioned Wi-Fi, so it also covers the very first boot where
         * the boot message would otherwise be dropped waiting for an IP. */
        if (!online_reported) {
            char ip[16];
            if (wifi_ip_string(ip, sizeof(ip))) {
                char text[64];
                snprintf(text, sizeof(text), "Oral-B watcher online (%s)", ip);
                if (send_with_retry(text)) {
                    online_reported = true;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
            }
        }

        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(2000)) != pdTRUE) {
            continue;
        }

        /* A fresh boot can beat Wi-Fi by a few seconds; wait for Matter. Drop
         * the message if the device is not (yet) on a network, otherwise a
         * long offline period would produce a burst of stale notifications. */
        bool have_network = true;
        const TickType_t wait_start = xTaskGetTickCount();
        while (!wifi_has_ip()) {
            if (xTaskGetTickCount() - wait_start > pdMS_TO_TICKS(120000)) {
                have_network = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        if (!have_network) {
            ESP_LOGW(TAG, "no Wi-Fi, dropping: %s", msg.text);
            continue;
        }

        send_with_retry(msg.text);
    }
}

esp_err_t telegram_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(TELEGRAM_QUEUE_LEN, sizeof(telegram_msg_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(telegram_task, "telegram", TELEGRAM_TASK_STACK, NULL, 4,
                    NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool telegram_send(const char *message)
{
    if (s_queue == NULL || message == NULL || message[0] == '\0') {
        return false;
    }

    telegram_msg_t msg;
    strncpy(msg.text, message, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping: %s", msg.text);
        return false;
    }
    return true;
}
