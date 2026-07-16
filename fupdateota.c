/* Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   Dual-SDK firmware update component.

   ESP32 / ESP-IDF: the streamed esp_https_ota_begin / get_img_desc /
   perform / finish flow, unchanged.

   ESP8266 (ESP8266_RTOS_SDK v3.4): updates are MANUAL only. A trigger from
   the app a request flag in NVS and restarts; on the next boot
   otaBootCheck() (called first thing in app_main) sees the flag, clears it
   (one-shot — no update loops), brings WiFi up with the stored credentials
   and performs the update BEFORE other code starts, on a clean, unfragmented
   heap. (A TLS handshake does not survive the fragmented heap left next to
   a running the main server: a memory-starved certificate verification fails
   as if the chain were untrusted, -0x2700 — confirmed experimentally.)

   Version safety on the ESP8266: esp_ota_get_partition_description() is
   unreliable there (the esp_app_desc_t is NOT at a fixed image offset — it
   sits at the start of the rodata segment, whose position varies with the
   code size), so:
     - the RUNNING version comes from esp_ota_get_app_description() (the
       linked-in descriptor, always correct);
     - the NEW image's descriptor is extracted WHILE the image streams into
       the passive partition, by walking the image's segment structure and
       capturing the segment whose data starts with the descriptor magic.
   The version comparison (ValidateImageHeader) then decides: not newer ->
   the download is aborted and the partition is never activated; newer ->
   esp_ota_set_boot_partition() runs only after the image is complete.
   The HTTP status is checked too, so an error page can never be mistaken
   for an image. The device restarts into the new firmware on success, or
   continues a completely normal startup otherwise.

   The public API (otaUpdate / otaUpdateStatus / otaBootCheck) is identical
   on both chips (otaBootCheck is a no-op on ESP32).
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

#ifdef CONFIG_IDF_TARGET_ESP8266
#include <freertos/semphr.h>
#include <esp_image_format.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <tcpip_adapter.h>
#include <esp8266/eagle_soc.h>
#else
#ifdef CONFIG_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif
#endif /* CONFIG_IDF_TARGET_ESP8266 */

#define OTA_TASK_PRIORITY    1
#ifdef CONFIG_IDF_TARGET_ESP8266
#define OTA_STACKSIZE        (6 * 1024)
#else
#define OTA_STACKSIZE        (8 * 1024)
#endif
#define OTA_TASK_NAME        "otaUpdateTask"

static const char *TAG = "Firmware update";

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

fupdateota_status_t otaUpdateStatus = FW_UPG_STATUS_IDLE;

/* Optional application hook for progress indication (status LED etc.).
 * The OTA component itself never touches GPIO. */
static void (*sIndicationHandler)(fupdateota_indication_t) = NULL;

void otaSetIndication(void (*handler)(fupdateota_indication_t event))
{
    sIndicationHandler = handler;
}

static void otaIndicate(fupdateota_indication_t event)
{
    if (sIndicationHandler) {
        sIndicationHandler(event);
    }
}

static esp_err_t ValidateImageHeader(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t running_app_info = {0};
#ifdef CONFIG_IDF_TARGET_ESP8266
    /* esp_ota_get_partition_description() is unreliable on the ESP8266 (the
     * descriptor is not at a fixed image offset). The linked-in descriptor
     * of the running app is authoritative. */
    running_app_info = *esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }
#endif

#ifndef CONFIG_SKIP_VERSION_CHECK
    // Parse and compare versions
    int curr_major = 0, curr_minor = 0, curr_patch = 0;
    int new_major = 0, new_minor = 0, new_patch = 0;

    // Parse current version (with basic error checking)
    if (sscanf(running_app_info.version, "%d.%d.%d", &curr_major, &curr_minor, &curr_patch) < 3) {
        ESP_LOGE(TAG, "Failed to parse current version: %s", running_app_info.version);
        return ESP_FAIL;
    }

    // Parse new version
    if (sscanf(new_app_info->version, "%d.%d.%d", &new_major, &new_minor, &new_patch) < 3) {
        ESP_LOGE(TAG, "Failed to parse new version: %s", new_app_info->version);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "New firmware version: %s", new_app_info->version);

    // Compare versions
    if (new_major > curr_major) {
        return ESP_OK;
    } else if (new_major == curr_major) {
        if (new_minor > curr_minor) {
            return ESP_OK;
        } else if (new_minor == curr_minor && new_patch > curr_patch) {
            return ESP_OK;
        }
    }
    // If we get here, the version isn't newer
    ESP_LOGE(TAG, "Version up-to-date: Current %s >= New %s",
             running_app_info.version, new_app_info->version);
    return ESP_FAIL;
