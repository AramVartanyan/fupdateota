idf_component_register(SRCS "fupdateota.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES ./server_certs/ca_cert.pem
                    REQUIRES main nvs_flash app_update)
