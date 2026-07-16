/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef _FUPDATEOTA_H_
#define _FUPDATEOTA_H_

#pragma once
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Firmware Upgrade Status
 *
 * These are the valid values for HAP_CHAR_CUSTOM_UUID_FW_UPG_STATUS
 */
typedef enum {
    /** FW Upgrade Failed */
    FW_UPG_STATUS_FAIL = -1,
    /** FW Upgrade Idle */
    FW_UPG_STATUS_IDLE = 0,
    /** FW Upgrade in Progress */
    FW_UPG_STATUS_UPGRADING = 1,
    /** FW Upgrade Successful */
    FW_UPG_STATUS_SUCCESS = 2,
} fupdateota_status_t;

extern fupdateota_status_t otaUpdateStatus;

/** Initiate FW OTA update
 *
 * @return 0 at fail
 * @return 1 success start
 */
bool otaUpdate(void);

/** Progress-indication events emitted during the boot-time update.
 *
 * The OTA component deliberately touches NO GPIO itself: the application
 * registers a handler (see otaSetIndication) and drives its own indication
 * hardware (e.g. the status LED via outputwrite) from these events.
 */
typedef enum {
    /** The boot-time update is starting (before WiFi comes up) */
    OTA_IND_START = 0,
    /** Periodic pulse: WiFi connect polling (~5/s) and download progress (~every 16 KB) */
    OTA_IND_TICK,
    /** Finished without installing an update; normal startup continues */
    OTA_IND_END,
} fupdateota_indication_t;

/** Register an optional indication handler, called from the OTA context on
 * the events above. Keep the handler short (a GPIO write). Call before
 * otaBootCheck(). Pass NULL to disable. */
void otaSetIndication(void (*handler)(fupdateota_indication_t event));

/** Boot-time OTA hook (ESP8266 only; a no-op on ESP32).
 *
 * On the ESP8266 an update requested via otaUpdate() only sets a flag and
 * restarts: the actual download happens on the NEXT boot, from this hook,
 * BEFORE HomeKit/mDNS/httpd have started — with a clean, unfragmented heap
 * (a TLS handshake does not survive the fragmented ~44K left after a
 * running HAP server is stopped). Call it first thing in app_main().
 * If no update was requested it returns immediately.
 */
void otaBootCheck(void);

#ifdef __cplusplus
}
#endif

#endif /* _FUPDATEOTA_H_ */