#else
    return ESP_OK;
#endif //CONFIG_SKIP_VERSION_CHECK
}

#ifdef CONFIG_IDF_TARGET_ESP8266

/* ============================ ESP8266 variant ============================ */

/* NVS bookkeeping for the boot-time update request. */
#define OTA_NVS_NAMESPACE "fwup"
#define OTA_NVS_BOOT_KEY  "boot_req"

#define OTA_BUF_SIZE      1024

#ifndef ESP_APP_DESC_MAGIC_WORD
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
#endif

/* ---- Streaming image parser ----
 * Walks the on-wire image structure (8-byte image header, then per segment
 * an 8-byte header followed by its data) and captures the esp_app_desc_t
 * from the segment whose data STARTS with the descriptor magic word. Only
 * segment starts are examined, so the magic constant embedded in the code
 * (the comparison in this very function, for instance) can never produce a
 * false match. */
typedef struct {
    enum { P_IMG_HDR, P_SEG_HDR, P_SEG_DATA, P_DONE } state;
    uint8_t  hdr[8];
    uint8_t  hdr_have;
    uint8_t  seg_count;
    uint8_t  seg_index;
    uint32_t seg_data_left;
    int      desc_fill;       /* bytes captured into desc; -1 = this segment rejected */
    bool     desc_done;
    esp_app_desc_t desc;
} ota_img_parser_t;

static void ota_parser_init(ota_img_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = P_IMG_HDR;
}

static void ota_parser_feed(ota_img_parser_t *p, const uint8_t *buf, int len)
{
    while (len > 0 && p->state != P_DONE) {
        switch (p->state) {
        case P_IMG_HDR:
        case P_SEG_HDR: {
            int take = 8 - p->hdr_have;
            if (take > len) take = len;
            memcpy(p->hdr + p->hdr_have, buf, take);
            p->hdr_have += take;
            buf += take;
            len -= take;
            if (p->hdr_have < 8) break;
            if (p->state == P_IMG_HDR) {
                p->seg_count = p->hdr[1];              /* segment count */
                p->state = P_SEG_HDR;
            } else {
                p->seg_data_left = (uint32_t)p->hdr[4] | ((uint32_t)p->hdr[5] << 8) |
                                   ((uint32_t)p->hdr[6] << 16) | ((uint32_t)p->hdr[7] << 24);
                p->desc_fill = p->desc_done ? -1 : 0;  /* try capturing this segment */
                p->state = P_SEG_DATA;
            }
            p->hdr_have = 0;
            break;
        }
        case P_SEG_DATA: {
            uint32_t take = p->seg_data_left;
            if ((uint32_t)len < take) take = len;

            if (p->desc_fill >= 0 && !p->desc_done) {
                uint32_t want = sizeof(esp_app_desc_t) - p->desc_fill;
                uint32_t cp = (take < want) ? take : want;
                memcpy((uint8_t *)&p->desc + p->desc_fill, buf, cp);
                p->desc_fill += cp;
                /* As soon as the first field is present, keep only a segment
                 * that really starts with the descriptor magic. */
                if (p->desc_fill >= 4 && p->desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                    p->desc_fill = -1;
                } else if (p->desc_fill == sizeof(esp_app_desc_t)) {
                    p->desc_done = true;
                }
            }

            p->seg_data_left -= take;
            buf += take;
            len -= take;
            if (p->seg_data_left == 0) {
                p->seg_index++;
                p->state = (p->seg_index >= p->seg_count) ? P_DONE : P_SEG_HDR;
            }
            break;
        }
        default:
            return;
        }
    }
}

static void otaBootFlagSet(uint8_t val)
{
    nvs_handle handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, OTA_NVS_BOOT_KEY, val);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static uint8_t otaBootFlagGet(void)
{
    nvs_handle handle;
    uint8_t val = 0;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, OTA_NVS_BOOT_KEY, &val);
        nvs_close(handle);
    }
    return val;
}

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* Signals app_main to continue the normal startup when no update happened. */
static SemaphoreHandle_t sBootCheckDone;

/* Force a reset by reprogramming the hardware watchdog — the byte-identical
 * register sequence of the SDK panic handler's hardware_restart()
 * (freertos/port/esp8266/panic.c). This is the ONE reset path proven to work
 * on this hardware in the middle of a full HAP + mDNS session storm (every
 * panic reboot goes through it): shortest period, pure-reset response mode,
 * enable — and the watchdog fires regardless of what any task is doing.
 * esp_restart() is useless here (it synchronously waits on the WiFi task and
 * hangs after a HAP lifecycle), and rom_software_reboot() proved to be no
 * hardware reset at all. */
