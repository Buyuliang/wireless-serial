idf_component_register(SRCS "tcp_serial.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_netif lwip driver nvs_flash)
