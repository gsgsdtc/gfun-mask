/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.3
 * @purpose 音频流水线实现：采集 → 编码 → 发送
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "audio_driver.h"
#include "opus_encoder.h"
#include "audio_pipeline.h"
#include "ble_l2cap.h"
#include "ble_l2cap.h"

#define TAG "AUDIO_PIPELINE"

/* ── 配置参数 ─────────────────────────────────────────────── */

#define PCM_FRAME_SIZE      320     /* 20ms @ 16kHz */
#define QUEUE_DEPTH         10
#define TASK_STACK_I2S      8192    /* pcm_frame_t=640B + I2S DMA 调用开销 */
#define TASK_STACK_ENCODER  8192
#define TASK_PRIORITY_I2S   5
#define TASK_PRIORITY_ENC   4

/* ── PCM 帧消息 ─────────────────────────────────────────────── */

typedef struct {
    int16_t samples[PCM_FRAME_SIZE];
} pcm_frame_t;

/* ── 内部状态 ─────────────────────────────────────────────── */

static QueueHandle_t s_pcm_queue = NULL;
static TaskHandle_t s_i2s_task = NULL;
static TaskHandle_t s_encoder_task = NULL;
static volatile audio_state_t s_state = AUDIO_STATE_IDLE;

/* ── I2S 读取任务 ─────────────────────────────────────────── */

static void i2s_task_func(void *arg)
{
    const audio_driver_ops_t *driver = audio_driver_get();
    pcm_frame_t frame;
    int samples_read;

    ESP_LOGI(TAG, "I2S task started");

    while (s_state == AUDIO_STATE_RECORDING) {
        samples_read = driver->read(frame.samples, PCM_FRAME_SIZE);

        if (samples_read == PCM_FRAME_SIZE) {
            if (xQueueSend(s_pcm_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "PCM queue full, dropping frame");
            }
        } else if (samples_read < 0) {
            ESP_LOGE(TAG, "I2S read error");
            s_state = AUDIO_STATE_ERROR;
            break;
        }
    }

    ESP_LOGI(TAG, "I2S task stopped");
    vTaskDelete(NULL);
}

/* ── 编码发送任务 ─────────────────────────────────────────── */

