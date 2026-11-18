# No esp-homekit-sdk requirement: the "paired only" guard of otaUpdate() reaches
# hap_get_paired_controller_count() through a weak reference (see fupdateota.c).
idf_component_register(SRCS "fupdateota.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES ./server_certs/ca_cert.pem
                    REQUIRES esp_https_ota esp_http_client app_update nvs_flash bootloader_support mbedtls)
