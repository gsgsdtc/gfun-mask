# GMF VAD Element Test Demo
# 测试 VADNet 在 GMF Pipeline 中的独立运行

cmake_minimum_required(VERSION 3.5)

# Set target board
set(BOARD "esp32-s3-box-lite" CACHE STRING "Target board")
set(IDF_TARGET "esp32s3" CACHE STRING "IDF target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gmf_vad_test)
