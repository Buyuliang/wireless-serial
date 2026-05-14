# ESP32 Wi-Fi 配网项目
cmake_minimum_required(VERSION 3.16)

# 必须在 project() 之前设置芯片目标

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_provision)
