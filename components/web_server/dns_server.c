/**
 * 简易DNS服务器 - Captive Portal劫持
 * 将所有DNS请求解析到AP网关 192.168.4.1
 */
#include "dns_server.h"
#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/task.h"

#define TAG "DNS_SERVER"
#define DNS_PORT 53
#define DNS_MAX_MSG_LEN 512
#define AP_GATEWAY "192.168.4.1"

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task = NULL;
static bool s_dns_running = false;

static void dns_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DNS_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "DNS socket创建失败");
        vTaskDelete(NULL);
        return;
    }

    if (bind(s_dns_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind失败");
        close(s_dns_socket);
        s_dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS服务器已启动，劫持所有请求到 %s", AP_GATEWAY);

    uint8_t rx_buf[DNS_MAX_MSG_LEN];
    uint8_t tx_buf[DNS_MAX_MSG_LEN];

    while (s_dns_running) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(s_dns_socket, rx_buf, sizeof(rx_buf), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0 || !s_dns_running) break;

        if (len < 12) continue;  // DNS头最小12字节

        // 构造DNS响应
        memcpy(tx_buf, rx_buf, len);

        // 设置响应标志
        tx_buf[2] = 0x81;  // Response, Recursion Desired + Standard query
        tx_buf[3] = 0x80;  // Recursion Available

        // Questions: 1 (从请求复制), Answers: 1
        // Byte 4-5: QDCOUNT (already set)
        tx_buf[6] = 0x00; tx_buf[7] = 0x01;  // ANCOUNT = 1
        tx_buf[8] = 0x00; tx_buf[9] = 0x00;  // NSCOUNT = 0
        tx_buf[10] = 0x00; tx_buf[11] = 0x00; // ARCOUNT = 0

        // 添加Answer section
        int pos = len;

        // Name pointer (指向请求中的域名)
        tx_buf[pos++] = 0xC0;
        tx_buf[pos++] = 0x0C;

        // Type A (0x0001)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x01;

        // Class IN (0x0001)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x01;

        // TTL (300秒)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x12;  // 18 seconds for quick redirect notice

        // Data length (4 bytes for IPv4)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x04;

        // IP: 192.168.4.1
        uint32_t ip = inet_addr(AP_GATEWAY);
        memcpy(&tx_buf[pos], &ip, 4);
        pos += 4;

        sendto(s_dns_socket, tx_buf, pos, 0,
               (struct sockaddr *)&source_addr, socklen);
    }

    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }

    ESP_LOGI(TAG, "DNS服务器已停止");
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (s_dns_running) return;
    s_dns_running = true;
    xTaskCreate(dns_task, "dns_server", 4096, NULL, 5, &s_dns_task);
}

void dns_server_stop(void)
{
    s_dns_running = false;
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_dns_task = NULL;
    }
}
