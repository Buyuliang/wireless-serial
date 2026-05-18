#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

/**
 * @brief Wi-Fi凭据回调函数类型
 * @param ssid Wi-Fi名称
 * @param password Wi-Fi密码
 */
typedef void (*credential_callback_t)(const char *ssid, const char *password);
typedef void (*ap_control_callback_t)(void);

/**
 * @brief 启动Web配网服务器（含DNS劫持）
 */
void start_web_server(void);

/**
 * @brief 停止Web配网服务器
 */
void stop_web_server(void);

/**
 * @brief 设置Wi-Fi凭据回调
 */
void web_server_set_credential_callback(credential_callback_t callback);
void web_server_set_ap_close_callback(ap_control_callback_t callback);

/**
 * @brief 扫描附近Wi-Fi（JSON格式返回）
 * 返回格式: [{"ssid":"xxx","rssi":-60,"auth":1,"channel":6}, ...]
 */
char* wifi_scan_json(void);

#endif // WEB_SERVER_H
