/**
 * ESP32 Wi-Fi 配网系统
 * 
 * 功能：
 *   1. 开机检测按键 -> 进入配网模式
 *   2. SoftAP + Web服务器 -> 手机扫码连接配网
 *   3. 扫描附近Wi-Fi并显示信号强度
 *   4. 用户选择Wi-Fi输入密码 -> 保存到NVS
 *   5. 下次开机自动连接保存的Wi-Fi
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "lwip/inet.h"

#include "web_server.h"
#include "qr_code.h"

/* ========== 宏定义 ========== */
#define TAG "PROVISION"

// 按键GPIO（根据实际硬件修改）
#define PROVISION_BUTTON_GPIO   GPIO_NUM_0   // BOOT按键

// AP配置
#define AP_SSID_PREFIX          "ESP32_Setup_"
#define AP_PASSWORD             "12345678"   // 至少8位
#define AP_MAX_CONN             4
#define AP_CHANNEL              1

// Wi-Fi连接超时
#define WIFI_CONNECT_TIMEOUT_MS 15000

// NVS存储
#define NVS_NAMESPACE           "wifi_config"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"

// 事件位
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define PROVISION_DONE_BIT      BIT2

// 配网按键检测：开机后按住按键的时长（ms）
#define BUTTON_HOLD_TIME_MS     3000

/* ========== 全局变量 ========== */
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static const int MAX_RETRY = 5;
static bool s_is_provisioning = false;
static char s_device_ssid[32];      // 设备AP名：ESP32_Setup_XXXX
static char s_device_id[16];        // 设备唯一ID（基于MAC后4字节）

/* ========== 函数声明 ========== */
static void wifi_init_sta(void);
static void wifi_init_softap(void);
static bool load_wifi_config(char *ssid, size_t ssid_len, char *password, size_t password_len);
static void save_wifi_config(const char *ssid, const char *password);
static bool check_provision_button(void);
static void generate_device_identity(void);
static void provisioning_task(void *pvParameters);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);

/* ========== NVS 操作 ========== */
static bool load_wifi_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS打开失败，可能首次运行");
        return false;
    }

    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &password_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取密码失败");
        return false;
    }

    ESP_LOGI(TAG, "从NVS加载Wi-Fi配置: SSID=%s", ssid);
    return true;
}

static void save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败");
        return;
    }

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Wi-Fi配置已保存: SSID=%s", ssid);
}

/* ========== 设备唯一标识（基于MAC地址） ========== */
static void generate_device_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    // 使用MAC后4字节作为设备短ID
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    // AP名称：ESP32_Setup_AABBCCDD
    snprintf(s_device_ssid, sizeof(s_device_ssid), "%s%s", AP_SSID_PREFIX, s_device_id);

    ESP_LOGI(TAG, "设备ID: %s, AP SSID: %s", s_device_id, s_device_ssid);
}

