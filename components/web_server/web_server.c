/**
 * Web配网服务器 + DNS劫持（Captive Portal）
 * 
 * 提供HTTP接口：
 *   GET  /           -> 配网主页
 *   GET  /scan       -> 扫描Wi-Fi（JSON）
 *   POST /connect    -> 提交Wi-Fi凭据
 *   GET  /status     -> 连接状态
 */

#include "web_server.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"
#include "dns_server.h"

#define TAG "WEB_SERVER"

static httpd_handle_t s_server = NULL;
static credential_callback_t s_credential_cb = NULL;

/* ========== 前端HTML页面（嵌入到代码中） ========== */
static const char *HTML_PAGE =
"<!DOCTYPE html>"
"<html lang='zh-CN'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 配网</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;"
"display:flex;justify-content:center;align-items:center;padding:16px}"
".card{background:#fff;border-radius:16px;padding:28px;width:100%;max-width:420px;"
"box-shadow:0 20px 60px rgba(0,0,0,0.3)}"
"h1{text-align:center;color:#333;margin-bottom:4px;font-size:22px}"
".subtitle{text-align:center;color:#888;font-size:13px;margin-bottom:24px}"
".section-title{font-size:14px;font-weight:600;color:#555;margin:16px 0 8px}"
".wifi-list{max-height:260px;overflow-y:auto;border:1px solid #e0e0e0;"
"border-radius:12px;margin-bottom:16px}"
".wifi-item{padding:12px 16px;border-bottom:1px solid #f0f0f0;cursor:pointer;"
"display:flex;align-items:center;justify-content:space-between;"
"transition:background 0.2s}"
".wifi-item:hover{background:#f5f5ff}"
".wifi-item.selected{background:#e8e8ff;border-left:3px solid #667eea}"
".wifi-item:last-child{border-bottom:none}"
".wifi-name{font-weight:500;color:#333;font-size:14px}"
".wifi-meta{display:flex;align-items:center;gap:6px}"
".rssi{font-size:12px;color:#888;min-width:36px}"
".lock{font-size:14px}"
".bars{display:flex;align-items:flex-end;gap:2px;height:16px}"
".bar{width:4px;border-radius:1px;background:#ccc}"
".bar.active{background:#667eea}"
".input-group{margin-bottom:16px}"
"input[type='text'],input[type='password']{width:100%;padding:12px 16px;"
"border:2px solid #e0e0e0;border-radius:12px;font-size:15px;outline:none;"
"transition:border-color 0.3s}"
"input:focus{border-color:#667eea}"
".btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:16px;"
"font-weight:600;cursor:pointer;transition:all 0.3s}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff}"
".btn-primary:hover{transform:translateY(-1px);box-shadow:0 4px 15px rgba(102,126,234,0.4)}"
".btn-primary:disabled{opacity:0.5;cursor:not-allowed;transform:none}"
".btn-scan{background:#f0f0ff;color:#667eea;margin-bottom:12px;font-size:14px;padding:10px}"
".status{text-align:center;padding:12px;border-radius:8px;margin-top:12px;"
"font-size:14px;display:none}"
".status.error{display:block;background:#ffe0e0;color:#c00}"
".status.success{display:block;background:#e0ffe0;color:#060}"
".status.loading{display:block;background:#e0e8ff;color:#336}"
".spinner{display:inline-block;width:16px;height:16px;border:2px solid #336;"
"border-top-color:transparent;border-radius:50%;animation:spin 0.8s linear infinite;"
"vertical-align:middle;margin-right:6px}"
"@keyframes spin{to{transform:rotate(360deg)}}"
".hidden{display:none}"
".loading-scan{text-align:center;padding:20px;color:#888}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h1>📶 Wi-Fi 配网</h1>"
"<p class='subtitle'>为您的ESP32设备配置网络</p>"
""
"<button class='btn btn-scan' onclick='scanWiFi()'>🔍 扫描附近Wi-Fi</button>"
""
"<div id='scanStatus' class='loading-scan hidden'>扫描中...</div>"
"<div id='wifiList' class='wifi-list'></div>"
""
"<div class='section-title'>手动输入Wi-Fi名称</div>"
"<div class='input-group'>"
"<input type='text' id='ssid' placeholder='Wi-Fi名称 (SSID)'>"
"</div>"
""
"<div class='section-title'>输入密码</div>"
"<div class='input-group'>"
"<input type='password' id='password' placeholder='Wi-Fi密码'>"
"</div>"
""
"<button class='btn btn-primary' id='connectBtn' onclick='connectWiFi()'>连接网络</button>"
""
"<div id='status' class='status'></div>"
"</div>"
""
"<script>"
"let selectedSSID='';"
""
"function rssiToBars(rssi){"
"let pct=rssi>-50?100:rssi>-60?80:rssi>-70?60:rssi>-80?40:20;"
"let n=Math.round(pct/25);"
"let h=[6,10,14,18];"
"let s='';"
"for(let i=0;i<4;i++){"
"s+='<div class=\"bar'+(i<n?' active':'')+'\" style=\"height:'+h[i]+'px\"></div>';"
"}"
"return '<div class=\"bars\">'+s+'</div>';"
"}"
""
"function selectWiFi(el,ssid){"
"document.querySelectorAll('.wifi-item').forEach(e=>e.classList.remove('selected'));"
"el.classList.add('selected');"
"selectedSSID=ssid;"
"document.getElementById('ssid').value=ssid;"
"document.getElementById('password').focus();"
"}"
""
"async function scanWiFi(){"
"let list=document.getElementById('wifiList');"
"let st=document.getElementById('scanStatus');"
"list.innerHTML='';"
"st.classList.remove('hidden');"
"try{"
"let res=await fetch('/scan');"
"let data=await res.json();"
"st.classList.add('hidden');"
"if(data.length===0){list.innerHTML='<div style=\"padding:20px;text-align:center;color:#888\">未发现Wi-Fi热点</div>';return;}"
"// 按信号强度排序"
"data.sort((a,b)=>b.rssi-a.rssi);"
"data.forEach(ap=>{"
"let lock=ap.auth>0?'🔒':'🔓';"
"let item=document.createElement('div');"
"item.className='wifi-item';"
"item.innerHTML='<div class=\"wifi-name\">'+lock+' '+esc(ap.ssid)+'</div>'"
"+'<div class=\"wifi-meta\">'+rssiToBars(ap.rssi)+'<span class=\"rssi\">'+ap.rssi+'dBm</span></div>';"
"item.onclick=function(){selectWiFi(this,ap.ssid)};"
"list.appendChild(item);"
"});"
"}catch(e){"
"st.classList.add('hidden');"
"list.innerHTML='<div style=\"padding:20px;text-align:center;color:#c00\">扫描失败，请重试</div>';"
"}"
"}"
""
"function esc(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML;}"
""
"async function connectWiFi(){"
"let ssid=document.getElementById('ssid').value.trim();"
"let pwd=document.getElementById('password').value;"
"let btn=document.getElementById('connectBtn');"
"let st=document.getElementById('status');"
"if(!ssid){showStatus('error','请输入或选择Wi-Fi名称');return;}"
"btn.disabled=true;"
"showStatus('loading','<span class=\"spinner\"></span>正在连接 '+esc(ssid)+' ...');"
"try{"
"let fd=new FormData();"
"fd.append('ssid',ssid);"
"fd.append('password',pwd);"
"let res=await fetch('/connect',{method:'POST',body:fd});"
"let txt=await res.text();"
"if(txt==='ok'){"
"showStatus('success','✅ 连接成功！设备即将重启...');"
"}else{"
"showStatus('error','❌ 连接失败，请检查密码是否正确');"
"btn.disabled=false;"
"}"
"}catch(e){"
"showStatus('error','❌ 请求失败，请重试');"
"btn.disabled=false;"
"}"
"}"
""
"function showStatus(type,msg){"
"let st=document.getElementById('status');"
"st.className='status '+type;"
"st.innerHTML=msg;"
"}"
"</script>"
"</body>"
"</html>";

