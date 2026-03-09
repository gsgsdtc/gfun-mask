/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §6.1
 * @purpose ESP32-S3-BOX-Lite 开发板引脚配置
 *          引脚来源：https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-lite/include/bsp/esp-box-lite.h
 */

#pragma once

/* ── I2S 配置（ES7243 ADC）────────────────────────────────── */

#define AUDIO_I2S_NUM           I2S_NUM_0
#define AUDIO_I2S_SCK_PIN       GPIO_NUM_17   /* BCLK  (BSP_I2S_SCLK) */
#define AUDIO_I2S_WS_PIN        GPIO_NUM_47   /* LRCK  (BSP_I2S_LCLK) */
#define AUDIO_I2S_DATA_PIN      GPIO_NUM_16   /* DIN   (BSP_I2S_DSIN, From ADC ES7243) */
#define AUDIO_I2S_MCLK_PIN      GPIO_NUM_2    /* MCLK  (BSP_I2S_MCLK) */

/* ── ES7243 I2C 配置 ───────────────────────────────────────── */
/* 注意：板载麦克风芯片是 ES7243（非 ES7210） */
/* ES7243 I2C 地址：0x10（7-bit） */

#define ES7210_I2C_ADDR         0x10          /* ES7243E 实际地址（I2C scan 确认）*/
#define ES7210_I2C_SDA_PIN      GPIO_NUM_8    /* BSP_I2C_SDA */
#define ES7210_I2C_SCL_PIN      GPIO_NUM_18   /* BSP_I2C_SCL */
#define ES7210_I2C_CLK_SPEED    100000        /* 100kHz */

/* ── 音频参数 ─────────────────────────────────────────────── */

#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1

/* I2S DMA 缓冲配置 */
#define AUDIO_DMA_BUF_COUNT     8
#define AUDIO_DMA_BUF_LEN       320    /* 20ms @ 16kHz = 320 samples */