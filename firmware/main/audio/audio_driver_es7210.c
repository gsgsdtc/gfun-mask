/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.1
 * @purpose ES7243E 麦克风驱动实现（ESP32-S3-BOX-Lite）
 *
 * 关键说明：
 *  - 板载芯片是 ES7243E（非 ES7210），I2C 地址 0x10
 *  - ES7243E 需要 MCLK 信号稳定后才能响应 I2C 指令
 *    参考：esp-adf es7243.c es7243_mclk_active()
 *  - Soft Reset（reg 0x00 = 0x80）会清空所有寄存器，只能在最开始执行一次
 *  - I2S 采双声道（RIGHT_LEFT），读完后提取 Left Channel 作为单声道输出
 */

#include "esp_log.h"
#include "driver/i2s.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "audio_driver.h"
#include "boards/esp32_s3_box_lite.h"

#define TAG "AUDIO_DRIVER"

/* ── I2C 写入寄存器 ───────────────────────────────────────── */

static esp_err_t es7243e_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(
        I2C_NUM_0,
        ES7210_I2C_ADDR,   /* 板载地址 0x10 */
        buf,
        2,
        pdMS_TO_TICKS(100)
    );
}

/* ── ES7243E 初始化序列 ───────────────────────────────────── */
/*
 * 严格对齐 esp-adf 官方序列（esp_codec_dev/device/es7243e/es7243e.c）：
 *   - es7243e_open() + es7243e_adc_enable(true) 合并
 *   - 共 3 次 Soft Reset，最终状态为 Slave 模式 + ADC 使能
 *   - PGA 增益改为官方值 0x1A（+30dB）
 *   - reg 0x06=0x03（官方值），ES7243E 工作于 I2S Slave 模式，
 *     BCLK/LRCK 来自 ESP32 I2S Master，内部分频配置以官方为准
 */
