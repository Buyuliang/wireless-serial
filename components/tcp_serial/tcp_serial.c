/**
 * TCP串口透传服务
 *
 * ESP32 作为 TCP Server，PC 端通过虚拟串口软件连接。
 * 实现 TCP <=> UART 双向数据透传。
 *
 * 使用方式（PC端）：
 *   Windows: com2tcp com2 192.168.x.x 8888
 *   Linux:   socat /dev/ttyS0 TCP:192.168.x.x:8888
 *   Mac:     socat /dev/tty.usbserial TCP:192.168.x.x:8888
 *
 * 或者使用 HW Virtual Serial Port 等图形化工具。
 */

#include "tcp_serial.h"

#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <unistd.h>

#define TAG "TCP_SERIAL"

/* ========== 配置 ========== */
#define TCP_LISTEN_PORT         8888
#define TCP_MAX_CLIENTS         1
#define TCP_RX_BUF_SIZE         1024
#define TCP_TX_BUF_SIZE         4096

/* UART 配置 - 使用 UART1 (GPIO4=TX, GPIO5=RX)，可按实际硬件修改 */
#define UART_NUM                UART_NUM_1
#define UART_TX_PIN             GPIO_NUM_4
#define UART_RX_PIN             GPIO_NUM_5
#define UART_BUF_SIZE           4096
#define UART_BAUD_DEFAULT       1500000
#define UART_TASK_STACK         4096

/* NVS 存储 baudrate */
#define NVS_NS_BAUD             "tcp_serial"
#define NVS_KEY_BAUD            "baudrate"

/* ========== 全局变量 ========== */
static int s_server_sock = -1;
static int s_client_sock = -1;
static bool s_running = false;
static TaskHandle_t s_tcp_task = NULL;
static TaskHandle_t s_uart_task = NULL;
static int s_listen_port = TCP_LISTEN_PORT;
static int s_baud_rate = UART_BAUD_DEFAULT;

/* ========== UART 初始化 ========== */
static void uart_init(int baudrate)
{
    uart_config_t uart_config = {
        .baud_rate = baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d 已初始化: %d baud, TX=GPIO%d, RX=GPIO%d",
             UART_NUM, baudrate, UART_TX_PIN, UART_RX_PIN);
}

/* ========== TCP => UART 转发任务 ========== */
static void tcp_to_uart_task(void *arg)
{
    uint8_t *buf = malloc(TCP_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "TCP=>UART 缓冲区分配失败");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP=>UART 转发任务已启动");

    while (s_running) {
        if (s_client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int len = recv(s_client_sock, buf, TCP_RX_BUF_SIZE, 0);
        if (len <= 0) {
            if (len == 0) {
                ESP_LOGI(TAG, "客户端断开连接");
            } else {
                ESP_LOGW(TAG, "recv 错误: errno %d", errno);
            }
            close(s_client_sock);
            s_client_sock = -1;
            continue;
        }

        /* TCP 数据写入 UART */
        int written = uart_write_bytes(UART_NUM, buf, len);
        if (written < 0) {
            ESP_LOGE(TAG, "UART 写入失败");
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/* ========== UART => TCP 转发任务 ========== */
static void uart_to_tcp_task(void *arg)
{
    uint8_t *buf = malloc(UART_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "UART=>TCP 缓冲区分配失败");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART=>TCP 转发任务已启动");

    while (s_running) {
        if (s_client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int len = uart_read_bytes(UART_NUM, buf, UART_BUF_SIZE, pdMS_TO_TICKS(50));
        if (len > 0) {
            int sent = send(s_client_sock, buf, len, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "TCP 发送失败: errno %d", errno);
                close(s_client_sock);
                s_client_sock = -1;
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/* ========== TCP Server 主任务 ========== */
static void tcp_server_task(void *arg)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(s_listen_port);

    s_server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_server_sock < 0) {
        ESP_LOGE(TAG, "创建 socket 失败: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err = bind(s_server_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "bind 失败: errno %d", errno);
        close(s_server_sock);
        s_server_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    err = listen(s_server_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "listen 失败: errno %d", errno);
        close(s_server_sock);
        s_server_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP Server 已启动，监听端口: %d", s_listen_port);

    /* 启动双向转发任务 */
    xTaskCreate(tcp_to_uart_task, "tcp2uart", UART_TASK_STACK, NULL, 5, &s_tcp_task);
    xTaskCreate(uart_to_tcp_task, "uart2tcp", UART_TASK_STACK, NULL, 5, &s_uart_task);

    /* 接受客户端连接循环 */
    while (s_running) {
        ESP_LOGI(TAG, "等待客户端连接...");

        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int new_sock = accept(s_server_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (new_sock < 0) {
            if (s_running) {
                ESP_LOGE(TAG, "accept 失败: errno %d", errno);
            }
            break;
        }

        /* 关闭旧连接 */
        if (s_client_sock >= 0) {
            ESP_LOGW(TAG, "新客户端连接，关闭旧连接");
            close(s_client_sock);
        }

        s_client_sock = new_sock;

        /* 设置 TCP 保活 */
        int keepalive = 1;
        int keepidle = 5;
        int keepintvl = 3;
        int keepcnt = 3;
        setsockopt(s_client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(s_client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(s_client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(s_client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        /* 启用 TCP_NODELAY 减少延迟 */
        int nodelay = 1;
        setsockopt(s_client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char addr_str[32];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "客户端已连接: %s:%d", addr_str, ntohs(source_addr.sin_port));
    }

    /* 清理 */
    if (s_client_sock >= 0) close(s_client_sock);
    if (s_server_sock >= 0) close(s_server_sock);
    s_client_sock = -1;
    s_server_sock = -1;

    vTaskDelete(NULL);
}

/* ========== 公共接口 ========== */
void tcp_serial_start(int port)
{
    if (s_running) {
        ESP_LOGW(TAG, "TCP串口服务已在运行");
        return;
    }

    s_listen_port = (port > 0) ? port : TCP_LISTEN_PORT;
    s_running = true;

    /* 初始化 UART */
    uart_init(s_baud_rate);

    /* 启动 TCP Server 任务 */
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "TCP串口透传服务已启动 (端口: %d, 波特率: %d)", s_listen_port, s_baud_rate);
}

void tcp_serial_stop(void)
{
    s_running = false;

    if (s_client_sock >= 0) {
        close(s_client_sock);
        s_client_sock = -1;
    }
    if (s_server_sock >= 0) {
        close(s_server_sock);
        s_server_sock = -1;
    }

    if (s_tcp_task) {
        vTaskDelete(s_tcp_task);
        s_tcp_task = NULL;
    }
    if (s_uart_task) {
        vTaskDelete(s_uart_task);
        s_uart_task = NULL;
    }

    ESP_LOGI(TAG, "TCP串口透传服务已停止");
}

bool tcp_serial_is_running(void)
{
    return s_running;
}
