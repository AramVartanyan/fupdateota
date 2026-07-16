/* Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>

#include "fupdateota.h"

#ifdef CONFIG_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#define OTA_TASK_PRIORITY    1
#define OTA_STACKSIZE        8 * 1024
#define OTA_TASK_NAME        "otaUpdateTask"

static const char *TAG = "Firmware update";

#ifdef CONFIG_USE_CERT_BUNDLE
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
#endif

fupdateota_status_t otaUpdateStatus = FW_UPG_STATUS_IDLE;

//#define OTA_URL_SIZE 256

static esp_err_t ValidateImageHeader(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info = {0};
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }

#ifndef CONFIG_SKIP_VERSION_CHECK
    // Update only if the new version is strictly newer (major.minor.patch) -> no downgrade
    int curr_major = 0, curr_minor = 0, curr_patch = 0;
    int new_major = 0, new_minor = 0, new_patch = 0;

    if (sscanf(running_app_info.version, "%d.%d.%d", &curr_major, &curr_minor, &curr_patch) < 3) {
        ESP_LOGE(TAG, "Failed to parse current version: %s", running_app_info.version);
        return ESP_FAIL;
    }
    if (sscanf(new_app_info->version, "%d.%d.%d", &new_major, &new_minor, &new_patch) < 3) {
        ESP_LOGE(TAG, "Failed to parse new version: %s", new_app_info->version);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "New firmware version: %s", new_app_info->version);

    if (new_major > curr_major) {
        return ESP_OK;
    } else if (new_major == curr_major) {
        if (new_minor > curr_minor) {
            return ESP_OK;
        } else if (new_minor == curr_minor && new_patch > curr_patch) {
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "Version up-to-date: Current %s >= New %s",
             running_app_info.version, new_app_info->version);
    return ESP_FAIL;
#else
    return ESP_OK;
#endif
}

void otaTask(void *pvParameter)
{
    //uint32_t memSize = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    //ESP_LOGI(TAG, "Free memmory size is %lu", memSize);
    
    otaUpdateStatus = FW_UPG_STATUS_UPGRADING;
    ESP_LOGI(TAG, "Starting OTA Update");
    esp_err_t ota_finish_err = ESP_OK;
    
    esp_http_client_config_t config = {
        .url = CONFIG_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem = (char *)server_cert_pem_start,
#endif /* CONFIG_USE_CERT_BUNDLE */
        .timeout_ms = CONFIG_OTA_RECV_TIMEOUT,
    };

#ifdef CONFIG_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif //CONFIG_SKIP_COMMON_NAME_CHECK

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    
    //memSize = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    //ESP_LOGI(TAG, "Free memmory size is %lu", memSize);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        goto ota_fail;
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
        goto ota_end;
    }
    err = ValidateImageHeader(&app_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image header verification failed");
        goto ota_end;
    }

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        // ESP_LOGD(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(https_ota_handle)) {
        // the OTA image was not completely received and user can customise the response to this situation.
        ESP_LOGE(TAG, "Firmware download incomplete or failed.");
        goto ota_fail;
    }

ota_end:
    if (https_ota_handle) {
        ota_finish_err = esp_https_ota_finish(https_ota_handle);
    }
    if ((err == ESP_OK) && (ota_finish_err == ESP_OK)) {
        otaUpdateStatus = FW_UPG_STATUS_SUCCESS;
        ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
        //Restart the accessory from the main code
        //esp_restart();
    } else {
        if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed %d", ota_finish_err);
        goto ota_fail;
    }
    
    vTaskDelete(NULL);
    return;
    
ota_fail:
    otaUpdateStatus = FW_UPG_STATUS_FAIL;
    vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);
    otaUpdateStatus = FW_UPG_STATUS_IDLE;
    vTaskDelete(NULL);
    
}

bool otaUpdate(void)
{
    xTaskCreate(&otaTask, OTA_TASK_NAME, OTA_STACKSIZE, NULL, OTA_TASK_PRIORITY, NULL);
    return true;
}

