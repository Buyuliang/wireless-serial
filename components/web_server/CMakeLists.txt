idf_component_register(
    SRCS "web_server.c" "dns_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server esp_netif lwip json
)
