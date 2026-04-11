# GMF VAD Element Demo

测试 VADNet 在 GMF Pipeline 中的独立运行。

## 架构

```
Mic IO ──▶ VAD Element ──▶ PCM Encoder ──▶ BLE Stub
                │
                └──▶ 检测 SPEECH_END ──▶ 自动停止 Pipeline
```

## 文件说明

| 文件 | 说明 |
|------|------|
| `gmf_vad_el.c/h` | VAD Element 核心实现（封装 VADNet） |
| `vad_test.c` | Demo 主程序 |
| `ble_l2cap_stub.c` | BLE 桩（仅打印日志） |

## 缺少的文件

以下文件需要从主项目复制或创建软链接：

- `gmf_pcm_enc_el.c/h` → 从 `firmware/main/audio/` 复制
- `gmf_mic_io.c/h` → 从 `firmware/main/audio/` 复制
- `audio_driver.c/h` → 从 `firmware/main/audio/` 复制
- `audio_driver_es7210.c` → 从 `firmware/main/audio/` 复制
- `opus_encoder.c/h` → 从 `firmware/main/audio/` 复制
- `audio_pipeline.h` → 从 `firmware/main/audio/` 复制

## 编译步骤

```bash
cd firmware/demo/gmf_vad_test
get_idf
idf.py build
```

## 测试流程

1. 烧录固件
2. 打开串口监视器
3. 说 "嗨乐鑫"（或任何话）
4. 停止说话
5. 观察日志：
   - `=== VAD: Speech START ===`
   - `Silence detected (count=1/2/3...)`
   - `=== VAD: Speech END (Auto-stop) ===`

## 配置参数

在 `vad_test.c` 中可调整：

```c
.min_speech_ms = 300,   // 最小语音持续时间
.min_silence_ms = 500,  // 触发句末的静音时长
```
