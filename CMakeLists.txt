idf_component_register(SRCS "fupdateota.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES ./server_certs/ca_cert.pem
                    REQUIRES esp_https_ota esp_http_client app_update nvs_flash esp_hap_core bootloader_support)