static void otaHardwareRestart(void)
{
    portDISABLE_INTERRUPTS();
    CLEAR_WDT_REG_MASK(WDT_CTL_ADDRESS, BIT0);
    WDT_REG_WRITE(WDT_OP_ADDRESS, 1);
    WDT_REG_WRITE(WDT_OP_ND_ADDRESS, 1);
    SET_PERI_REG_BITS(PERIPHS_WDT_BASEADDR + WDT_CTL_ADDRESS,
                      WDT_CTL_RSTLEN_MASK, 7 << WDT_CTL_RSTLEN_LSB, 0);
    SET_PERI_REG_BITS(PERIPHS_WDT_BASEADDR + WDT_CTL_ADDRESS,
                      WDT_CTL_RSPMOD_MASK, 0 << WDT_CTL_RSPMOD_LSB, 0);
    SET_PERI_REG_BITS(PERIPHS_WDT_BASEADDR + WDT_CTL_ADDRESS,
                      WDT_CTL_EN_MASK, 1 << WDT_CTL_EN_LSB, 0);
    while (1) { }
}

/* Manual trigger: set the request flag and restart into the boot-time
 * update. The 2 s delay lets the HomeKit response / event for the
 * triggering write reach the controller before the connection drops. */
static void otaRestartTask(void *pvParameter)
{
    (void)pvParameter;
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Restarting to run the requested update on a clean heap");
    vTaskDelay(100 / portTICK_PERIOD_MS);   // let the log line drain
    otaHardwareRestart();
}

/* Shut the boot-time WiFi back down so app_wifi can initialise it fresh —
 * including the default event loop, so app_wifi's own
 * esp_event_loop_create_default() succeeds untouched. This is safe because
 * the tcpip glue handlers are (re)registered inside every esp_wifi_init()
 * (tcpip_adapter_set_default_wifi_handlers, no once-only guard), so they
 * come back on the new loop with app_wifi's init. */
static void otaBootWifiCleanup(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
}

/* Minimal WiFi STA bring-up with the credentials stored by provisioning.
 * Returns true when an IP address was obtained. */
static bool otaBootWifi(void)
{
    tcpip_adapter_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;

    wifi_config_t wifi_config = { 0 };
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config) != ESP_OK ||
        wifi_config.sta.ssid[0] == 0) {
        ESP_LOGE(TAG, "No stored WiFi credentials - cannot update");
        return false;
    }
    ESP_LOGI(TAG, "Connecting to stored SSID: %s", wifi_config.sta.ssid);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;

    /* Poll for an address; nudge a (re)connect every ~4 s. No event handlers
     * are registered here, so there is nothing to unregister later. The
     * status LED blinks at ~2.5 Hz while connecting. */
    for (int i = 0; i < 100; i++) {                       // up to ~20 s
        tcpip_adapter_ip_info_t ip = { 0 };
        if ((i % 20) == 0) {
            esp_wifi_connect();
        }
        otaIndicate(OTA_IND_TICK);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            ESP_LOGI(TAG, "Got IP");
            return true;
        }
    }
    ESP_LOGE(TAG, "WiFi connection timed out");
    return false;
}

/* The requested update, on a clean boot-time heap. The image streams into
 * the passive partition while its descriptor is being captured; the
 * partition is activated only for a complete image with a NEWER version.
 * Ends either in esp_restart() (updated) or in releasing sBootCheckDone so
 * the normal HomeKit startup continues. */