static void encoder_task_func(void *arg)
{
    pcm_frame_t pcm_frame;
    uint8_t opus_buf[1024];
    int opus_len;

    ESP_LOGI(TAG, "Encoder task started");

    while (s_state == AUDIO_STATE_RECORDING) {
        if (xQueueReceive(s_pcm_queue, &pcm_frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        opus_len = opus_encoder_encode(pcm_frame.samples, opus_buf, sizeof(opus_buf));

        if (opus_len > 0) {
            /* 若信道被流控阻塞，等待直到 TX 解除或连接断开 */
            while (!ble_l2cap_is_tx_ready() && s_state == AUDIO_STATE_RECORDING) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            if (s_state != AUDIO_STATE_RECORDING) {
                break;
            }

            ble_l2cap_send_frame(FRAME_TYPE_AUDIO, opus_buf, opus_len);
        }
    }

    ESP_LOGI(TAG, "Encoder task stopped");
    vTaskDelete(NULL);
}

/* ── 公开接口 ─────────────────────────────────────────────── */

int audio_pipeline_init(void)
{
    if (s_state != AUDIO_STATE_IDLE) {
        ESP_LOGW(TAG, "Pipeline already initialized");
        return 0;
    }

    /* 检查驱动是否注册 */
    if (!audio_driver_is_registered()) {
        ESP_LOGE(TAG, "No audio driver registered");
        return -1;
    }

    /* 初始化驱动 */
    const audio_driver_ops_t *driver = audio_driver_get();
    if (driver->init() != 0) {
        ESP_LOGE(TAG, "Driver init failed");
        return -1;
    }

    /* 初始化编码器 */
    if (opus_encoder_init() != 0) {
        ESP_LOGE(TAG, "Opus encoder init failed");
        driver->deinit();
        return -1;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized");
    return 0;
}

int audio_pipeline_start(void)
{
    ESP_LOGI(TAG, "audio_pipeline_start: state=%d", s_state);

    /* ERROR 状态允许重新启动（恢复录音） */
    if (s_state == AUDIO_STATE_ERROR) {
        ESP_LOGW(TAG, "Pipeline in error state, resetting to idle");
        s_state = AUDIO_STATE_IDLE;
    }

    if (s_state != AUDIO_STATE_IDLE) {
        ESP_LOGW(TAG, "Pipeline not idle, current state: %d", s_state);
        return -1;
    }

    const audio_driver_ops_t *driver = audio_driver_get();

    /* 重置帧计数 */
    opus_encoder_reset_count();

    /* 启动驱动 */
    ESP_LOGI(TAG, "Starting audio driver...");
    if (driver->start() != 0) {
        ESP_LOGE(TAG, "Driver start failed");
        return -1;
    }
    ESP_LOGI(TAG, "Audio driver started");

    /* 创建队列 */
    s_pcm_queue = xQueueCreate(QUEUE_DEPTH, sizeof(pcm_frame_t));
    if (s_pcm_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create PCM queue");
        driver->stop();
        return -1;
    }
    ESP_LOGI(TAG, "PCM queue created");

    s_state = AUDIO_STATE_RECORDING;

    /* 创建任务 */
    BaseType_t ret;
    ret = xTaskCreate(i2s_task_func, "i2s_task", TASK_STACK_I2S, NULL, TASK_PRIORITY_I2S, &s_i2s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2S task");
        s_state = AUDIO_STATE_IDLE;
        vQueueDelete(s_pcm_queue);
        s_pcm_queue = NULL;
        driver->stop();
        return -1;
    }
    ESP_LOGI(TAG, "I2S task created");

    ret = xTaskCreate(encoder_task_func, "encoder_task", TASK_STACK_ENCODER, NULL, TASK_PRIORITY_ENC, &s_encoder_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create encoder task");
        s_state = AUDIO_STATE_IDLE;
        vTaskDelete(s_i2s_task);
        s_i2s_task = NULL;
        vQueueDelete(s_pcm_queue);
        s_pcm_queue = NULL;
        driver->stop();
        return -1;
    }
    ESP_LOGI(TAG, "Encoder task created");

    ESP_LOGI(TAG, "Audio pipeline started");
    return 0;
}

int audio_pipeline_stop(void)
{
    if (s_state != AUDIO_STATE_RECORDING) {
        ESP_LOGW(TAG, "Pipeline not recording");
        return -1;
    }

    /* 设置停止标志 */
    s_state = AUDIO_STATE_IDLE;

    /* 等待任务结束 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 清理队列 */
    if (s_pcm_queue) {
        vQueueDelete(s_pcm_queue);
        s_pcm_queue = NULL;
    }

    /* 停止驱动 */
    const audio_driver_ops_t *driver = audio_driver_get();
    driver->stop();

    /* 发送录音结束帧 */
    uint32_t total_frames = opus_encoder_get_frame_count();
    ble_l2cap_send_frame(FRAME_TYPE_RECORD_END, (uint8_t*)&total_frames, sizeof(total_frames));

    ESP_LOGI(TAG, "Audio pipeline stopped, sent %lu frames", (unsigned long)total_frames);
    return 0;
}

audio_state_t audio_pipeline_get_state(void)
{
    return s_state;
}

uint32_t audio_pipeline_get_frame_count(void)
{
    return opus_encoder_get_frame_count();
}

void audio_pipeline_deinit(void)
{
    if (s_state == AUDIO_STATE_RECORDING) {
        audio_pipeline_stop();
    }

    opus_encoder_deinit();

    const audio_driver_ops_t *driver = audio_driver_get();
    driver->deinit();

    ESP_LOGI(TAG, "Audio pipeline deinitialized");
}