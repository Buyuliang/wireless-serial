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
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/netif.h"

#define TAG "WEB_SERVER"

static httpd_handle_t s_server = NULL;
static credential_callback_t s_credential_cb = NULL;
static ap_control_callback_t s_ap_close_cb = NULL;

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
".password-wrap{position:relative}"
".password-wrap input{padding-right:52px}"
".eye-btn{position:absolute;top:50%;right:12px;transform:translateY(-50%);width:28px;height:28px;border:none;background:transparent;cursor:pointer;padding:0;color:#667eea;display:flex;align-items:center;justify-content:center}"
".eye-btn svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"
".btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:16px;font-weight:600;cursor:pointer;transition:all 0.3s}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff}"
".btn-primary:hover{transform:translateY(-1px);box-shadow:0 4px 15px rgba(102,126,234,0.4)}"
".btn-primary:disabled{opacity:0.5;cursor:not-allowed;transform:none}"
".btn-scan{background:#f0f0ff;color:#667eea;margin-bottom:12px;font-size:14px;padding:10px}"
".btn-secondary{background:#f0f0ff;color:#4455aa;margin-top:12px}"
".btn-secondary:hover{transform:translateY(-1px);box-shadow:0 4px 15px rgba(68,85,170,0.2)}"
".scan-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:4px;gap:12px}"
".scan-count{font-size:12px;color:#888;white-space:nowrap}"
".scan-auto{font-size:11px;color:#aaa;margin-bottom:12px;display:block}"
".status{text-align:center;padding:12px;border-radius:8px;margin-top:12px;font-size:14px;display:none;line-height:1.6}"
".status.error{display:block;background:#ffe0e0;color:#c00}"
".status.success{display:block;background:#e0ffe0;color:#060}"
".status.loading{display:block;background:#e0e8ff;color:#336}"
".spinner{display:inline-block;width:16px;height:16px;border:2px solid #336;border-top-color:transparent;border-radius:50%;animation:spin 0.8s linear infinite;vertical-align:middle;margin-right:6px}"
"@keyframes spin{to{transform:rotate(360deg)}}"
".hidden{display:none}"
".loading-scan{text-align:center;padding:20px;color:#888}"
".helper{font-size:12px;color:#666;line-height:1.5;margin-top:10px}"
".toast{position:fixed;bottom:30px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:10px 24px;border-radius:20px;font-size:14px;z-index:999;opacity:0;transition:opacity 0.3s;pointer-events:none}"
".toast.show{opacity:1}"
"</style>"
"</head>"
"<body>"
"<div class='card'>"
"<h1>&#x914D;&#x7F51; Wi-Fi &#x8BBE;&#x7F6E;</h1>"
"<p class='subtitle'>&#x8FDE;&#x63A5;&#x5230;ESP32&#x8FDB;&#x884C;&#x914D;&#x7F51;&#x8BBE;&#x7F6E;</p>"
"<div class='scan-header'>"
"<button class='btn btn-scan' type='button' onclick='scanWiFi()'>&#x5237;&#x65B0;&#x626B;&#x63CF; Wi-Fi</button>"
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
"<div class='password-wrap'>"
"<input type='password' id='password' placeholder='Wi-Fi&#x5BC6;&#x7801;'>"
"<button type='button' class='eye-btn' id='togglePassword' onclick='togglePassword()' aria-label='toggle password'>"
"<svg id='eyeOpen' viewBox='0 0 24 24'><path d='M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6Z'></path><circle cx='12' cy='12' r='3'></circle></svg>"
"<svg id='eyeClosed' viewBox='0 0 24 24' class='hidden'><path d='M3 3l18 18'></path><path d='M10.6 10.7a3 3 0 0 0 4.2 4.2'></path><path d='M9.4 5.3A11.4 11.4 0 0 1 12 5c6.5 0 10 7 10 7a17.2 17.2 0 0 1-4 4.8'></path><path d='M6.2 6.3C3.7 8 2 12 2 12s3.5 6 10 6a10.7 10.7 0 0 0 3-.4'></path></svg>"
"</button>"
"</div>"
"</div>"
"<button id='connectBtn' class='btn btn-primary' type='button' onclick='connectWiFi()'>&#x8FDE;&#x63A5; Wi-Fi</button>"
"<div id='status' class='status'></div>"
"</div>"
"<div id='toast' class='toast'></div>"
"<script>"
"let scanTimer = null;"
"window.onload = function(){"
"  scanWiFi();"
"  scanTimer = setInterval(scanWiFi, 30000);"
"  document.getElementById('scanAuto').textContent='自动刷新: 30秒';"
"};"
"function esc(s){var d=document.createElement('div');d.textContent=s||'';return d.innerHTML;}"
"function rssiToBars(rssi){"
"  var level=rssi>-50?4:rssi>-60?3:rssi>-70?2:rssi>-80?1:1;"
"  var heights=[6,10,14,18];"
"  var bars='';"
"  for(var i=0;i<4;i++){bars+='<div class=\"bar'+(i<level?' active':'')+'\" style=\"height:'+heights[i]+'px\"></div>';}"
"  return '<div class=\"bars\">'+bars+'</div>';"
"}"
"function showToast(msg){"
"  var t=document.getElementById('toast');"
"  t.textContent=msg;"
"  t.classList.add('show');"
"  setTimeout(function(){t.classList.remove('show');},2000);"
"}"
"function showCopied(btn){"
"  btn.classList.add('copied');"
"  btn.textContent='✓ 已复制';"
"  setTimeout(function(){btn.classList.remove('copied');btn.textContent='复制';},2000);"
"}"
"function copySSID(ssid, btn){"
"  if(navigator.clipboard && navigator.clipboard.writeText){"
"    navigator.clipboard.writeText(ssid).then(function(){showCopied(btn);showToast('已复制: '+ssid);}).catch(function(){showToast('复制失败');});"
"    return;"
"  }"
"  var ta=document.createElement('textarea');"
"  ta.value=ssid;"
"  ta.style.position='fixed';"
"  ta.style.left='-9999px';"
"  document.body.appendChild(ta);"
"  ta.select();"
"  try{document.execCommand('copy');showCopied(btn);showToast('已复制: '+ssid);}catch(e){showToast('复制失败');}"
"  document.body.removeChild(ta);"
"}"
"function selectWiFi(el, ssid){"
"  document.querySelectorAll('.wifi-item').forEach(function(node){node.classList.remove('selected');});"
"  el.classList.add('selected');"
"  document.getElementById('ssid').value=ssid;"
"  document.getElementById('password').focus();"
"}"
"function togglePassword(){"
"  var p=document.getElementById('password');"
"  var open=document.getElementById('eyeOpen');"
"  var closed=document.getElementById('eyeClosed');"
"  if(p.type==='password'){"
"    p.type='text';"
"    open.classList.add('hidden');"
"    closed.classList.remove('hidden');"
"  }else{"
"    p.type='password';"
"    open.classList.remove('hidden');"
"    closed.classList.add('hidden');"
"  }"
"}"
"function buildWiFiItem(ap){"
"  var item=document.createElement('div');"
"  var lock=ap.auth>0?'[Lock]':'[Open]';"
"  item.className='wifi-item';"
"  item.innerHTML='<div class=\"wifi-name\">'+lock+' '+esc(ap.ssid)+'</div>'+'<div class=\"wifi-right\">'+rssiToBars(ap.rssi)+'<span class=\"rssi\">'+ap.rssi+'dBm</span>'+'<button type=\"button\" class=\"btn-copy\">复制</button></div>';"
"  item.onclick=function(){selectWiFi(item, ap.ssid);};"
"  var copyBtn=item.querySelector('.btn-copy');"
"  copyBtn.onclick=function(event){event.stopPropagation();copySSID(ap.ssid, copyBtn);};"
"  return item;"
"}"
"async function scanWiFi(){"
"  var list=document.getElementById('wifiList');"
"  var st=document.getElementById('scanStatus');"
"  var cnt=document.getElementById('scanCount');"
"  st.classList.remove('hidden');"
"  cnt.textContent='';"
"  try{"
"    var res=await fetch('/scan',{cache:'no-store'});"
"    if(!res.ok){throw new Error('HTTP '+res.status);}"
"    var data=await res.json();"
"    st.classList.add('hidden');"
"    if(!Array.isArray(data)){throw new Error('invalid scan response');}"
"    data=data.filter(function(ap){return ap && ap.ssid && ap.ssid.trim().length>0;});"
"    data.sort(function(a,b){return b.rssi-a.rssi;});"
"    cnt.textContent=data.length+' 个网络';"
"    if(data.length===0){list.innerHTML='<div style=\"padding:20px;text-align:center;color:#888\">未发现可见SSID</div>';return;}"
"    list.innerHTML='';"
"    data.forEach(function(ap){list.appendChild(buildWiFiItem(ap));});"
"  }catch(e){"
"    console.error('scan failed', e);"
"    st.classList.add('hidden');"
"    list.innerHTML='<div style=\"padding:20px;text-align:center;color:#c00\">扫描出错，请重试</div>';"
"  }"
"}"
"async function connectWiFi(){"
"  var ssid=document.getElementById('ssid').value.trim();"
"  var pwd=document.getElementById('password').value;"
"  var btn=document.getElementById('connectBtn');"
"  if(!ssid){showStatus('error','请先选择或输入 Wi-Fi名称');return;}"
"  btn.disabled=true;"
"  showStatus('loading','<span class=\"spinner\"></span>正在连接 '+esc(ssid)+' ...');"
"  try{"
"    var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pwd);"
"    var res=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});"
"    var txt=await res.text();"
"    if(txt==='ok'){"
"      showStatus('success','连接请求已提交，设备正在尝试接入网络...');"
"      pollStatus();"
"    }else{"
"      showStatus('error','连接失败，请检查SSID和密码');"
"      btn.disabled=false;"
"    }"
"  }catch(e){"
"    showStatus('error','连接请求失败');"
"    btn.disabled=false;"
"  }"
"}"
"async function closeProvisionAP(){"
"  try{"
"    var r=await fetch('/ap/close',{method:'POST'});"
"    var t=await r.text();"
"    if(t==='ok'){"
"      showStatus('success','配网热点正在关闭，设备将保持已连接的 Wi-Fi 状态。');"
"      showToast('配网热点将关闭');"
"    }else{"
"      showToast('关闭失败');"
"    }"
"  }catch(e){"
"    showToast('关闭失败');"
"  }"
"}"
"async function pollStatus(){"
"  var tries=0;"
"  var timer=setInterval(async function(){"
"    tries++;"
"    if(tries>30){"
"      clearInterval(timer);"
"      showStatus('error','等待连接超时，请检查 Wi-Fi 密码或路由器状态');"
"      document.getElementById('connectBtn').disabled=false;"
"      return;"
"    }"
"    try{"
"      var r=await fetch('/status',{cache:'no-store'});"
"      var d=await r.json();"
"      if(d.status==='connected' && d.ip){"
"        clearInterval(timer);"
"        var info='✅ Wi-Fi连接成功<br><br>';"
"        info+='设备IP: <b>'+d.ip+'</b><br>';"
"        info+='串口透传端口: <b>8888</b><br><br>';"
"        info+='连接方式:<br>';"
"        info+='&nbsp;&nbsp;Windows: socat COM2 TCP:'+d.ip+':8888<br>';"
"        info+='&nbsp;&nbsp;Linux/Mac: socat /dev/ttyUSB0 TCP:'+d.ip+':8888';"
"        info+='<button type=\"button\" class=\"btn btn-secondary\" onclick=\"closeProvisionAP()\">&#x5173;&#x95ED;&#x914D;&#x7F51;&#x70ED;&#x70B9;</button>';"
"        info+='<div class=\"helper\">&#x9ED8;&#x8BA4;&#x4FDD;&#x6301;&#x70ED;&#x70B9;&#x5F00;&#x542F;&#xFF0C;&#x786E;&#x8BA4;&#x8FDE;&#x63A5;&#x6210;&#x529F;&#x540E;&#x518D;&#x7531;&#x4F60;&#x624B;&#x52A8;&#x5173;&#x95ED;&#x3002;</div>';"
"        showStatus('success',info);"
"        document.getElementById('connectBtn').disabled=false;"
"      }"
"    }catch(e){}"
"  },1000);"
"}"
"function showStatus(type,msg){"
"  var st=document.getElementById('status');"
"  st.className='status '+type;"
"  st.innerHTML=msg;"
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
        char ip_str[16] = {0};
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }

        cJSON_AddStringToObject(root, "status", "connected");
        cJSON_AddStringToObject(root, "ip", ip_str);
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

static esp_err_t ap_close_post_handler(httpd_req_t *req)
{
    if (s_ap_close_cb) {
        s_ap_close_cb();
    }

    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
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
        httpd_uri_t ap_close_uri = {.uri = "/ap/close", .method = HTTP_POST, .handler = ap_close_post_handler};

        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &scan_uri);
        httpd_register_uri_handler(s_server, &connect_uri);
        httpd_register_uri_handler(s_server, &status_uri);
        httpd_register_uri_handler(s_server, &ap_close_uri);

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

void web_server_set_ap_close_callback(ap_control_callback_t callback)
{
    s_ap_close_cb = callback;
}
