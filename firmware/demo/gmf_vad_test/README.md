## 多轮对话测试（WakeNet + VADNet）

基于 GMF VAD Element 的多轮对话测试，支持唤醒词唤醒和连续多轮对话。

### 功能特性

| 特性 | 说明 | 状态 |
|------|------|------|
| 唤醒词检测 | 说 "Jarvis" 唤醒设备 | ✅ 已配置 |
| 多轮对话 | 唤醒后可以连续说多句话 | ✅ 正常 |
| VAD句末检测 | 每句话结束自动检测 | ✅ 正常 |
| 自动超时 | 1分钟后自动回到等待唤醒状态 | ✅ 已配置 |

### 当前实现状态

- **VADNet模型**: vadnet1_medium (AFE Pipeline正确加载)
- **WakeNet模型**: wn9_jarvis_tts (AGC模式)
- **多轮对话**: 已验证工作正常（检测到多轮Speech START/END）
- **状态机**: STATE_IDLE → STATE_ACTIVE → 超时处理

### 切换测试模式

编辑 `main/CMakeLists.txt` 切换测试文件：

```cmake
idf_component_register(
    SRCS
        # 基础测试（单轮，仅 VAD）:
        # "vad_test.c"

        # 多轮测试（唤醒词 + 多轮对话）:
        "vad_test_multi.c"
        ...
)
```

切换后重新编译：
```bash
idf.py fullclean
idf.py build
```

### 烧录固件

ESP32-S3 Box Lite 需要使用 esptool 直接烧录：

```bash
PORT=/dev/cu.usbmodem21101  # 根据实际端口调整

# 进入下载模式：按住 BOOT → 短按 RST → 松开 BOOT

/Users/guoshiguang/.espressif/python_env/idf5.5_py3.13_env/bin/python \
  ~/esp/v5.5.2/esp-idf/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 -p $PORT -b 460800 \
  --before default_reset --connect-attempts 20 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/gmf_vad_test.bin \
  0x130000 build/srmodels/srmodels.bin
```

### 多轮测试流程

```
┌─────────────────────────────────────────────────────────────┐
│                        多轮对话流程                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   [STATE_IDLE] ──WakeWord──▶ [STATE_ACTIVE] ──Timeout──▶   │
│       │                        │     ▲                      │
│       │                        │     │                      │
│       │                        ▼     │                      │
│       │                   Speech Start                      │
│       │                        │     │                      │
│       │                        ▼     │                      │
│       │                   Speech End ─┘                      │
│       │                    (可多轮)                         │
│       │                                                     │
│   等待 "Jarvis"                                         1分钟超时 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 预期日志

#### 1. 启动 - 等待唤醒
```
[VAD_MULTI] ========================================
[VAD_MULTI] Multi-Round VAD Test Starting...
[VAD_MULTI] Say 'Jarvis' to wake up
[VAD_MULTI] ========================================
[GMF_VAD_EL] WakeNet enabled: wn9_jarvis_tts
[GMF_VAD_EL] AFE Pipeline: [input] -> |VAD(vadnet1_medium)| -> |WakeNet(wn9_jarvis_tts,)| -> [output]
...
[VAD_MULTI] [STATE_IDLE] Waiting for wake word 'Jarvis'...
```

#### 2. 唤醒成功
```
[GMF_VAD_EL] WakeWord detected!
[VAD_MULTI] === WakeWord Detected! ===
[VAD_MULTI] [STATE_ACTIVE] Wake up success! Start multi-round mode
```

#### 3. 多轮对话
```
[VAD_MULTI] === Round 1: Speech START ===
...
[VAD_MULTI] === Round 1: Speech END ===
[VAD_MULTI] Speech ended. Waiting for next round...

[VAD_MULTI] === Round 2: Speech START ===  ← 可以立即说下一轮
...
[VAD_MULTI] === Round 2: Speech END ===
```

#### 4. 超时结束
```
[VAD_MULTI] [TIMEOUT] 60 seconds passed, going back to idle
[VAD_MULTI] [STATE_STOPPING] Stopping pipeline...
[VAD_MULTI] Multi-Round Test Complete!
[VAD_MULTI] Total rounds: 3
[VAD_MULTI] Total speech segments: 3
```

### 配置参数

在 `vad_test_multi.c` 中调整：

```c
gmf_vad_el_cfg_t vad_cfg = {
    .mode = VAD_MODE_3,
    .min_speech_ms = 300,
    .min_silence_ms = 500,
    .enable_wakenet = true,         // 启用唤醒词
    .wakenet_model = "wn9_jarvis_tts",  // 唤醒词模型
    .callback = on_vad_event,
};
```

超时时间在 `timeout_task` 中设置：
```c
vTaskDelay(pdMS_TO_TICKS(60000));  // 60秒超时
```

### 手工测试步骤

1. **烧录多轮测试固件**
   ```bash
   # 确保 CMakeLists.txt 中使用 vad_test_multi.c
   idf.py build
   esptool.py --chip esp32s3 -p /dev/cu.usbmodem21101 write_flash \
       0x10000 build/gmf_vad_test.bin \
       0x130000 build/srmodels/srmodels.bin
   ```

2. **打开串口监视器**
   ```bash
   idf.py -p /dev/cu.usbmodem21101 monitor
   ```

3. **说唤醒词**
   - 等待出现 `[STATE_IDLE] Waiting for wake word 'Jarvis'...`
   - 清晰地说 "Jarvis"
   - 应看到 `=== WakeWord Detected! ===`

4. **开始多轮对话**
   - 第 1 轮：说任意话语（如 "今天天气怎么样"），等待 Speech END
   - 第 2 轮：立即说下一句话（如 "帮我查一下"），等待 Speech END
   - 第 3 轮：继续说...

5. **等待超时**
   - 停止说话，等待 1 分钟
   - 应看到 `[TIMEOUT] 60 seconds passed`
   - Pipeline 停止，测试完成

### 注意事项

1. **唤醒词模型**：确保 `srmodels.bin` 包含 `wn9_jarvis_tts` 模型（当前配置已包含）
2. **首次唤醒**：WakeNet 首次检测可能需要 1-2 秒初始化
3. **多轮间隔**：Speech END 后可以立即开始下一轮，无需等待
4. **超时重置**：超时后需要复位设备或重新烧录才能再次测试

### 已知问题

| 问题 | 描述 | 状态 |
|------|------|------|
| 唤醒词检测 | 设备有时会跳过唤醒阶段直接进入多轮对话 | 调试中 |
| 调试日志 | 回调日志量较大，可能影响性能 | 已添加条件编译开关 |

### 与基础测试的区别

| 对比项 | 基础测试 (vad_test.c) | 多轮测试 (vad_test_multi.c) |
|--------|----------------------|---------------------------|
| 唤醒方式 | 直接启动，无需唤醒 | 需说 "Jarvis" 唤醒 |
| 对话轮次 | 单轮，检测 Speech END 即停 | 多轮，支持连续对话 |
| 结束条件 | 句末静音 500ms | 句末静音或 1 分钟超时 |
| 循环运行 | 单次 | 单次（超时后停止） |
| WakeNet | 禁用 | 启用 |
