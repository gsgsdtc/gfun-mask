/*
 * @doc     docs/modules/audio-capture/design/07-wake-word-voice-activation-backend-design.md §4.1
 * @purpose BOOT 键中断处理：录音中按下则停止录音，恢复唤醒词检测
 *
 * 设计要点：
 *   - ISR 仅发送任务通知，不在中断上下文中调用 pipeline 接口
 *   - 按键防抖：仅在 AUDIO_STATE_RECORDING 时生效
 *   - 按键响应时延目标 ≤ 200ms
 */

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp32_s3_box_lite.h"
#include "audio_pipeline.h"
#include "wake_detector.h"
#include "button_handler.h"

#define TAG              "BUTTON"
#define BTN_TASK_STACK   2048
#define BTN_TASK_PRIO    10   /* 高于普通任务，保证响应时延 */

/* ── 私有状态 ─────────────────────────────────────────────── */

static TaskHandle_t s_btn_task = NULL;

/* ── GPIO ISR：仅通知任务，不做业务逻辑 ──────────────────── */

/*
 * @doc     §4.3 业务规则
 * @purpose 按键中断服务程序，通过任务通知触发按键处理任务
 * @context ISR 上下文限制，不可调用非 ISR 安全函数；
 *          所有实际逻辑在 _btn_task 中执行
 */
static void IRAM_ATTR _gpio_isr_handler(void *arg)
{
    BaseType_t higher_prio_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_btn_task, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ── 按键处理任务 ─────────────────────────────────────────── */

/*
 * @doc     §4.1 按键停止录音流程
 * @purpose 等待 ISR 通知，仅在 RECORDING 状态下执行停止录音
 * @context 与 wake_detector_result_cb 的 audio_pipeline_start() 互斥；
 *          state 检查保证两者不会同时触发
 */
static void _btn_task(void *arg)
{
    while (1) {
        /* 阻塞等待 ISR 通知（无超时） */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* 简单软件防抖：等待 50ms 确认按键稳定 */
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BUTTON_STOP_GPIO) != 0) {
            /* 抖动，忽略 */
            continue;
        }

        /* 仅在录音状态下响应 */
        if (audio_pipeline_get_state() != AUDIO_STATE_RECORDING) {
            ESP_LOGD(TAG, "Button pressed but not recording, ignored");
            continue;
        }

        ESP_LOGI(TAG, "Stop button pressed, stopping recording");

        /* 停止 GMF Pipeline（内部发送 FRAME_TYPE_RECORD_END） */
        audio_pipeline_stop();

        /* 恢复唤醒词检测 */
        wake_detector_resume();
    }
}

/* ── 公开接口 ─────────────────────────────────────────────── */

esp_err_t button_handler_init(void)
{
    /* ── 1. 配置 GPIO ── */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_STOP_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* BOOT 键低电平有效 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,    /* 下降沿触发 */
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %d", ret);
        return ret;
    }

    /* ── 2. 创建按键处理任务（ISR 通知目标）── */
    if (xTaskCreate(_btn_task, "btn_task", BTN_TASK_STACK, NULL,
                    BTN_TASK_PRIO, &s_btn_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    /* ── 3. 安装 GPIO ISR 服务并注册中断 ── */
    gpio_install_isr_service(0);
    ret = gpio_isr_handler_add(BUTTON_STOP_GPIO, _gpio_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %d", ret);
        vTaskDelete(s_btn_task);
        s_btn_task = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Button handler initialized (GPIO %d)", BUTTON_STOP_GPIO);
    return ESP_OK;
}

void button_handler_deinit(void)
{
    gpio_isr_handler_remove(BUTTON_STOP_GPIO);

    if (s_btn_task) {
        vTaskDelete(s_btn_task);
        s_btn_task = NULL;
    }
}