static esp_err_t es7243e_init_regs(void)
{
    esp_err_t ret = ESP_OK;

    /* === 对应 es7243e_open() === */

    /* Step 1: 上电，然后第一次 Soft Reset（清空所有寄存器） */
    ret |= es7243e_write_reg(0x01, 0x3A);
    ret |= es7243e_write_reg(0x00, 0x80);  /* First Soft Reset */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7243E soft reset failed");
        return ret;
    }

    /* Step 2: 解锁序列 */
    ret |= es7243e_write_reg(0xF9, 0x00);
    ret |= es7243e_write_reg(0x04, 0x02);
    ret |= es7243e_write_reg(0x04, 0x01);
    ret |= es7243e_write_reg(0xF9, 0x01);
    ret |= es7243e_write_reg(0x00, 0x1E);
    ret |= es7243e_write_reg(0x01, 0x00);  /* 模拟模块下电，配置时钟 */

    /* Step 3: 时钟配置（官方值：SCLK=MCLK/4=1.024MHz，LRCK=MCLK/256=16kHz）
     * ES7243E 工作在 I2S Slave 模式下，BCLK/LRCK 由 ESP32 I2S Master 提供，
     * 此处配置会被后续 Soft Reset 清除，但保留官方原始序列 */
    ret |= es7243e_write_reg(0x02, 0x00);
    ret |= es7243e_write_reg(0x03, 0x20);
    ret |= es7243e_write_reg(0x04, 0x01);
    ret |= es7243e_write_reg(0x0D, 0x00);
    ret |= es7243e_write_reg(0x05, 0x00);
    ret |= es7243e_write_reg(0x06, 0x03);  /* 官方值：SCLK=MCLK/4 */
    ret |= es7243e_write_reg(0x07, 0x00);
    ret |= es7243e_write_reg(0x08, 0xFF);

    /* Step 4: 接口配置（I2S Philips 标准，16-bit）*/
    ret |= es7243e_write_reg(0x09, 0xCA);
    ret |= es7243e_write_reg(0x0A, 0x85);
    ret |= es7243e_write_reg(0x0B, 0x00);
    ret |= es7243e_write_reg(0x0E, 0xBF);
    ret |= es7243e_write_reg(0x0F, 0x80);
    ret |= es7243e_write_reg(0x14, 0x0C);
    ret |= es7243e_write_reg(0x15, 0x0C);

    /* Step 5: 模拟配置 */
    ret |= es7243e_write_reg(0x17, 0x02);
    ret |= es7243e_write_reg(0x18, 0x26);
    ret |= es7243e_write_reg(0x19, 0x77);
    ret |= es7243e_write_reg(0x1A, 0xF4);
    ret |= es7243e_write_reg(0x1B, 0x66);
    ret |= es7243e_write_reg(0x1C, 0x44);
    ret |= es7243e_write_reg(0x1E, 0x00);
    ret |= es7243e_write_reg(0x1F, 0x0C);

    /* Step 6: MIC PGA 增益（此处设置会被 Step 7 的 Soft Reset 清空，仅保留官方原始序列）*/
    ret |= es7243e_write_reg(0x20, 0x1A);  /* MIC1 PGA: +30dB（会被后续 Soft Reset 清空）*/
    ret |= es7243e_write_reg(0x21, 0x1A);  /* MIC2 PGA: +30dB（会被后续 Soft Reset 清空）*/

    /* Step 7: 第二次 Soft Reset → Slave 模式，然后 ADC 使能 */
    ret |= es7243e_write_reg(0x00, 0x80);  /* Second Soft Reset (Slave Mode) */
    ret |= es7243e_write_reg(0x01, 0x3A);
    ret |= es7243e_write_reg(0x16, 0x3F);
    ret |= es7243e_write_reg(0x16, 0x00);

    /* === 对应 es7243e_adc_enable(true) === */

    /* Step 8: 第三次 Soft Reset → 最终启动 ADC */
    ret |= es7243e_write_reg(0xF9, 0x00);
    ret |= es7243e_write_reg(0x04, 0x01);
    ret |= es7243e_write_reg(0x17, 0x01);
    /* 注意：不在 Soft Reset 前写 PGA，因为 Soft Reset 会清空所有寄存器 */
    ret |= es7243e_write_reg(0x00, 0x80);  /* Third Soft Reset */
    ret |= es7243e_write_reg(0x01, 0x3A);
    ret |= es7243e_write_reg(0x16, 0x3F);
    ret |= es7243e_write_reg(0x16, 0x00);
    /* PGA 增益必须在最后一次 Soft Reset 之后设置，否则会被重置 */
    ret |= es7243e_write_reg(0x20, 0x1A);  /* MIC1 PGA: +30dB */
    ret |= es7243e_write_reg(0x21, 0x1A);  /* MIC2 PGA: +30dB */

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7243E register init failed (err=%d)", ret);
        return ret;
    }

    ESP_LOGI(TAG, "ES7243E initialized: Slave mode, 3x Soft Reset sequence (official)");
    return ESP_OK;
}

/* ── 驱动接口实现 ─────────────────────────────────────────── */

