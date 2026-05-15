#ifndef TCP_SERIAL_H
#define TCP_SERIAL_H

#include <stdbool.h>

/**
 * @brief 启动TCP串口透传服务
 *
 * 初始化UART并启动TCP Server，实现TCP↔UART双向数据透传。
 * TCP Server监听指定端口，支持一个客户端连接。
 *
 * @param port  TCP监听端口号
 */
void tcp_serial_start(int port);

/**
 * @brief 停止TCP串口透传服务
 */
void tcp_serial_stop(void);

/**
 * @brief 获取当前TCP串口服务是否正在运行
 */
bool tcp_serial_is_running(void);

#endif // TCP_SERIAL_H
