# fupdateota

Firmware update (OTA) helper for ESP-IDF (ESP32 chips) and ESP8266_RTOS_SDK.

## Configuration (menuconfig → Firmware update)

| Option | Default | Description |
|---|---|---|
| `CONFIG_FIRMWARE_UPGRADE_URL` | — | the firmware image (https) |
| `CONFIG_SKIP_COMMON_NAME_CHECK` | y | skip the CN check of the server certificate |
| `CONFIG_USE_CERT_BUNDLE` | y | the ESP-IDF certificate bundle instead of `server_certs/ca_cert.pem` |
| `CONFIG_SKIP_VERSION_CHECK` | n | install also a version that is not newer |
| `CONFIG_FUPDATEOTA_HAP_PAIRED_ONLY` | y | `otaUpdate()` does nothing while the HomeKit accessory is not paired. With esp-homekit-sdk in the project it works as is; without HomeKit the check is skipped by itself — no change needed |
| `CONFIG_OTA_RECV_TIMEOUT` | 5000 | receive timeout, ms |
