#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_event.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_ota_ops.h"

#include "esp_partition.h"

#include "esp_http_client.h"
#include "esp_delta_ota.h"

#define HTTP_CHUNK_SIZE (2048)
#define BUFFSIZE 1024

static const char *TAG = "http_delta_ota";

static char ota_write_data[BUFFSIZE + 1] = { 0 };
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

const esp_partition_t *src, *dest;
esp_ota_handle_t ota_handle;

static int write_cb(void *arg_p, const uint8_t *buf_p, size_t size)
{
    if (size <= 0) {
        return -DELTA_INVALID_BUF_SIZE;
    }
    if (esp_ota_write(ota_handle, buf_p, size) != ESP_OK) {
        return -DELTA_WRITING_ERROR;
    }
    return DELTA_OK;
}

static int read_cb(void *arg_p, uint8_t *buf_p, size_t size)
{
    int *src_offset1 = (int *)arg_p;

    if (size <= 0) {
        return -DELTA_INVALID_BUF_SIZE;
    }
    if (esp_partition_read(src, *src_offset1, buf_p, size) != ESP_OK) {
        return -DELTA_READING_SOURCE_ERROR;
    }

    *src_offset1 += size;
    if (*src_offset1 >= src->size) {
        return -DELTA_OUT_OF_MEMORY;
    }

    return DELTA_OK;
}

static int seek_cb(void *arg_p, int offset)
{
    int *src_offset1 = (int *)arg_p;
    *src_offset1 +=offset;
    if (*src_offset1 >= src->size) {
        return -DELTA_SEEKING_ERROR;
    }

    return DELTA_OK;
}

static void reboot(void)
{
    ESP_LOGI(TAG, "Rebooting in 5 seconds...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    esp_restart();
}

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void __attribute__((noreturn)) task_fatal_error(void)
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    (void)vTaskDelete(NULL);
    while (1) {
        ;
    }
}

static void ota_example_task(void *pvParameter)
{
    esp_err_t err;

    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPG_URL,
        .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
    };
#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        task_fatal_error();
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        task_fatal_error();
    }
    esp_http_client_fetch_headers(client);

    src = esp_ota_get_running_partition();
    dest = esp_ota_get_next_update_partition(NULL);

    if (src == NULL || dest == NULL) {
        task_fatal_error();
    }

    if (src->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX ||
        dest->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        task_fatal_error();
    }

    if (esp_ota_begin(dest, OTA_SIZE_UNKNOWN, &(ota_handle)) != ESP_OK) {
        task_fatal_error();
    }
    esp_delta_ota_cfg_t cfg = {
        .read_cb = &read_cb,
        .seek_cb = &seek_cb,
        .write_cb = &write_cb,
        .src_offset = 0,
    };
    esp_delta_ota_handle_t handle = delta_ota_set_cfg(&cfg);
    if (handle == NULL) {
        ESP_LOGE(TAG, "delta_ota_set_cfg failed");
        task_fatal_error();
    }
    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            task_fatal_error();
        } else if (data_read > 0) {
            if (esp_delta_ota_feed_patch(handle, (const uint8_t *)ota_write_data, data_read) < 0) {
                ESP_LOGE(TAG, "Error while applying patch");
                task_fatal_error();
            }
        } else if (data_read == 0) {
            if (esp_http_client_is_complete_data_received(client) == true) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                break;
            }
        }
    }
    esp_delta_ota_finish(handle);
    esp_delta_ota_deinit(handle);
    esp_ota_set_boot_partition(dest);
    http_cleanup(client);
    reboot();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initialising WiFi Connection...");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    xTaskCreate(&ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);
}
