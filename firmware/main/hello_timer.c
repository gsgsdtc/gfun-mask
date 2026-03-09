/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §3.1 §3.2
 * @purpose 3 秒周期定时器：L2CAP 信道建立后由 ble_l2cap.c 调用 hello_timer_start()，
 *          断连时调用 hello_timer_stop()。
 *          每次触发调用 ble_l2cap_send_hello() 发送 "hello world"。
 */

#include "esp_log.h"
#include "esp_timer.h"
#include "ble_l2cap.h"
#include "hello_timer.h"

#define TAG "HELLO_TIMER"

static esp_timer_handle_t s_timer = NULL;
static bool s_running = false;

static void timer_callback(void *arg)
{
    ble_l2cap_send_hello();
}

void hello_timer_start(void)
{
    if (s_running) {
        return;  /* 幂等：已在运行则忽略 */
    }

    if (s_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = timer_callback,
            .name     = "hello_timer",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    }

    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer,
                    HELLO_INTERVAL_MS * 1000ULL));  /* 转换为微秒 */
    s_running = true;
    ESP_LOGI(TAG, "Timer started, interval=%dms", HELLO_INTERVAL_MS);
}

void hello_timer_stop(void)
{
    if (!s_running || s_timer == NULL) {
        return;  /* 幂等：未运行则忽略 */
    }

    esp_timer_stop(s_timer);
    s_running = false;
    ESP_LOGI(TAG, "Timer stopped");
}