/* ========== Wi-Fi扫描 ========== */
char* wifi_scan_json(void)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_wifi_scan_start(&scan_config, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return strdup("[]");
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", ap_list[i].authmode);
        cJSON_AddNumberToObject(ap, "channel", ap_list[i].primary);
        cJSON_AddItemToArray(root, ap);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(ap_list);

    return json_str;
}

/* ========== HTTP 处理函数 ========== */

// GET / - 配网主页
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

// GET /scan - 扫描Wi-Fi
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    char *json = wifi_scan_json();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

// URL解码辅助函数
static void url_decode(char *dst, const char *src, size_t max_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < max_len - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = ' ';
}

// POST /connect - 提交Wi-Fi凭据
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 解析表单数据: ssid=xxx&password=xxx
    char ssid[64] = {0};
    char password[64] = {0};

    // 简单URL解码并提取字段
    char *ssid_start = strstr(buf, "ssid=");
    char *pwd_start = strstr(buf, "password=");

    if (ssid_start) {
        ssid_start += 5;
        char *end = strchr(ssid_start, '&');
        if (end) {
            char raw_ssid[64] = {0};
            size_t len = end - ssid_start;
            if (len >= sizeof(raw_ssid)) len = sizeof(raw_ssid) - 1;
            strncpy(raw_ssid, ssid_start, len);
            url_decode(ssid, raw_ssid, sizeof(ssid));
        } else {
            url_decode(ssid, ssid_start, sizeof(ssid));
        }
    }

    if (pwd_start) {
        pwd_start += 9;
        char *end = strchr(pwd_start, '&');
        if (end) {
            char raw_pwd[64] = {0};
            size_t len = end - pwd_start;
            if (len >= sizeof(raw_pwd)) len = sizeof(raw_pwd) - 1;
            strncpy(raw_pwd, pwd_start, len);
            url_decode(password, raw_pwd, sizeof(password));
        } else {
            url_decode(password, pwd_start, sizeof(password));
        }
    }

    ESP_LOGI(TAG, "收到配网请求: SSID=%s", ssid);

    if (strlen(ssid) == 0) {
        httpd_resp_send(req, "fail", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 通知回调
    if (s_credential_cb) {
        s_credential_cb(ssid, password);
    }

    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /status - 连接状态
static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap);

    cJSON *root = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(root, "status", "connected");
        cJSON_AddStringToObject(root, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
    } else {
        cJSON_AddStringToObject(root, "status", "disconnected");
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

/* ========== 服务器启停 ========== */
void start_web_server(void)
{
    if (s_server != NULL) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "启动Web服务器，端口: %d", config.server_port);

    if (httpd_start(&s_server, &config) == ESP_OK) {
        // 注册URI处理
        httpd_uri_t root_uri = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
        httpd_uri_t scan_uri = { .uri="/scan", .method=HTTP_GET, .handler=scan_get_handler };
        httpd_uri_t connect_uri = { .uri="/connect", .method=HTTP_POST, .handler=connect_post_handler };
        httpd_uri_t status_uri = { .uri="/status", .method=HTTP_GET, .handler=status_get_handler };

        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &scan_uri);
        httpd_register_uri_handler(s_server, &connect_uri);
        httpd_register_uri_handler(s_server, &status_uri);

        // 启动DNS劫持（Captive Portal）
        dns_server_start();

        ESP_LOGI(TAG, "Web服务器已启动");
    } else {
        ESP_LOGE(TAG, "Web服务器启动失败");
    }
}

void stop_web_server(void)
{
    if (s_server) {
        dns_server_stop();
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web服务器已停止");
    }
}

void web_server_set_credential_callback(credential_callback_t callback)
{
    s_credential_cb = callback;
}
