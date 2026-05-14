# ESP32 Wi-Fi 配网系统

## 功能概述

一个完整的 ESP32 Wi-Fi 配网解决方案，支持 Web 页面配网和二维码扫描连接。

### 核心流程

```
开机
  ├── 检测BOOT按键是否长按3秒
  │     ├── 是 → 进入配网模式（SoftAP + Web服务器）
  │     └── 否 → 读取NVS中保存的Wi-Fi配置
  │               ├── 有配置 → STA模式连接Wi-Fi
  │               └── 无配置 → 进入配网模式
  │
  └── 配网模式：
        ├── 启动SoftAP（SSID: ESP32_Setup_XXXXXXXX）
        ├── 启动DNS劫持（Captive Portal）
        ├── 生成二维码（内容绑定设备MAC地址）
        └── 手机扫码 → 连接AP → 访问配网页面 → 选择Wi-Fi → 输入密码 → 连接成功
```

## 文件结构

```
esp32_provision/
├── CMakeLists.txt                  # 项目顶层CMake
├── sdkconfig.defaults              # 默认SDK配置
├── README.md
├── main/
│   ├── CMakeLists.txt
│   └── main.c                      # 主程序：按键检测、Wi-Fi模式切换、NVS存储
└── components/
    ├── web_server/
    │   ├── CMakeLists.txt
    │   ├── web_server.h
    │   ├── web_server.c            # HTTP服务器 + 嵌入式配网页面
    │   ├── dns_server.h
    │   └── dns_server.c            # DNS劫持（Captive Portal）
    └── qr_code/
        ├── CMakeLists.txt
        ├── qr_code.h
        └── qr_code.c              # 二维码生成（终端ASCII输出）
```

## 编译和烧录

### 前置条件

- ESP-IDF v4.4+ （推荐 v5.x）
- ESP32 开发板
- USB串口连接

### 编译

```bash
# 设置ESP-IDF环境变量
. $HOME/esp/esp-idf/export.sh

# 进入项目目录
cd esp32_provision

# 编译
idf.py build
```

### 烧录

```bash
# 烧录到ESP32
idf.py -p /dev/ttyUSB0 flash monitor
```

## 使用方法

### 首次配网

1. **烧录固件**后，ESP32启动
2. 因为NVS中没有保存的Wi-Fi配置，自动进入配网模式
3. 串口日志会输出AP名称和二维码内容：
   ```
   设备ID: A1B2C3D4, AP SSID: ESP32_Setup_A1B2C3D4
   二维码内容: WIFI:S:ESP32_Setup_A1B2C3D4;T:WPA2;P:12345678;;
   ```
4. **手机扫码**（或手动连接Wi-Fi）：
   - Wi-Fi名称: `ESP32_Setup_A1B2C3D4`（每个设备唯一，基于MAC地址）
   - 密码: `12345678`
5. 连接后手机会**自动弹出配网页面**（Captive Portal）
   - 如未弹出，手动访问 `http://192.168.4.1`
6. 点击 **"扫描附近Wi-Fi"** 查看可用热点
7. 选择你的Wi-Fi，输入密码，点击 **"连接网络"**
8. 连接成功后，配置保存到NVS，下次开机自动连接

### 重新配网

- 开机时**长按BOOT键（GPIO0）3秒以上**，进入配网模式

## 设备唯一标识方案

每个设备生成唯一的AP名称，绑定硬件MAC地址：

```
AP SSID = "ESP32_Setup_" + MAC后4字节
例如: ESP32_Setup_A1B2C3D4
```

二维码内容格式（标准Wi-Fi QR格式）：
```
WIFI:S:ESP32_Setup_A1B2C3D4;T:WPA2;P:12345678;;
```

这种方式保证：
- 每个设备的AP名称和二维码**全球唯一**
- 不需要额外存储设备ID
- 手机扫描后可以直接连接对应设备的AP

### 进阶方案（可选）

| 方案 | 说明 | 安全等级 |
|------|------|---------|
| MAC地址后4字节 | 当前方案，简单可靠 | ⭐⭐⭐ |
| MAC + 芯片ID(eFuse) | 读取ESP32内置唯一ID | ⭐⭐⭐⭐ |
| UUID v4 随机生成 | 首次启动生成，存入NVS | ⭐⭐⭐⭐ |
| JWT令牌 | 服务端签发，带时效 | ⭐⭐⭐⭐⭐ |

## 自定义配置

修改 `main.c` 中的宏定义：

```c
#define PROVISION_BUTTON_GPIO   GPIO_NUM_0   // 配网按键GPIO
#define AP_SSID_PREFIX          "ESP32_Setup_"  // AP名称前缀
#define AP_PASSWORD             "12345678"   // AP密码（至少8位）
#define AP_CHANNEL              1            // AP信道
#define BUTTON_HOLD_TIME_MS     3000         // 长按时间阈值
```

## Web配网页面

嵌入式响应式HTML页面，支持：
- 📶 Wi-Fi扫描，按信号强度排序
- 📊 信号强度可视化（信号条 + dBm数值）
- 🔒 显示加密类型
- 📱 手机自适应布局
- 🎨 渐变紫色主题UI

## 注意事项

1. AP密码必须至少8个字符（WPA2要求）
2. 首次烧录会擦除NVS，需重新配网
3. DNS劫持仅在配网模式下运行，正常连接Wi-Fi后不启动
4. 如果连接保存的Wi-Fi失败5次，需要手动重启进入配网模式

## License

MIT
