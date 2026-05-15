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

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dns_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/netif.h"

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
"<title>ESP32 &#x914D;&#x7F51;</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;justify-content:center;align-items:center;padding:16px}"
".card{background:#fff;border-radius:16px;padding:28px;width:100%;max-width:420px;box-shadow:0 20px 60px rgba(0,0,0,0.3)}"
"h1{text-align:center;color:#333;margin-bottom:4px;font-size:22px}"
".subtitle{text-align:center;color:#888;font-size:13px;margin-bottom:24px}"
".section-title{font-size:14px;font-weight:600;color:#555;margin:16px 0 8px}"
".wifi-list{max-height:300px;overflow-y:auto;border:1px solid #e0e0e0;border-radius:12px;margin-bottom:16px}"
".wifi-item{padding:10px 16px;border-bottom:1px solid #f0f0f0;cursor:pointer;display:flex;align-items:center;justify-content:space-between;transition:background 0.2s}"
".wifi-item:hover{background:#f5f5ff}"
".wifi-item.selected{background:#e8e8ff;border-left:3px solid #667eea}"
".wifi-item:last-child{border-bottom:none}"
".wifi-name{font-weight:500;color:#333;font-size:14px;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".wifi-right{display:flex;align-items:center;gap:8px;flex-shrink:0}"
".rssi{font-size:12px;color:#888;min-width:36px}"
".bars{display:flex;align-items:flex-end;gap:2px;height:16px}"
".bar{width:4px;border-radius:1px;background:#ccc}"
".bar.active{background:#667eea}"
".btn-copy{background:none;border:1px solid #ddd;border-radius:6px;padding:3px 8px;cursor:pointer;font-size:12px;color:#667eea;transition:all 0.2s;white-space:nowrap}"
".btn-copy:hover{background:#667eea;color:#fff;border-color:#667eea}"
".btn-copy.copied{background:#4caf50;color:#fff;border-color:#4caf50}"
".input-group{margin-bottom:16px}"
"input[type='text'],input[type='password']{width:100%;padding:12px 16px;border:2px solid #e0e0e0;border-radius:12px;font-size:15px;outline:none;transition:border-color 0.3s}"
"input:focus{border-color:#667eea}"
".btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:16px;font-weight:600;cursor:pointer;transition:all 0.3s}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff}"
".btn-primary:hover{transform:translateY(-1px);box-shadow:0 4px 15px rgba(102,126,234,0.4)}"
".btn-primary:disabled{opacity:0.5;cursor:not-allowed;transform:none}"
".btn-scan{background:#f0f0ff;color:#667eea;margin-bottom:12px;font-size:14px;padding:10px}"
".scan-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:4px}"
".scan-count{font-size:12px;color:#888}"
".scan-auto{font-size:11px;color:#aaa;margin-bottom:12px;display:block}"
".status{text-align:center;padding:12px;border-radius:8px;margin-top:12px;font-size:14px;display:none}"
".status.error{display:block;background:#ffe0e0;color:#c00}"
".status.success{display:block;background:#e0ffe0;color:#060}"
".status.loading{display:block;background:#e0e8ff;color:#336}"
".spinner{display:inline-block;width:16px;height:16px;border:2px solid #336;border-top-color:transparent;border-radius:50%;animation:spin 0.8s linear infinite;vertical-align:middle;margin-right:6px}"
"@keyframes spin{to{transform:rotate(360deg)}}"
".hidden{display:none}"
".loading-scan{text-align:center;padding:20px;color:#888}"
".toast{position:fixed;bottom:30px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:10px 24px;border-radius:20px;font-size:14px;z-index:999;opacity:0;transition:opacity 0.3s;pointer-events:none}"
".toast.show{opacity:1}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h1>&#x914D;&#x7F51; Wi-Fi &#x8BBE;&#x7F6E;</h1>"
"<p class='subtitle'>&#x8FDE;&#x63A5;&#x5230;ESP32&#x8FDB;&#x884C;&#x914D;&#x7F51;&#x8BBE;&#x7F6E;</p>"
"<div class='scan-header'>"
"<button class='btn btn-scan' onclick='scanWiFi()'>&#x5237;&#x65B0;&#x626B;&#x63CF; Wi-Fi</button>"
"<span id='scanCount' class='scan-count'></span>"
"</div>"
"<span id='scanAuto' class='scan-auto'></span>"
"<div id='scanStatus' class='loading-scan hidden'>&#x626B;&#x63CF;&#x4E2D;...</div>"
"<div id='wifiList' class='wifi-list'></div>"
"<div class='section-title'>&#x8F93;&#x5165;&#x6216;&#x9009;&#x62E9;Wi-Fi&#x540D;&#x79F0;</div>"
"<div class='input-group'>"
"<input type='text' id='ssid' placeholder='Wi-Fi&#x540D;&#x79F0; (SSID)'>"
"</div>"
"<div class='section-title'>&#x8F93;&#x5165;&#x5BC6;&#x7801;</div>"
"<div class='input-group'>"
"<input type='password' id='password' placeholder='Wi-Fi&#x5BC6;&#x7801;'>"
"</div>"
"<button class='btn btn-primary' id='connectBtn' onclick='connectWiFi()'>&#x5F00;&#x59CB;&#x8FDE;&#x63A5;</button>"
"<div id='status' class='status'></div>"
"</div>"
"<div id='toast' class='toast'></div>"
"<script>"
"let scanTimer = null;"
"window.onload = function(){"
"scanWiFi();"
"scanTimer = setInterval(scanWiFi, 30000);"
"document.getElementById('scanAuto').textContent='\\u81EA\\u52A8\\u5237\\u65B0: 30\\u79D2';"
"};"
"function esc(s){var d=document.createElement('div');d.textContent=s||'';return d.innerHTML;}"
"function rssiToBars(rssi){"
"let pct=rssi>-50?100:rssi>-60?80:rssi>-70?60:rssi>-80?40:20;"
"let n=Math.round(pct/25);"
"let h=[6,10,14,18];"
"let s='';"
"for(let i=0;i<4;i++){s+='<div class=\"bar'+(i<n?' active':'')+'\" style=\"height:'+h[i]+'px\"></div>';}"
"return '<div class=\"bars\">'+s+'</div>';"
"}"
"function showToast(msg){"
"var t=document.getElementById('toast');"
"t.textContent=msg;"
"t.classList.add('show');"
"setTimeout(function(){t.classList.remove('show');},2000);"
"}"
"function showCopied(btn){"
"btn.classList.add('copied');"
"btn.textContent='\\u2713 \\u5DF2\\u590D\\u5236';"
"setTimeout(function(){btn.classList.remove('copied');btn.textContent='\\u590D\\u5236';},2000);"
"}"
"function copySSID(ssid, btn){"
"var ta=document.createElement('textarea');"
"ta.value=ssid;"
"ta.style.position='fixed';"
"ta.style.left='-9999px';"
"document.body.appendChild(ta);"
"ta.select();"
"try{document.execCommand('copy');showCopied(btn);showToast('\\u5DF2\\u590D\\u5236: '+ssid);}catch(e){showToast('\\u590D\\u5236\\u5931\\u8D25');}"
"document.body.removeChild(ta);"
"}"
"function selectWiFi(el, ssid){"
"document.querySelectorAll('.wifi-item').forEach(function(node){node.classList.remove('selected');});"
"el.classList.add('selected');"
"document.getElementById('ssid').value=ssid;"
"document.getElementById('password').focus();"
"}"
"function buildWiFiItem(ap){"
"var item=document.createElement('div');"
"var lock=ap.auth>0?'[Lock]':'[Open]';"
"item.className='wifi-item';"
"item.innerHTML='<div class=\"wifi-name\">'+lock+' '+esc(ap.ssid)+'</div>'+'<div class=\"wifi-right\">'+rssiToBars(ap.rssi)+'<span class=\"rssi\">'+ap.rssi+'dBm</span>'+'<button type=\"button\" class=\"btn-copy\">\\u590D\\u5236</button></div>';"
"item.onclick=function(){selectWiFi(item, ap.ssid);};"
"var copyBtn=item.querySelector('.btn-copy');"
"copyBtn.onclick=function(event){event.stopPropagation();copySSID(ap.ssid, copyBtn);};"
"return item;"
"}"
"async function scanWiFi(){"
"var list=document.getElementById('wifiList');"
"var st=document.getElementById('scanStatus');"
"var cnt=document.getElementById('scanCount');"
"st.classList.remove('hidden');"
"cnt.textContent='';"
"try{"
"var res=await fetch('/scan',{cache:'no-store'});"
"if(!res.ok){throw new Error('HTTP '+res.status);}"
"var data=await res.json();"
"st.classList.add('hidden');"
"if(!Array.isArray(data) || data.length===0){list.innerHTML='<div style=\"padding:20px;text-align:center;color:#888\">\\u672A\\u53D1\\u73B0Wi-Fi\\u4FE1\\u53F7</div>';return;}"
"data=data.filter(function(ap){return ap && ap.ssid && ap.ssid.trim().length>0;});"
"data.sort(function(a,b){return b.rssi-a.rssi;});"
"cnt.textContent=data.length+' \\u4E2A\\u7F51\\u7EDC';"
"if(data.length===0){list.innerHTML='<div style=\"padding:20px;text-align:center;color:#888\">\\u672A\\u53D1\\u73B0\\u53EF\\u89C1SSID</div>';return;}"
"list.innerHTML='';"
"data.forEach(function(ap){list.appendChild(buildWiFiItem(ap));});"
"}catch(e){"
"console.error('scan failed', e);"
"st.classList.add('hidden');"
"list.innerHTML='<div style=\"padding:20px;text-align:center;color:#c00\">\\u626B\\u63CF\\u51FA\\u9519\\uFF0C\\u8BF7\\u91CD\\u8BD5</div>';"
"}"
"}"
"async function connectWiFi(){"
"var ssid=document.getElementById('ssid').value.trim();"
"var pwd=document.getElementById('password').value;"
"var btn=document.getElementById('connectBtn');"
"if(!ssid){showStatus('error','\\u8BF7\\u5148\\u9009\\u62E9\\u6216\\u8F93\\u5165 Wi-Fi\\u540D\\u79F0');return;}"
"btn.disabled=true;"
"showStatus('loading','<span class=\"spinner\"></span>\\u6B63\\u5728\\u8FDE\\u63A5 '+esc(ssid)+' ...');"
"try{"
"var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pwd);"
"var res=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
"var txt=await res.text();"
"if(txt==='ok'){"
"showStatus('success','\\u8FDE\\u63A5\\u8BF7\\u6C42\\u5DF2\\u63D0\\u4EA4\\uFF0C\\u8BBE\\u5907\\u6B63\\u5728\\u5C1D\\u8BD5\\u63A5\\u5165\\u7F51\\u7EDC...');pollStatus();"
"}else{"
"showStatus('error','\\u8FDE\\u63A5\\u5931\\u8D25\\uFF0C\\u8BF7\\u68C0\\u67E5SSID\\u548C\\u5BC6\\u7801');"
"btn.disabled=false;"
"}"
"}catch(e){"
"showStatus('error','\\u8FDE\\u63A5\\u8BF7\\u6C42\\u5931\\u8D25');"
"btn.disabled=false;"
"}"
"}"
"async function pollStatus(){"
"var tries=0;"
"var timer=setInterval(async function(){"
"tries++;"
"if(tries>30){clearInterval(timer);return;}"
"try{"
"var r=await fetch('/status',{cache:'no-store'});"
"var d=await r.json();"
"if(d.status==='connected'&&d.ip){"
"clearInterval(timer);"
"var info='\\u2705 Wi-Fi\\u8FDE\\u63A5\\u6210\\u529F<BR><BR>';"
"info+='\\u8BBE\\u5907IP: <B>'+d.ip+'</B><BR>';"
"info+='\\u4E32\\u53E3\\u900F\\u4F20\\u7AEF\\u53E3: <B>8888</B><BR><BR>';"
"info+='\\u8FDE\\u63A5\\u65B9\\u5F0F:<BR>';"
"info+='&nbsp;&nbsp;Windows: socat COM2 TCP:'+d.ip+':8888<BR>';"
"info+='&nbsp;&nbsp;Linux/Mac: socat /dev/ttyUSB0 TCP:'+d.ip+':8888';"
"showStatus('success',info);"
"}"
"}catch(e){}"
"},1000);"
"}"
"function showStatus(type,msg){"
"var st=document.getElementById('status');"
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

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi扫描启动失败: %s", esp_err_to_name(err));
        return strdup("[]");
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描结果数量失败: %s", esp_err_to_name(err));
        return strdup("[]");
    }

    ESP_LOGI(TAG, "扫描完成，发现 %u 个AP", ap_count);
    if (ap_count == 0) {
        return strdup("[]");
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG, "AP列表内存分配失败");
        return strdup("[]");
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
        free(ap_list);
        return strdup("[]");
    }

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
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
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

    char *ssid_start = strstr(buf, "ssid=");
    char *pwd_start = strstr(buf, "password=");

    if (ssid_start) {
        ssid_start += 5;
        char *end = strchr(ssid_start, '&');
        if (end) {
            char raw_ssid[64] = {0};
            size_t len = end - ssid_start;
            if (len >= sizeof(raw_ssid)) {
                len = sizeof(raw_ssid) - 1;
            }
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
            if (len >= sizeof(raw_pwd)) {
                len = sizeof(raw_pwd) - 1;
            }
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
        cJSON_AddStringToObject(root, "ip", ip4addr_ntoa(netif_ip4_addr(netif_default)));
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
    if (s_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "启动Web服务器，端口: %d", config.server_port);

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};
        httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};
        httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler};

        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &scan_uri);
        httpd_register_uri_handler(s_server, &connect_uri);
        httpd_register_uri_handler(s_server, &status_uri);

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
