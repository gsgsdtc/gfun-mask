# Design: 06 - ESP32 固件迁移至 GMF-Core

> 所属模块：audio-capture
> 关联需求：docs/feat/feat-06-esp32-gmf-core-framework-migration.md
> 关联前端设计：无
> 更新日期：2026-03-15
> 状态：草稿

## 1. 设计概述

### 1.1 目标

将 `audio_pipeline.c` 内部的手写 FreeRTOS 双任务架构（I2S 读取任务 + 编码任务 + Queue）替换为 GMF-Core Pipeline，对外保持 `audio_pipeline.h` 公开接口完全不变，业务行为（PCM passthrough + BLE L2CAP 发送）不变。

### 1.2 设计约束

- `audio_pipeline.h` 公开接口（`init/start/stop/get_state/get_frame_count/deinit`）**严禁变更**，调用方（`main.c`、BLE 控制回调）零修改
- `audio_driver.h` + `audio_driver_es7210.c`（ES7243E 驱动）**保持不变**，作为 GMF IO 底层
- `opus_encoder.h/.c`（PCM passthrough）**保持不变**，由 GMF Element 内部调用
- BLE 模块（`ble_gap`、`ble_gatts`、`ble_l2cap`）**不在修改范围**
- ESP-IDF v5.5，使用 Legacy I2S API（`driver/i2s.h`）不改动

---

## 2. 接口设计

### 2.1 对外接口（不变）

`audio_pipeline.h` 接口定义完全保留：

| 函数 | 说明 | 变更 |
|------|------|------|
| `audio_pipeline_init()` | 初始化驱动 + 编码器 + GMF Pipeline | 内部重写，签名不变 |
| `audio_pipeline_start()` | 运行 GMF Pipeline | 内部重写，签名不变 |
| `audio_pipeline_stop()` | 停止 Pipeline，发送 RECORD_END | 内部重写，签名不变 |
| `audio_pipeline_get_state()` | 返回当前状态 | 不变 |
| `audio_pipeline_get_frame_count()` | 返回已发送帧数 | 不变 |
| `audio_pipeline_deinit()` | 释放 Pipeline 和驱动资源 | 内部重写，签名不变 |

### 2.2 新增内部模块接口

#### `gmf_mic_io`（自定义 GMF IO 源）

| 函数 | 说明 |
|------|------|
| `gmf_mic_io_create(pool)` | 创建 GMF IO 源，注册到 Pool |
| open job | 调用 `audio_driver->start()` |
| read job | 调用 `audio_driver->read(buf, samples)` |
| close job | 调用 `audio_driver->stop()` |

基于 `gmf_io` 提供的 IO 扩展接口实现，底层委托给 `audio_driver_ops_t` HAL。

#### `gmf_pcm_enc_el`（自定义 GMF 编码 Element）

| 函数 | 说明 |
|------|------|
| `gmf_pcm_enc_el_create(pool)` | 创建 Element，注册到 Pool |
| open job | 调用 `opus_encoder_init()`，重置帧计数 |
| process job | 从输入 Port 读取 PCM → 调用 `opus_encoder_encode()` → `ble_l2cap_send_frame()` |
| close job | 调用 `opus_encoder_deinit()` |

### 2.3 依赖变更

新增 `firmware/idf_component.yml`（或在现有 CMake 中追加）：

| 组件 | 版本 | 用途 |
|------|------|------|
| `espressif/gmf_core` | `^0.7.10` | Pipeline / Element / Task 框架 |
| `espressif/gmf_io` | `^0.7.6` | IO 扩展基础接口 |

---

## 3. 模型设计

### 3.1 GMF Pipeline 结构

```
┌─────────────────────────────────────────────────────────┐
│                     GMF Pipeline                        │
│                                                         │
│  ┌──────────────────┐     ┌──────────────────────────┐  │
│  │  gmf_mic_io      │────►│  gmf_pcm_enc_el          │  │
│  │  (IO Source)     │ PCM │  (PCM passthrough)        │  │
│  │                  │     │  → ble_l2cap_send_frame() │  │
│  │  底层：          │     │  底层：opus_encoder.c     │  │
│  │  audio_driver    │     │  (memcpy + frame count)   │  │
│  │  + ES7243E       │     │                           │  │
│  └──────────────────┘     └──────────────────────────┘  │
│                                                         │
│  GMF Task（单 Task，管理 Pipeline 调度）                  │
└─────────────────────────────────────────────────────────┘
```

