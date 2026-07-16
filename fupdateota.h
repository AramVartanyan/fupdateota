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

#ifdef __cplusplus
}
#endif

#endif /* _FUPDATEOTA_H_ */
