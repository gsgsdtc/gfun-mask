# ESP32-S3-BOX-Lite 音频采集参考文档

> 基于 [esp-box](https://github.com/espressif/esp-box) 项目 Review，面向 VoiceMask 项目的音频采集参考。

---

## 1. ESP32-S3-BOX-Lite 音频硬件架构

```
ESP32-S3
  │
  ├── I2S0 (Master)
  │     ├── MCLK  → GPIO 2   → ES7243E (ADC Clock)
  │     ├── BCLK  → GPIO 17  → ES7243E / ES8311
  │     ├── LRCK  → GPIO 47  → ES7243E / ES8311
  │     ├── DOUT  → GPIO 15  → ES8311 (DAC Data, 扬声器)
  │     └── DIN   ← GPIO 16  ← ES7243E (ADC Data, 麦克风)
  │
  ├── I2C0
  │     ├── SDA   → GPIO 8
  │     └── SCL   → GPIO 18
  │
  ├── ES7243E (麦克风 ADC, I2C 地址 0x10)
  │     └── 2x MEMS MIC → 立体声输入
  │
  └── ES8311  (扬声器 DAC, I2C 地址 0x18)
        └── Class-D Amp → 扬声器
```

**关键差异**：BOX-Lite 没有 ES7210（四路麦克风），只有 **ES7243E**（双路麦克风）。

---

## 2. esp-box BSP 音频采集方式（推荐参考）

esp-box 使用以下层次结构：

```
bsp_i2s_read()
    └── esp_codec_dev_read(record_dev_handle, ...)
            └── [esp_codec_dev 框架]
                    └── [I2S std driver: i2s_std.h]
                            └── [ES7243E via I2C]
```

### 2.1 BSP 关键参数（来自 esp32_bsp_board.c）

```c
#define CODEC_DEFAULT_SAMPLE_RATE   16000
#define CODEC_DEFAULT_BIT_WIDTH     16
#define CODEC_DEFAULT_ADC_VOLUME    24.0f   // dB
#define CODEC_DEFAULT_CHANNEL       2        // 注意：始终用 2 声道！
```

> **重要**：即使只用单声道麦克风，BSP 始终以 **2 声道**读取 I2S。
> 读完后通过数据处理提取所需的单声道数据。

### 2.2 BSP I2S 初始化参考（来自 chatgpt_demo/main.c）

```c
/* BSP I2S 引脚定义 */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK, /* GPIO 2  */ \
        .bclk = BSP_I2S_SCLK, /* GPIO 17 */ \
        .ws   = BSP_I2S_LCLK, /* GPIO 47 */ \
        .dout = BSP_I2S_DOUT, /* GPIO 15 */ \
        .din  = BSP_I2S_DSIN, /* GPIO 16 */ \
        .invert_flags = {0},                 \
    }

/* Duplex Mono 配置（BSP 默认） */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate) \
    {                                          \
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate), \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(        \
                        I2S_DATA_BIT_WIDTH_16BIT,              \
                        I2S_SLOT_MODE_MONO),                   \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                          \
    }

/* 使用方式 */
i2s_std_config_t i2s_config = BSP_I2S_DUPLEX_MONO_CFG(16000);
i2s_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;  // 重要！
bsp_audio_init(&i2s_config);
```

> **MCLK 倍率**：BOX-Lite 使用 `I2S_MCLK_MULTIPLE_384`（即 MCLK = 384 × LRCK = 384 × 16kHz = 6.144MHz）。

### 2.3 AFE 音频读取方式（来自 chatgpt_demo/main/app/app_sr.c）

```c
#define I2S_CHANNEL_NUM  2  // 固定读 2 声道

static void audio_feed_task(void *arg)
{
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = 3;  // AFE 需要 3 声道（2 MIC + 1 REF）

    // 分配 2 声道的缓冲区
    int16_t *audio_buffer = heap_caps_malloc(
        audio_chunksize * sizeof(int16_t) * feed_channel,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    while (true) {
        // 读取 2 声道数据（stereo）
        bsp_i2s_read((char *)audio_buffer,
                     audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t),
                     &bytes_read,
                     portMAX_DELAY);

        // 将 2 声道扩展为 3 声道（第 3 声道填 0 作为参考）
        for (int i = audio_chunksize - 1; i >= 0; i--) {
            audio_buffer[i * 3 + 2] = 0;
            audio_buffer[i * 3 + 1] = audio_buffer[i * 2 + 1];
            audio_buffer[i * 3 + 0] = audio_buffer[i * 2 + 0];
        }

        afe_handle->feed(afe_data, audio_buffer);
    }
}
```

---

## 3. ES7243E 关键知识点

### 3.1 ES7243E vs ES7210 对比

| 特性 | ES7243E（BOX-Lite）| ES7210（BOX / BOX-3）|
|------|------------------|---------------------|
| 通道数 | 2 | 4 |
| I2C 地址 | 0x10 | 0x40-0x43 |
| MCLK 倍率 | 256x 或 384x | 256x |
| I2C 初始化要求 | **必须先有 MCLK** | 必须先有 MCLK |

### 3.2 ES7243E 上电时序（关键！）

```
上电顺序（严格）：
  1. 配置 I2S 硬件 → MCLK 开始输出到 GPIO 2
  2. 等待 ≥100ms（ES7243E 需要看到稳定的 MCLK 才响应 I2C）
  3. 初始化 I2C
  4. 写入 ES7243E 寄存器（一次写完，中间不能 Soft Reset）
  5. 最终使能 ADC（reg 0x16 = 0x00）
  6. 启动 I2S DMA（i2s_start 或 i2s_channel_enable）
```

> **关键约束**：Soft Reset（reg 0x00 = 0x80）会将**所有寄存器**恢复默认值。
> 在时钟/接口寄存器配置后不能再执行 Soft Reset。

### 3.3 ES7243E 时钟寄存器计算

对于 16kHz 采样率，MCLK_MULTIPLE = 256：

```
MCLK  = 256 × LRCK = 256 × 16000 = 4.096 MHz
BCLK  = 16bit × 2ch × LRCK = 16 × 2 × 16000 = 512 kHz
BCLK  = MCLK / 8

寄存器配置：
  reg 0x06 = 0x07  → SCLK_DIV = (0x07+1) = 8 → BCLK = MCLK/8 = 512kHz  ✓
  reg 0x07 = 0x00  → LRCK_DIV 高字节
  reg 0x08 = 0xFF  → LRCK_DIV 低字节 → DIV = 0x00FF+1 = 256 → LRCK = MCLK/256 = 16kHz ✓
```

对于 384x 倍率（chatgpt_demo 使用）：

```
MCLK  = 384 × 16000 = 6.144 MHz
BCLK  = 512 kHz → BCLK_DIV = 6.144/0.512 = 12
  reg 0x06 = 0x0B  → SCLK_DIV = 12
  reg 0x07/0x08 = 0x017F → LRCK_DIV = 384-1 → MCLK/384 = 16kHz
```

---

## 4. esp-box 示例索引

### 4.1 usb_headset（最简单的音频读写示例）

| 文件 | 功能 |
|------|------|
| `examples/usb_headset/main/main.c` | 初始化流程（I2C→Display→I2S→BSP→UAC） |
| `examples/usb_headset/main/src/usb_headset.c` | `bsp_i2s_read()` / `bsp_i2s_write()` 用法 |
| `examples/usb_headset/sdkconfig.ci.box-lite` | BOX-Lite 配置：`CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite=y` |

**BOX-Lite 初始化顺序**：
```c
bsp_i2c_init();                          // 1. I2C（for ES7243E）
display_lcd_init();                      // 2. LCD
i2s_std_config_t cfg = BSP_I2S_DUPLEX_MONO_CFG(16000);
cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
bsp_audio_init(&cfg);                    // 3. I2S（MCLK 开始输出）
bsp_board_init();                        // 4. Codec 初始化（ES7243E I2C 配置）
bsp_codec_set_fs(16000, 16, 2);          // 5. 设置采样格式
bsp_codec_volume_set(99, NULL);          // 6. 设置音量
bsp_codec_mute_set(false);              // 7. 取消静音
```

### 4.2 chatgpt_demo（完整语音识别 + AI 对话示例）

| 文件 | 功能 |
|------|------|
| `examples/chatgpt_demo/main/main.c` | 完整初始化流程 |
| `examples/chatgpt_demo/main/app/app_sr.c` | AFE 语音采集任务，**最佳参考** |
| `examples/chatgpt_demo/main/app/app_audio.c` | 音频播放回调、音量控制 |
| `examples/chatgpt_demo/sdkconfig.ci.box-lite` | BOX-Lite 配置 |
| `examples/chatgpt_demo/sdkconfig.defaults` | 默认 sdkconfig（必须包含） |

### 4.3 BSP 核心文件

| 文件 | 功能 |
|------|------|
| `components/bsp/src/boards/esp32_bsp_board.c` | `bsp_codec_init()`, `bsp_i2s_read()`, `bsp_codec_set_fs()` |
| `components/bsp/include/bsp_board.h` | 所有音频 API 声明 |
| `components/bsp/Kconfig.projbuild` | 板型选择：`BSP_BOARD_ESP32_S3_BOX_Lite` |

---

## 5. 当前固件 (audio_driver_es7210.c) 问题分析

### 5.1 Bug #1：Soft Reset 在配置中间多次触发（根本原因）

**问题代码**（`es7243e_init_regs()`）：

```c
/* 解锁 / 软复位 */
es7243e_write_reg(0x01, 0x3A);
es7243e_write_reg(0x00, 0x80);  // ✅ 第1次 Soft Reset（正确，清空寄存器）
es7243e_write_reg(0xF9, 0x00);
// ... 配置时钟、接口、模拟寄存器 ...
es7243e_write_reg(0x21, 0x1A);

/* 最终上电序列 */
es7243e_write_reg(0x00, 0x80);  // ❌ 第2次 Soft Reset！ → 清空所有时钟配置
es7243e_write_reg(0x01, 0x3A);
es7243e_write_reg(0x16, 0x3F);
es7243e_write_reg(0x16, 0x00);

/* 配置 PGA 增益 */
es7243e_write_reg(0x20, 0x10);
es7243e_write_reg(0x21, 0x10);

es7243e_write_reg(0x00, 0x80);  // ❌ 第3次 Soft Reset！ → 再次清空所有配置
es7243e_write_reg(0x01, 0x3A);
es7243e_write_reg(0x16, 0x3F);
es7243e_write_reg(0x16, 0x00);
```

**影响**：ES7243E 经过多次 Soft Reset，时钟和接口寄存器被清空，ADC 以默认配置（未知采样率）运行，导致输出噪声或静音。

### 5.2 Bug #2：中间功率下降

```c
es7243e_write_reg(0x00, 0x1E);
es7243e_write_reg(0x01, 0x00);  // ❌ 关闭所有模拟模块！
// 然后继续配置时钟...
```

`reg 0x01 = 0x00` 将模拟电路全部关电，之后的配置写入可能无效。

### 5.3 Bug #3：I2S Legacy API 已废弃

```c
// ❌ 旧 API（ESP-IDF 5.x 中已废弃）
#include "driver/i2s.h"
i2s_config_t i2s_cfg = { ... };
i2s_driver_install(AUDIO_I2S_NUM, &i2s_cfg, 0, NULL);

// ✅ 新 API（esp-box BSP 使用）
#include "driver/i2s_std.h"
i2s_std_config_t i2s_cfg = { ... };
i2s_new_channel(&chan_cfg, NULL, &rx_channel);
i2s_channel_init_std_mode(rx_channel, &i2s_cfg);
i2s_channel_enable(rx_channel);
```

### 5.4 Bug #4：DMA 缓冲区大小单位错误

```c
// ❌ 当前配置
.dma_buf_len = AUDIO_DMA_BUF_LEN * 2,  // = 320 * 2 = 640 个采样点

// ✅ 应为
.dma_buf_len = AUDIO_DMA_BUF_LEN,       // = 320 个采样点（20ms @ 16kHz）
```

旧 API 的 `dma_buf_len` 单位是**采样点数**（不是字节），设为 640 导致 40ms 延迟而非 20ms。

### 5.5 Bug #5：MCLK 倍率不匹配

```c
// ❌ 当前代码（fixed_mclk = 0, use_apll = true）
// ESP-IDF 自动计算 MCLK = 256 × 16kHz = 4.096 MHz

// reg 0x06 = 0x03 → SCLK_DIV = 4 → BCLK = 4.096MHz/4 = 1.024MHz
// 但 I2S Master 生成的 BCLK = 16bit × 2ch × 16kHz = 512kHz
// 不匹配！
```

**正确值**：
- MCLK = 4.096 MHz → reg 0x06 = **0x07**（DIV=8 → BCLK=512kHz）
- 或者 MCLK = 6.144 MHz → reg 0x06 = **0x0B**（DIV=12 → BCLK=512kHz）

### 5.6 Bug #6：Channel Format 可能导致读取空数据

```c
// ❌ 仅读左声道（可能 ES7243E 在右声道输出数据）
.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,

// ✅ 与 esp-box 保持一致，读取双声道
.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
// 然后从双声道数据中提取 Left 声道
```

---

## 6. 修复方案

### 6.1 最简修复（保留 Legacy I2S API）

修复 `es7243e_init_regs()` 中的寄存器序列：

```c
static esp_err_t es7243e_init_regs(void)
{
    esp_err_t ret = ESP_OK;

    /* Step 1: 一次性 Soft Reset（后面不再 Reset）*/
    ret |= es7243e_write_reg(0x00, 0x80);  // Soft Reset
    vTaskDelay(pdMS_TO_TICKS(10));         // 等待复位完成

    /* Step 2: 配置时钟 (MCLK=4.096MHz, BCLK=512kHz, LRCK=16kHz) */
    ret |= es7243e_write_reg(0x01, 0x3A);  // Power up chip
    ret |= es7243e_write_reg(0x02, 0x00);  // PLLCTL1
    ret |= es7243e_write_reg(0x03, 0x20);  // PLLCTL2
    ret |= es7243e_write_reg(0x04, 0x01);  // PLLCTL3
    ret |= es7243e_write_reg(0x0D, 0x00);  // MCLKSEL
    ret |= es7243e_write_reg(0x05, 0x00);  // CLKDIV1
    ret |= es7243e_write_reg(0x06, 0x07);  // SCLK_DIV=8 → BCLK=4.096MHz/8=512kHz ✓
    ret |= es7243e_write_reg(0x07, 0x00);  // LRCK_DIV high byte
    ret |= es7243e_write_reg(0x08, 0xFF);  // LRCK_DIV low byte → 256 → 16kHz ✓

    /* Step 3: 配置接口 (I2S Philips, 16-bit) */
    ret |= es7243e_write_reg(0x09, 0xCA);
    ret |= es7243e_write_reg(0x0A, 0x85);  // I2S standard, 16-bit
    ret |= es7243e_write_reg(0x0B, 0x00);
    ret |= es7243e_write_reg(0x0E, 0xBF);
    ret |= es7243e_write_reg(0x0F, 0x80);
    ret |= es7243e_write_reg(0x14, 0x0C);
    ret |= es7243e_write_reg(0x15, 0x0C);

    /* Step 4: 模拟配置 */
    ret |= es7243e_write_reg(0x17, 0x02);
    ret |= es7243e_write_reg(0x18, 0x26);
    ret |= es7243e_write_reg(0x19, 0x77);
    ret |= es7243e_write_reg(0x1A, 0xF4);
    ret |= es7243e_write_reg(0x1B, 0x66);
    ret |= es7243e_write_reg(0x1C, 0x44);
    ret |= es7243e_write_reg(0x1E, 0x00);
    ret |= es7243e_write_reg(0x1F, 0x0C);

    /* Step 5: MIC PGA 增益 */
    ret |= es7243e_write_reg(0x20, 0x10);  // MIC1 PGA: 24dB
    ret |= es7243e_write_reg(0x21, 0x10);  // MIC2 PGA: 24dB

    /* Step 6: 使能 ADC（不再 Soft Reset！）*/
    ret |= es7243e_write_reg(0x16, 0x3F);  // Power down ADC（短暂）
    ret |= es7243e_write_reg(0x16, 0x00);  // Power up ADC

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7243E register init failed");
    } else {
        ESP_LOGI(TAG, "ES7243E initialized: 16kHz, 16-bit, MCLK=4.096MHz");
    }
    return ret;
}
```

同时修复 I2S 配置：

```c
i2s_config_t i2s_cfg = {
    .mode = I2S_MODE_MASTER | I2S_MODE_RX,
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // ✅ 双声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = AUDIO_DMA_BUF_COUNT,
    .dma_buf_len = AUDIO_DMA_BUF_LEN,              // ✅ 320 采样点（不是 *2）
    .use_apll = true,
    .fixed_mclk = 0,
};
```

读取时需适配双声道：

```c
static int es7210_read(int16_t *buf, size_t samples)
{
    // 双声道缓冲区（每个 frame = L+R 两个采样点）
    int16_t stereo_buf[samples * 2];
    size_t bytes_read;

    esp_err_t ret = i2s_read(
        AUDIO_I2S_NUM,
        stereo_buf,
        samples * 2 * sizeof(int16_t),  // 双声道
        &bytes_read,
        pdMS_TO_TICKS(100));

    if (ret != ESP_OK) return -1;

    // 提取左声道（ES7243E MIC1 在左声道）
    int frames = (int)(bytes_read / sizeof(int16_t)) / 2;
    for (int i = 0; i < frames; i++) {
        buf[i] = stereo_buf[i * 2];  // 取 Left channel
    }
    return frames;
}
```

### 6.2 推荐方案：迁移到 esp-box BSP

参考 `examples/usb_headset/main/main.c`，使用 BSP 的 `bsp_i2s_read()` 接口，避免手动管理 ES7243E 寄存器。

BSP 中 `components/bsp/idf_component.yml` 已声明对 `espressif/esp-box-lite:2.0.*` 的依赖，直接使用即可获得完整的 Codec 初始化支持。

---

## 7. sdkconfig 必备配置（BOX-Lite）

```ini
# 目标芯片
CONFIG_IDF_TARGET="esp32s3"

# 板型选择（关键！）
CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite=y

# SPIRAM
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# CPU 频率
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y

# Data cache（重要，影响 I2S DMA 稳定性）
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y

# APLL（精确 MCLK）
# use_apll = true 时自动启用，无需额外配置
```

---

## 8. 快速调试清单

- [ ] I2C Scan 能看到 `0x10`（ES7243E）？
- [ ] `i2s_start()` 在 ES7243E I2C init **之前**已经执行？（MCLK 需要先运行）
- [ ] `dma_buf_len` 单位是**采样点**（非字节）？
- [ ] ES7243E 寄存器序列中没有第 2、3 次 Soft Reset（`reg 0x00 = 0x80`）？
- [ ] I2S 读取的是**双声道**数据？
- [ ] BCLK 分频 (`reg 0x06`) 与 I2S Master 实际输出的 BCLK 匹配？
