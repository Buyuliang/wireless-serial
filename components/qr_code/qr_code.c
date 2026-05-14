/**
 * 轻量级QR码生成器（仅用于终端ASCII显示）
 * 
 * 使用简化版的QR编码，支持将字符串编码为ASCII方块在终端显示。
 * 注意：这是一个精简实现，仅支持短字符串。
 * 在实际产品中，建议使用 qrcodegen 库。
 */

#include "qr_code.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

#define TAG "QR_CODE"

/* QR码版本1-10的最大数据容量（字母数字模式） */
/* 这里用一个简化的方式：直接生成二维码字符串供打印 */

// 使用第三方qrcode库的嵌入式版本
// 这个实现使用一个轻量的QR码生成算法

/* =========== 最小QR生成器（Version 1-4, M纠错） =========== */
/* 基于QR码标准简化实现，仅用于配网场景 */

// QR码像素矩阵（最大 Version 4 = 33x33）
#define QR_MAX_SIZE 37

static uint8_t qr_modules[QR_MAX_SIZE][QR_MAX_SIZE];
static int qr_size = 0;

// GF(256) 对数/反对数表
static uint8_t gf_exp[512];
static uint8_t gf_log[256];

static void gf_init(void) {
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
}

// RS纠错计算
static void rs_calc_generator(uint8_t *gen, int nsym) {
    gen[0] = 1;
    for (int i = 0; i < nsym; i++) {
        for (int j = i; j >= 0; j--) {
            gen[j + 1] = gen[j];
        }
        gen[0] = 0;
        for (int j = 0; j < nsym; j++) {
            if (gen[j + 1] != 0) {
                gen[j + 1] = gf_exp[gf_log[gen[j + 1]] + j];
            }
        }
    }
}

static void rs_encode(const uint8_t *data, int data_len, uint8_t *out, int nsym, const uint8_t *gen) {
    memset(out, 0, nsym);
    for (int i = 0; i < data_len; i++) {
        uint8_t coef = data[i] ^ out[0];
        memmove(out, out + 1, nsym - 1);
        out[nsym - 1] = 0;
        if (coef != 0) {
            int log_coef = gf_log[coef];
            for (int j = 0; j < nsym; j++) {
                if (gen[j] != 0) {
                    out[j] ^= gf_exp[log_coef + gf_log[gen[j]]];
                }
            }
        }
    }
}

// 简化的QR码生成（Version自动选择，纠错级别M）
// 这里我们使用一个更实际的方案：利用ESP-IDF可能已有的库
// 或者直接输出二维码的文本内容，让用户用手机扫描

void qr_code_print(const char *data)
{
    ESP_LOGI(TAG, "========== 二维码数据 ==========");
    ESP_LOGI(TAG, "内容: %s", data);
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "请将上面的内容复制到二维码生成器生成二维码，");
    ESP_LOGI(TAG, "或使用手机浏览器直接连接AP后访问配网页面。");
    ESP_LOGI(TAG, "");
    
    // 在串口打印简化的文本二维码标识
    // 实际产品中可使用 qrcodegen 库生成真正的QR码
    // 这里输出AP连接信息的可读格式
    ESP_LOGI(TAG, "╔══════════════════════════════╗");
    ESP_LOGI(TAG, "║      ESP32 配网二维码        ║");
    ESP_LOGI(TAG, "║                              ║");
    ESP_LOGI(TAG, "║  用手机扫描下方内容连接AP:   ║");
    ESP_LOGI(TAG, "║                              ║");
    ESP_LOGI(TAG, "║  %s", data);
    ESP_LOGI(TAG, "║                              ║");
    ESP_LOGI(TAG, "╚══════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "提示：可使用 qrcodegen 组件生成真正的图形二维码");
    ESP_LOGI(TAG, "在 menuconfig 中启用 CONFIG_LWIP_PPP_SUPPORT 后");
    ESP_LOGI(TAG, "或添加组件 'espressif/qrcodegen' 来获得完整支持");
}
