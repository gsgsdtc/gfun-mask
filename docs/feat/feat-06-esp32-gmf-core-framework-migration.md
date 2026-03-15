<!-- 写作边界：本文档是需求文档，只写"做什么"和"为什么"。
     禁止写入：代码片段、API 设计、数据库 schema、技术方案、实现步骤。
     技术设计由下游 feat-review-design skill 负责。 -->

# Feat-06: ESP32 固件迁移至 GMF-Core 音频框架

> 更新日期：2026-03-15
> 来源：内部架构决策（feat-05 废弃后的替代方案；参考 [GMF-Core v0.7.10](https://components.espressif.com/components/espressif/gmf_core/versions/0.7.10/readme)、[gmf_io v0.7.6](https://components.espressif.com/components/espressif/gmf_io/versions/0.7.6/readme)）
> 优先级：P0（feat-04 VAD 的前置依赖，feat-04 需在本 feat 合并后方可实施）

## 1. 功能概述

### 1.1 背景

当前 ESP32 固件的音频处理采用自定义架构：手写 FreeRTOS 任务（I2S 读取任务 + 编码任务）通过 Queue 传递 PCM 数据。这套架构在 feat-03（Phase 3）基本满足需求，但存在以下问题：

- 任务调度和数据传递完全手写，维护成本高、扩展能力弱
- 音频处理单元之间缺乏统一的生命周期管理（启动/停止/暂停/恢复）
- 引入新的音频处理能力（如 VAD、降噪等）需要大量重写底层调度逻辑

此前 feat-05 尝试通过迁移至 ESP-ADF 解决此问题，但因 ESP-ADF v2.7 与 IDF v5.5 存在 API 不兼容问题（`xSemaphoreHandle` 等 IDF v4 API 已废弃），feat-05 已废弃。

**ESP-GMF（Espressif General Multimedia Framework）** 是 Espressif 官方推出的下一代多媒体框架，以独立 ESP 组件形式发布，无 IDF 版本绑定限制。本次迁移基于以下组件重建音频层：

| 组件 | 版本 | 职责 |
|------|------|------|
| **gmf_core** | ≥ v0.7.10 | Pipeline / Element / Task 调度框架 |
| **gmf_io** | ≥ v0.7.6 | IO 扩展接口（用于包装现有麦克风驱动 HAL） |

**关于音频输入**：VoiceMask 的麦克风输入存在多种方案，本 feat 以 **ESP32-S3-BOX-Lite 现有麦克风为主验证目标**，不改变当前已支持的麦克风配置。BOX-Lite 板载麦克风芯片为 **ES7243E**（I2C 地址 0x10），经 I2S 传输至 ESP32-S3，现有 `audio_driver.h` HAL 已封装其初始化和读取逻辑并通过验证。

本 feat 将在 GMF IO 层**包装现有 `audio_driver` HAL**，而非直接替换为 `io_codec_dev`，以保留已验证的 ES7243E 特殊初始化序列（三步 Soft Reset），降低迁移风险。

后续实验阶段可能采用其他输入方案（片内 ADC、PDM 麦克风、其他 Codec 等），GMF IO 层以**可替换的方式**设计，不同方案通过替换 IO 实现即可，上层 Pipeline 无需改动。

**关于音频编码**：当前编码层为 PCM passthrough（`opus_encoder.c` 直接 memcpy，未集成真正的 Opus）。本 feat 为纯架构重构，**不改变编码行为**，迁移后继续保持 PCM passthrough。真正的 Opus 编码集成将在后续独立 feat 中完成（见后续改进）。

> 📌 **后续改进**：真正的 Opus 编码集成（使用 `gmf_audio` Opus Element 替换 PCM passthrough）将作为独立 feat 跟进，需在本 feat 合并后评估 iOS 端 Opus 解码能力后推进。

### 1.2 目标

- 以 GMF Pipeline 替换现有手写 FreeRTOS 双任务架构（I2S 读取任务 + 编码任务 + Queue）
- 将现有 `audio_driver` HAL 包装为 GMF IO 源，接入 GMF Pipeline
- 迁移后**保持现有 PCM passthrough 编码行为不变**，与 iOS 端 BLE L2CAP 帧协议完全兼容
- 为 feat-04 VAD 预留 Pipeline 中的处理节点接入点

### 1.3 非目标

- 不切换为真正的 Opus 编码（保持 PCM passthrough，Opus 集成属于后续 feat）
- 不改变 ESP32-S3-BOX-Lite 现有麦克风（ES7243E）的驱动配置，迁移后硬件行为保持一致
- 不在本 feat 实验其他麦克风输入方案（片内 ADC、PDM 等属于后续实验 feat）
- 不实现 VAD、多麦克风阵列采集或降噪（AEC）等高级音频处理
- 不改变 BLE L2CAP 通信协议或帧格式
- 不改变 iOS 端任何代码

## 2. 用户场景

> 本 feat 为纯固件架构重构，用户可见行为与迁移前完全一致。以下场景用于验证迁移后功能无回归。

### 场景 1: 正常语音录制并发送

- **角色**：佩戴 VoiceMask 的用户
- **触发**：iOS 端发出开始录音指令（`CMD_START_RECORD`）
- **流程**：
  1. ESP32 收到指令，启动 GMF Pipeline
  2. GMF IO 源通过 ES7243E 采集麦克风 PCM 数据
  3. PCM 数据经 Pipeline 处理后打包为音频帧（PCM passthrough）
  4. 音频帧通过 BLE L2CAP 发送至 iOS
  5. iOS 收到音频帧并正常处理
- **结果**：语音聊天流程正常运行，延迟和质量与迁移前一致

### 场景 2: 停止录音并恢复待机

- **角色**：佩戴 VoiceMask 的用户
- **触发**：iOS 端发出停止录音指令（`CMD_STOP_RECORD`）或超时
- **流程**：
  1. ESP32 收到停止指令
  2. Pipeline 有序停止，ES7243E 采集停止，资源正确释放
  3. 固件进入待机状态，等待下次指令
- **结果**：无资源泄漏，下次录音可正常启动

### 场景 3: BLE 断开后重连

- **角色**：BLE 连接意外中断的用户
- **触发**：BLE 连接断开（走出范围、手机锁屏等）
- **流程**：
  1. Pipeline 因无 BLE 输出目标而停止，麦克风采集随之停止
  2. 用户重新连接 BLE
  3. Pipeline 重新启动，麦克风采集恢复
- **结果**：重连后语音功能完全恢复，无需重启设备

## 3. 验收标准

### 功能验收

- [ ] 语音录制流程端到端正常：ES7243E 采集 → GMF Pipeline → PCM 帧 → BLE L2CAP → iOS 接收
- [ ] Pipeline 可正确响应 start、stop、pause、resume 生命周期指令，麦克风资源随 Pipeline 正确开关
- [ ] BLE 断开时 Pipeline 安全停止，重连后可重新启动（无 crash、无资源泄漏）
- [ ] 与 feat-03 行为对比：相同指令序列下，iOS 收到的音频帧格式和内容等价（PCM passthrough 行为不变）
- [ ] 固件在 ESP32-S3-BOX-Lite 上编译成功并正常运行，IDF 版本 v5.5

### 非功能验收

- [ ] 性能：端到端音频延迟不超过 feat-03 基准的 120%（当前基准约 80ms）
- [ ] 内存：固件在 idle 状态下 heap free 不低于 feat-03 基准的 80%
- [ ] 稳定性：连续运行 10 分钟语音聊天无 crash 或任务 watchdog 超时

## 4. 约束条件

- **ESP 组件依赖**（通过 `idf_component.yml` 引入）：
  - `gmf_core` ≥ v0.7.10
  - `gmf_io` ≥ v0.7.6（提供 IO 扩展接口）
- **IDF 版本**：ESP-IDF v5.5（与现有项目保持一致）
- **主测试硬件**：ESP32-S3-BOX-Lite（实验阶段主力硬件，后续会陆续引入其他实验组合）
- **HAL 保留**：现有 `audio_driver.h` + `audio_driver_es7210.c`（ES7243E 驱动）保持不变，作为 GMF IO 层的底层实现
- **IO 可替换性**：GMF IO 层以可替换方式设计，后续实验不同输入方案时，仅替换 IO 实现，上层 Pipeline 无需改动
- **BLE 协议兼容**：帧格式必须与 iOS 端 `L2CAPHandler.swift` 现有解析逻辑完全兼容
- **现有 BLE 模块不变**：`ble_gap.c`、`ble_gatts.c`、`ble_l2cap.c` 不在本 feat 修改范围内
- **依赖前置**：feat-03 已合并且通过验收；feat-05 已废弃，不应有遗留代码依赖