### 3.2 状态机（不变）

```
┌──────────┐  audio_pipeline_start()  ┌───────────┐  audio_pipeline_stop()  ┌──────────┐
│   IDLE   │ ────────────────────────► │ RECORDING │ ──────────────────────► │   IDLE   │
└──────────┘                           └───────────┘                         └──────────┘
                                             │
                                    BLE 发送持续失败
                                             ▼
                                        ┌─────────┐
                                        │  ERROR  │ ──► 自动恢复为 IDLE
                                        └─────────┘
```

### 3.3 数据流格式（不变）

| 字段 | 字节数 | 说明 |
|------|--------|------|
| frame_type | 1 | `0x01` AUDIO |
| length | 2 | payload 长度（640 = 320 samples × 2 bytes） |
| payload | 640 | raw PCM int16，单声道，16kHz，16-bit |

---

## 4. 逻辑设计

### 4.1 核心流程：audio_pipeline_init()

```
audio_pipeline_init()
│
├─ 检查 audio_driver 已注册
├─ audio_driver->init()          ← ES7243E: I2S 安装 + MCLK 输出 + I2C 配置 + 三步 Soft Reset
├─ opus_encoder_init()           ← PCM passthrough 初始化（重置帧计数）
│
├─ gmf_pool_create()             ← 创建 GMF Pool
├─ gmf_mic_io_create(pool)       ← 注册 IO 源到 Pool
├─ gmf_pcm_enc_el_create(pool)   ← 注册 Element 到 Pool
│
└─ gmf_pipeline_create()         ← 创建 Pipeline，关联 Pool + Task
   └─ gmf_pipeline_bind_task()   ← 绑定 GMF Task（单线程驱动 Pipeline）
```

### 4.2 核心流程：audio_pipeline_start()

```
audio_pipeline_start()
│
├─ 检查 state == IDLE（ERROR 状态自动重置为 IDLE）
├─ opus_encoder_reset_count()
├─ s_state = AUDIO_STATE_RECORDING
│
└─ gmf_pipeline_run()
   │
   ├─ [Opening Phase]
   │   ├─ gmf_mic_io.open()      → audio_driver->start() → i2s_start()
   │   └─ gmf_pcm_enc_el.open()  → opus_encoder_init()（已在 init 中调用，此处幂等）
   │
   └─ [Running Phase - GMF Task 循环执行]
       └─ gmf_pcm_enc_el.process()
           ├─ 从 IO Port 读取 PCM（触发 gmf_mic_io.read → audio_driver->read）
           ├─ opus_encoder_encode(pcm, buf)   ← PCM passthrough（memcpy）
           ├─ 等待 ble_l2cap_is_tx_ready()    ← 流控检查
           └─ ble_l2cap_send_frame(AUDIO, buf, len)
```

### 4.3 核心流程：audio_pipeline_stop()

```
audio_pipeline_stop()
│
├─ 检查 state == AUDIO_STATE_RECORDING
├─ gmf_pipeline_stop()
│   └─ [Cleanup Phase]
│       ├─ gmf_pcm_enc_el.close()  → opus_encoder_deinit()
│       └─ gmf_mic_io.close()      → audio_driver->stop() → i2s_stop()
│
├─ s_state = AUDIO_STATE_IDLE
│
└─ ble_l2cap_send_frame(RECORD_END, &total_frames, 4)
```

### 4.4 业务规则

- **ERROR 自动恢复**：BLE 发送失败不立即停止 Pipeline；连续失败超阈值时 `s_state = ERROR`，下次 `start()` 自动重置为 IDLE
- **BLE 流控等待**：`process()` 中若 `ble_l2cap_is_tx_ready()` 为 false，自旋等待（同现有逻辑），不丢帧
- **ES7243E 初始化顺序约束**：`audio_driver->init()` 必须在 GMF Pipeline 启动前完成（确保 MCLK 先于 I2C 配置）；此约束由 `audio_pipeline_init()` 顺序保证

