/*
 * @purpose PCM 直传模式（Opus 集成前的验证阶段）
 *
 * 把原始 PCM int16 直接作为 payload 发送，frame_type=0x01 不变。
 * iOS 端收到后可以直接播放验证麦克风是否有效。
 * 确认有声音后再切换到真正的 Opus 编码。
 */

#include <string.h>
#include "esp_log.h"
#include "opus_encoder.h"

#define TAG "OPUS_ENCODER"

static uint32_t s_frame_count = 0;

/* 每 100 帧打印一次信号能量，方便串口确认麦克风是否工作 */
static void log_energy(const int16_t *pcm, int samples)
{
    static int log_counter = 0;
    if (++log_counter % 100 != 0) return;

    int32_t energy = 0;
    for (int i = 0; i < samples; i++) {
        energy += (pcm[i] < 0) ? -pcm[i] : pcm[i];
    }
    ESP_LOGI(TAG, "Frame %lu energy=%ld (avg=%ld/sample)",
             (unsigned long)s_frame_count,
             (long)energy,
             (long)(energy / samples));
}

int opus_encoder_init(void)
{
    s_frame_count = 0;
    ESP_LOGI(TAG, "PCM passthrough mode (Opus pending)");
    return 0;
}

int opus_encoder_encode(const int16_t *pcm_in, uint8_t *opus_out, size_t max_out)
{
    if (pcm_in == NULL || opus_out == NULL) return -1;

    log_energy(pcm_in, OPUS_FRAME_SIZE);

    /* 直接把 PCM int16 字节序列复制到输出（little-endian，与平台一致）*/
    size_t pcm_bytes = OPUS_FRAME_SIZE * sizeof(int16_t);  /* = 640 字节 */
    if (max_out < pcm_bytes) return -1;

    memcpy(opus_out, pcm_in, pcm_bytes);
    s_frame_count++;
    return (int)pcm_bytes;
}

void opus_encoder_deinit(void)
{
    ESP_LOGI(TAG, "PCM encoder deinit, total frames=%lu", (unsigned long)s_frame_count);
}

uint32_t opus_encoder_get_frame_count(void) { return s_frame_count; }
void opus_encoder_reset_count(void)          { s_frame_count = 0; }