static void otaBootTask(void *pvParameter)
{
    (void)pvParameter;
    ESP_LOGI(TAG, "Boot-time OTA update (free heap %u)",
             (unsigned)esp_get_free_heap_size());
    otaIndicate(OTA_IND_START);

    if (!otaBootWifi()) {
        goto ota_done;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_FIRMWARE_UPGRADE_URL,
        .cert_pem = (char *)server_cert_pem_start,
        .timeout_ms = CONFIG_OTA_RECV_TIMEOUT,
    };

#ifdef CONFIG_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif //CONFIG_SKIP_COMMON_NAME_CHECK

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        goto ota_done;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", err);
        esp_http_client_cleanup(client);
        goto ota_done;
    }
    esp_http_client_fetch_headers(client);

    /* An error page must never be mistaken for a firmware image. */
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Server returned HTTP status %d", status);
        http_cleanup(client);
        goto ota_done;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        http_cleanup(client);
        goto ota_done;
    }
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Couldn't allocate the OTA data buffer");
        http_cleanup(client);
        goto ota_done;
    }

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        free(buf);
        http_cleanup(client);
        goto ota_done;
    }

    ota_img_parser_t parser;
    ota_parser_init(&parser);

    esp_err_t ota_write_err = ESP_OK;
    bool version_ok = false;
    bool rejected = false;
    int binary_file_len = 0;

    while (1) {
        int data_read = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed, all data received");
            break;
        }
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: data read error");
            ota_write_err = ESP_FAIL;
            break;
        }
        ota_write_err = esp_ota_write(update_handle, buf, data_read);
        if (ota_write_err != ESP_OK) {
            break;
        }
        binary_file_len += data_read;
        if ((binary_file_len & 0x3FFF) < OTA_BUF_SIZE) {
            otaIndicate(OTA_IND_TICK);          // visible flicker every ~16 KB
        }

        ota_parser_feed(&parser, (const uint8_t *)buf, data_read);
        if (parser.desc_done && !version_ok && !rejected) {
            if (ValidateImageHeader(&parser.desc) == ESP_OK) {
                version_ok = true;
            } else {
                /* Not newer (or unreadable) — stop downloading right away;
                 * the partition is simply never activated. */
                rejected = true;
                break;
            }
        }
    }

    bool data_complete = !rejected && esp_http_client_is_complete_data_received(client);
    free(buf);
    http_cleanup(client);
    ESP_LOGI(TAG, "Total binary data length written: %d", binary_file_len);

    esp_err_t ota_end_err = esp_ota_end(update_handle);
    if (rejected) {
        goto ota_done;
    }
    if (!version_ok) {
        ESP_LOGE(TAG, "No app descriptor found in the downloaded image");
        goto ota_done;
    }
    if (ota_write_err != ESP_OK || !data_complete || ota_end_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed (write=%d, complete=%d, end=%d)",
                 ota_write_err, (int)data_complete, ota_end_err);
        goto ota_done;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
        goto ota_done;
    }

    ESP_LOGI(TAG, "OTA upgrade successful. Rebooting into the new firmware ...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();

ota_done:
    /* No update happened — hand the WiFi driver back in a pristine state
     * and let the normal startup continue. */
    otaIndicate(OTA_IND_END);
    otaBootWifiCleanup();
    ESP_LOGI(TAG, "Continuing normal startup (free heap %u)",
             (unsigned)esp_get_free_heap_size());
    xSemaphoreGive(sBootCheckDone);
    vTaskDelete(NULL);
}

void otaBootCheck(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Leave the recovery to the main initialisation path. */
        return;
    }
    if (!otaBootFlagGet()) {
        return;
    }
    /* One-shot: clear the flag BEFORE the attempt, so whatever happens next
     * the following boot is a normal one — no update loops are possible. */
    otaBootFlagSet(0);
    ESP_LOGI(TAG, "Update requested from HomeKit - running it before HomeKit starts");

    sBootCheckDone = xSemaphoreCreateBinary();
    if (!sBootCheckDone) {
        return;
    }

    /* TLS handshake processing needs a roomy stack; app_main's is small. */
    xTaskCreate(otaBootTask, OTA_TASK_NAME, OTA_STACKSIZE, NULL, OTA_TASK_PRIORITY, NULL);
    /* Wait here: the task either restarts the device (update installed) or
     * releases the semaphore so the normal startup continues. */
    xSemaphoreTake(sBootCheckDone, portMAX_DELAY);
    vSemaphoreDelete(sBootCheckDone);
    sBootCheckDone = NULL;
}

#else /* !CONFIG_IDF_TARGET_ESP8266 */

void otaBootCheck(void)
{
    /* Boot-time staging is only needed on the ESP8266. */
}

/* ============================= ESP32 variant ============================= */

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

#endif /* CONFIG_IDF_TARGET_ESP8266 */

bool otaUpdate(void)
{
    /* Never disturb the initial pairing window: an unpaired accessory skips
     * OTA. A manual trigger can only arrive from a paired controller, so
     * this check can only ever filter automated callers. */
    if (hap_get_paired_controller_count() == 0) {
        ESP_LOGW(TAG, "Accessory is not paired yet - skipping OTA");
        return false;
    }
#ifdef CONFIG_IDF_TARGET_ESP8266
    /* Stage the update for the next boot (clean heap) and restart. */
    otaUpdateStatus = FW_UPG_STATUS_UPGRADING;
    otaBootFlagSet(1);
    xTaskCreate(&otaRestartTask, OTA_TASK_NAME, 4096, NULL, OTA_TASK_PRIORITY, NULL);
#else
    xTaskCreate(&otaTask, OTA_TASK_NAME, OTA_STACKSIZE, NULL, OTA_TASK_PRIORITY, NULL);
#endif
    return true;
}
