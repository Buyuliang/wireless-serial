/*
 * ESP32 Wi-Fi provisioning and wireless serial bridge.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "qr_code.h"
#include "tcp_serial.h"
#include "web_server.h"

#define TAG "PROVISION"

#define PROVISION_BUTTON_GPIO   GPIO_NUM_0

#define AP_SSID_PREFIX          "ESP32_Setup_"
#define AP_PASSWORD             "12345678"
#define AP_MAX_CONN             4
#define AP_CHANNEL              1

#define WIFI_CONNECT_TIMEOUT_MS 15000

#define NVS_NAMESPACE           "wifi_config"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "password"

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define PROVISION_DONE_BIT      BIT2

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static const int MAX_RETRY = 5;
static bool s_wifi_stack_ready = false;
static bool s_wifi_handlers_registered = false;
static bool s_is_provisioning = false;
static bool s_sta_should_connect = false;
static esp_netif_t *s_wifi_sta_netif = NULL;
static esp_netif_t *s_wifi_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static char s_device_ssid[32];
static char s_device_id[16];

static void wifi_init_sta(void);
static void wifi_init_softap(void);
static void wifi_stack_init(void);
static void wifi_event_group_prepare(void);
static void start_provisioning_mode(void);
static bool load_wifi_config(char *ssid, size_t ssid_len, char *password, size_t password_len);
static void save_wifi_config(const char *ssid, const char *password);
static bool check_provision_button(void);
static void generate_device_identity(void);
static void provisioning_task(void *pvParameters);
static void on_wifi_credentials_received(const char *ssid, const char *password);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

static bool load_wifi_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS, may be first boot");
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
        ESP_LOGW(TAG, "Failed to load Wi-Fi password from NVS");
        return false;
    }

    ESP_LOGI(TAG, "Loaded Wi-Fi config from NVS: SSID=%s", ssid);
    return true;
}

static void save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(handle, NVS_KEY_PASSWORD, password));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved Wi-Fi config: SSID=%s", ssid);
}

static void generate_device_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_ssid, sizeof(s_device_ssid), "%s%s", AP_SSID_PREFIX, s_device_id);

    ESP_LOGI(TAG, "Device ID: %s, AP SSID: %s", s_device_id, s_device_ssid);
}

static bool check_provision_button(void)
{
    bool pressed = (gpio_get_level(PROVISION_BUTTON_GPIO) == 0);
    ESP_LOGI(TAG, "Boot GPIO%d state: %s",
             PROVISION_BUTTON_GPIO,
             pressed ? "pressed, enter provisioning" : "not pressed");
    return pressed;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started");
            if (s_sta_should_connect) {
                ESP_LOGI(TAG, "Wi-Fi STA connecting...");
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "Provisioning mode: skip STA auto-connect");
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Wi-Fi STA disconnected, reason=%d", event->reason);
            if (!s_sta_should_connect) {
                ESP_LOGI(TAG, "Provisioning mode: ignore STA disconnect");
            } else if (s_retry_num < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGW(TAG, "Retry Wi-Fi connection %d/%d", s_retry_num, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGE(TAG, "Wi-Fi connection failed after retries");
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station joined AP: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station left AP: MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_event_group_prepare(void)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | PROVISION_DONE_BIT);
}

static void wifi_stack_init(void)
{
    if (s_wifi_stack_ready) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (s_wifi_sta_netif == NULL) {
        s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_wifi_ap_netif == NULL) {
        s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (!s_wifi_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_wifi_event_instance));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_ip_event_instance));
        s_wifi_handlers_registered = true;
    }

    s_wifi_stack_ready = true;
}

static void wifi_init_sta(void)
{
    wifi_event_group_prepare();
    s_sta_should_connect = true;
    s_retry_num = 0;

    wifi_stack_init();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "STA mode ready");
}

static void wifi_init_softap(void)
{
    wifi_event_group_prepare();
    s_sta_should_connect = false;
    s_retry_num = 0;

    wifi_stack_init();

    wifi_config_t ap_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strncpy((char *)ap_config.ap.ssid, s_device_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=%s, Password=%s", s_device_ssid, AP_PASSWORD);
}

static void start_provisioning_mode(void)
{
    s_is_provisioning = true;
    web_server_set_credential_callback(on_wifi_credentials_received);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(err);
    }

    wifi_init_softap();
    start_web_server();
}

static void on_wifi_credentials_received(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Received Wi-Fi config: SSID=%s", ssid);

    save_wifi_config(ssid, password);

    ESP_LOGI(TAG, "Switching from AP mode to STA mode...");

    ESP_ERROR_CHECK(esp_wifi_stop());
    s_sta_should_connect = true;
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        xEventGroupSetBits(s_wifi_event_group, PROVISION_DONE_BIT);
        tcp_serial_start(8888);
    } else {
        ESP_LOGE(TAG, "Wi-Fi connect failed, returning to provisioning mode");
        start_provisioning_mode();
    }
}

static void provisioning_task(void *pvParameters)
{
    (void)pvParameters;

    wifi_init_softap();
    start_web_server();

    char qr_data[128];
    snprintf(qr_data, sizeof(qr_data),
             "WIFI:S:%s;T:WPA2;P:%s;;", s_device_ssid, AP_PASSWORD);
    ESP_LOGI(TAG, "========== Provisioning QR ==========");
    ESP_LOGI(TAG, "%s", qr_data);
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "Connect phone to AP and open http://192.168.4.1");

    qr_code_print(qr_data);

    xEventGroupWaitBits(s_wifi_event_group,
                        PROVISION_DONE_BIT,
                        pdFALSE, pdFALSE,
                        portMAX_DELAY);

    ESP_LOGI(TAG, "Provisioning completed");

    stop_web_server();
    s_is_provisioning = false;
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 Wi-Fi provisioning start ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    generate_device_identity();

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PROVISION_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    char saved_ssid[64] = {0};
    char saved_password[64] = {0};
    bool has_config = load_wifi_config(saved_ssid, sizeof(saved_ssid),
                                       saved_password, sizeof(saved_password));
    bool force_provision = check_provision_button();

    if (force_provision || !has_config) {
        if (!has_config) {
            ESP_LOGI(TAG, "No saved Wi-Fi config, entering provisioning mode");
        }

        s_is_provisioning = true;
        web_server_set_credential_callback(on_wifi_credentials_received);
        xTaskCreate(provisioning_task, "provision_task", 8192, NULL, 5, NULL);
    } else {
        ESP_LOGI(TAG, "Connecting with saved Wi-Fi config: %s", saved_ssid);

        wifi_init_sta();

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, saved_password, sizeof(sta_config.sta.password) - 1);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi connected with saved config");
            tcp_serial_start(8888);
        } else {
            ESP_LOGE(TAG, "Wi-Fi connect failed, switching back to provisioning mode");
            start_provisioning_mode();
        }
    }

    ESP_LOGI(TAG, "=== ESP32 Wi-Fi provisioning ready ===");
}
