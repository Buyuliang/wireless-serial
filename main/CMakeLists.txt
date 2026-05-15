idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash driver lwip json esp_timer
    PRIV_REQUIRES web_server qr_code tcp_serial
)