static int es7210_init(void)
{
    esp_err_t ret;

    /* 1. 先安装 I2S 驱动，MCLK 立即在 GPIO2 上输出
     *    ES7243E 需要看到有效 MCLK 才能响应 I2C 配置
     *    参考：esp-adf es7243.c "it is necessary to output mclk
     *          to es7243 to activate the I2C configuration"
     *
     *    channel_format = RIGHT_LEFT：读双声道，在 read() 中提取 Left Channel
     *    dma_buf_len = AUDIO_DMA_BUF_LEN（单位：采样点数，不是字节）
     */
    i2s_config_t i2s_cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,   /* 双声道，与 esp-box BSP 一致 */
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,               /* 320 采样点（原 *2 有误）*/
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pin_cfg = {
        .mck_io_num = AUDIO_I2S_MCLK_PIN,
        .bck_io_num = AUDIO_I2S_SCK_PIN,
        .ws_io_num = AUDIO_I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = AUDIO_I2S_DATA_PIN,
    };
    ret = i2s_driver_install(AUDIO_I2S_NUM, &i2s_cfg, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed: %d", ret);
        return -1;
    }
    ret = i2s_set_pin(AUDIO_I2S_NUM, &pin_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pin failed: %d", ret);
        return -1;
    }
    ESP_LOGI(TAG, "I2S initialized: %dHz, 16-bit, stereo (MCLK on GPIO%d)",
             AUDIO_SAMPLE_RATE, AUDIO_I2S_MCLK_PIN);

    /* 2. 等待 MCLK 稳定，ES7243E 需要若干 MCLK 周期后才响应 I2C */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 3. 初始化 I2C（外部有上拉电阻，PULLUP_DISABLE）*/
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ES7210_I2C_SDA_PIN,
        .scl_io_num = ES7210_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = ES7210_I2C_CLK_SPEED,
    };
    ret = i2c_param_config(I2C_NUM_0, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %d", ret);
        return -1;
    }
    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %d", ret);
        return -1;
    }

    /* 4. I2C 总线扫描（确认 ES7243E 可见，预期：0x10）*/
    ESP_LOGI(TAG, "I2C scan (after MCLK stable):");
    bool found = false;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        uint8_t tmp = 0;
        esp_err_t scan_ret = i2c_master_write_to_device(
            I2C_NUM_0, addr, &tmp, 1, pdMS_TO_TICKS(10));
        if (scan_ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X%s",
                     addr, addr == ES7210_I2C_ADDR ? " ← ES7243E" : "");
            found = true;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "  No I2C devices found! Check MCLK and wiring.");
    }

    /* 5. 写入 ES7243E 寄存器 */
    ret = es7243e_init_regs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7243E I2C init failed (%d)", ret);
        return -1;
    }
    ESP_LOGI(TAG, "ES7243E I2C init OK");

    return 0;
}

static int es7210_start(void)
{
    esp_err_t ret = i2s_start(AUDIO_I2S_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S start failed: %d", ret);
        return -1;
    }
    ESP_LOGI(TAG, "Audio capture started");
    return 0;
}

static int es7210_stop(void)
{
    esp_err_t ret = i2s_stop(AUDIO_I2S_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S stop failed: %d", ret);
        return -1;
    }
    ESP_LOGI(TAG, "Audio capture stopped");
    return 0;
}

static int es7210_read(int16_t *buf, size_t samples)
{
    /* 双声道缓冲区：每个 frame = L + R 各一个 int16_t */
    int16_t stereo_buf[samples * 2];
    size_t bytes_read = 0;

    esp_err_t ret = i2s_read(
        AUDIO_I2S_NUM,
        stereo_buf,
        samples * 2 * sizeof(int16_t),
        &bytes_read,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        return -1;
    }

    int frames = (int)(bytes_read / sizeof(int16_t)) / 2;

    /* 计算 L/R 两个声道的能量，用于诊断哪个声道有信号 */
    int32_t energy_l = 0, energy_r = 0;
    static int diag_count = 0;
    for (int i = 0; i < frames; i++) {
        energy_l += abs(stereo_buf[i * 2]);
        energy_r += abs(stereo_buf[i * 2 + 1]);
    }

    /* 每 50 帧打印一次诊断（约 1 秒），方便确认哪个声道有音频 */
    if (++diag_count % 50 == 0) {
        ESP_LOGI(TAG, "I2S energy: L=%ld, R=%ld (frames=%d)",
                 (long)energy_l, (long)energy_r, frames);
    }

    /* 取能量更高的声道（自动适配 BOX-Lite 硬件接线）
     * 首次启动时通过日志确认后，可硬编码为固定声道以减少开销 */
    int ch = (energy_l >= energy_r) ? 0 : 1;
    for (int i = 0; i < frames; i++) {
        buf[i] = stereo_buf[i * 2 + ch];
    }
    return frames;
}

static void es7210_deinit(void)
{
    i2s_driver_uninstall(AUDIO_I2S_NUM);
    i2c_driver_delete(I2C_NUM_0);
    ESP_LOGI(TAG, "Audio driver deinitialized");
}

/* ── 驱动注册 ─────────────────────────────────────────────── */

static const audio_driver_ops_t es7210_driver = {
    .init = es7210_init,
    .start = es7210_start,
    .stop = es7210_stop,
    .read = es7210_read,
    .deinit = es7210_deinit,
};

void audio_driver_es7210_register(void)
{
    audio_driver_register(&es7210_driver);
    ESP_LOGI(TAG, "ES7243E driver registered (via es7210 interface)");
}