### 4.5 边界/异常处理

| 异常 | 处理 |
|------|------|
| `gmf_pipeline_run()` 失败 | 返回 -1，`s_state` 保持 IDLE，driver->stop() 清理 |
| `audio_driver->read()` 返回负值 | gmf_mic_io 向 Pipeline 上报错误，Pipeline 进入 Cleanup Phase |
| BLE 连接断开（`ble_l2cap_is_tx_ready` 持续 false） | 超时后 `s_state = ERROR`，Pipeline 停止；重连后可重新 start() |
| `gmf_pipeline_stop()` 时 Task 未及时退出 | 等待 Task notify 信号，超时后强制删除（同现有 `vTaskDelay(100ms)` 逻辑） |

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | Mock 边界 |
|------|------|----------|
| 单元测试 | `gmf_mic_io`、`gmf_pcm_enc_el` 各自 open/process/close 逻辑 | Mock `audio_driver_ops_t`、Mock `ble_l2cap_send_frame` |
| 集成测试（板上） | 完整 Pipeline 在 ESP32-S3-BOX-Lite 实机运行 | 无 Mock，连接 iOS 真实验证 |
| 回归对比测试 | feat-03 相同指令序列，iOS 收到音频帧内容和格式等价 | 无 Mock |

### 5.2 关键用例

| 用例 | 输入 | 期望结果 |
|------|------|---------|
| 正常录制启停 | `init → start → 10s 录音 → stop` | iOS 收到 ≥ 490 个正确 PCM 帧，RECORD_END 帧含正确总帧数 |
| 重复启停 | `start → stop → start → stop` × 3 | 每次均正常，无资源泄漏，heap free 不下降 |
| ERROR 恢复 | BLE 断开后重连，再次 start | Pipeline 正常重启，无 crash |
| 内存基准 | idle 状态 `esp_get_free_heap_size()` | ≥ feat-03 基准的 80% |
| 延迟基准 | 端到端音频延迟（iOS 侧测量） | ≤ feat-03 基准 × 1.2（约 96ms） |

---

## 6. 影响评估

### 6.1 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `firmware/main/audio/audio_pipeline.c` | **重写** | 内部实现替换为 GMF Pipeline，接口不变 |
| `firmware/main/audio/gmf_mic_io.c/.h` | **新增** | 自定义 GMF IO 源，包装 `audio_driver_ops_t` |
| `firmware/main/audio/gmf_pcm_enc_el.c/.h` | **新增** | 自定义 GMF 编码 Element，包装 `opus_encoder` |
| `firmware/idf_component.yml` | **新增/更新** | 添加 `gmf_core`、`gmf_io` 依赖 |
| `firmware/main/CMakeLists.txt` | **更新** | 添加新文件到编译列表 |

### 6.2 不变文件

| 文件 | 原因 |
|------|------|
| `audio_pipeline.h` | 公开接口契约，严禁变更 |
| `audio_driver.h/.c` | HAL 抽象层，直接复用 |
| `audio_driver_es7210.c` | ES7243E 驱动已验证，保留 |
| `opus_encoder.h/.c` | PCM passthrough，由 Element 调用 |
| `ble_gap/gatts/l2cap.c/.h` | BLE 模块不在范围 |
| `boards/esp32_s3_box_lite.h` | 硬件引脚配置不变 |
| `main.c` | 调用方不变 |

### 6.3 对其他模块的影响

- **ble-channel 模块**：无影响，帧格式和控制指令流程不变
- **voice-chat / pipecat-pipeline**：无影响，iOS 端接收的音频内容不变
- **feat-04 VAD**：本设计在 Pipeline 的 `gmf_pcm_enc_el` 前预留了可插入处理 Element 的位置，feat-04 可在此插入 VAD Element

### 6.4 回滚方案

- 本次所有改动在独立分支 `feat/06-esp32-gmf-core-framework-migration`
- `audio_pipeline.h` 接口不变，回滚只需将 `audio_pipeline.c` 替换为旧版本，删除 `gmf_mic_io`、`gmf_pcm_enc_el` 文件，移除 `idf_component.yml` 中的 GMF 依赖
- 无数据库 / 协议格式变更，回滚无副作用