/* ========== 按键检测 ========== */
static bool check_provision_button(void)
{
    ESP_LOGI(TAG, "检测配网按键 (GPIO%d)，按住 %d ms 进入配网模式...",
             PROVISION_BUTTON_GPIO, BUTTON_HOLD_TIME_MS);

    // BOOT按键低电平有效
    int64_t start_time = esp_timer_get_time();
    bool was_pressed = false;

    // 等待用户松开按键（上电瞬间可能正在按着）
    while (gpio_get_level(PROVISION_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t elapsed = (esp_timer_get_time() - start_time) / 1000;
        if (elapsed > BUTTON_HOLD_TIME_MS) {
            was_pressed = true;
            break;
        }
    }

    if (was_pressed) {
        ESP_LOGI(TAG, "按键已按住超过 %d ms，进入配网模式", BUTTON_HOLD_TIME_MS);
        // 等待松开
        while (gpio_get_level(PROVISION_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return true;
    }

    // 再检测3秒内是否重新按下
    start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) / 1000 < BUTTON_HOLD_TIME_MS) {
        if (gpio_get_level(PROVISION_BUTTON_GPIO) == 0) {
            // 检测到按下，等待持续按住
            int64_t press_start = esp_timer_get_time();
            while (gpio_get_level(PROVISION_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                int64_t hold_ms = (esp_timer_get_time() - press_start) / 1000;
                if (hold_ms > BUTTON_HOLD_TIME_MS) {
                    ESP_LOGI(TAG, "长按检测触发，进入配网模式");
                    while (gpio_get_level(PROVISION_BUTTON_GPIO) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    return true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "未检测到配网按键");
    return false;
}

/* ========== Wi-Fi 事件处理 ========== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA已启动，开始连接...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGW(TAG, "连接失败，重试 %d/%d", s_retry_num, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGE(TAG, "Wi-Fi连接失败，已达最大重试次数");
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "设备连接到AP: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "设备断开AP连接: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获取IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ========== STA 模式连接Wi-Fi ========== */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler,
                                                         NULL,
                                                         &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                         IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler,
                                                         NULL,
                                                         &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA模式已启动");
}

/* ========== SoftAP 模式启动配网 ========== */
static void wifi_init_softap(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler,
                                                         NULL, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_device_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));  // AP+STA模式，方便后续扫描+切换
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP已启动: SSID=%s, Password=%s", s_device_ssid, AP_PASSWORD);
}

/* ========== 配网回调：用户提交Wi-Fi信息 ========== */
static void on_wifi_credentials_received(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "收到Wi-Fi配置: SSID=%s", ssid);

    // 保存到NVS
    save_wifi_config(ssid, password);

    // 停止AP，切换到STA模式连接
    ESP_LOGI(TAG, "正在切换到STA模式连接Wi-Fi...");

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi连接成功！");
        xEventGroupSetBits(s_wifi_event_group, PROVISION_DONE_BIT);
    } else {
        ESP_LOGE(TAG, "Wi-Fi连接失败，请重新配网");
        // 可以重新启动AP模式让用户重试
        esp_wifi_stop();
        wifi_init_softap();
        start_web_server();
    }
}

/* ========== 配网任务 ========== */
static void provisioning_task(void *pvParameters)
{
    // 启动AP
    wifi_init_softap();

    // 启动Web服务器
    start_web_server();

    // 生成并显示二维码信息
    char qr_data[128];
    snprintf(qr_data, sizeof(qr_data),
             "WIFI:S:%s;T:WPA2;P:%s;;", s_device_ssid, AP_PASSWORD);
    ESP_LOGI(TAG, "========== 二维码内容 ==========");
    ESP_LOGI(TAG, "%s", qr_data);
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "请用手机扫描二维码连接Wi-Fi，然后访问 http://192.168.4.1");

    // 在终端打印ASCII二维码（便于调试）
    qr_code_print(qr_data);

    // 等待配网完成
    xEventGroupWaitBits(s_wifi_event_group,
                        PROVISION_DONE_BIT,
                        pdFALSE, pdFALSE,
                        portMAX_DELAY);

    ESP_LOGI(TAG, "配网完成！");

    // 停止Web服务器
    stop_web_server();

    s_is_provisioning = false;
    vTaskDelete(NULL);
}

/* ========== 主入口 ========== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 Wi-Fi 配网系统启动 ===");

    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 生成设备唯一标识
    generate_device_identity();

    // 初始化按键GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PROVISION_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // 检查是否需要配网
    char saved_ssid[64] = {0};
    char saved_password[64] = {0};
    bool has_config = load_wifi_config(saved_ssid, sizeof(saved_ssid),
                                        saved_password, sizeof(saved_password));
    bool force_provision = check_provision_button();

    if (force_provision || !has_config) {
        /* ===== 配网模式 ===== */
        if (!has_config) {
            ESP_LOGI(TAG, "未找到保存的Wi-Fi配置，进入配网模式");
        }

        s_is_provisioning = true;

        // 注册配网回调
        web_server_set_credential_callback(on_wifi_credentials_received);

        // 创建配网任务
        xTaskCreate(provisioning_task, "provision_task", 8192, NULL, 5, NULL);

    } else {
        /* ===== 正常模式：直接连接Wi-Fi ===== */
        ESP_LOGI(TAG, "使用保存的配置连接Wi-Fi: %s", saved_ssid);

        wifi_init_sta();

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, saved_password, sizeof(sta_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

        // 等待连接结果
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi连接成功！设备已就绪");
        } else {
            ESP_LOGE(TAG, "Wi-Fi连接失败，请长按BOOT键重新配网");
            // 可以在此加入重启进入配网模式的逻辑
        }
    }

    ESP_LOGI(TAG, "=== 系统初始化完成 ===");
}
