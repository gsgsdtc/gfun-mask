#pragma once

/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §3.1 §3.2
 * @purpose 3 秒周期定时器：L2CAP 信道建立后启动，断连后停止，
 *          每次触发调用 ble_l2cap_send_hello()。
 */

#define HELLO_INTERVAL_MS  3000   /* 发送间隔，单位毫秒 */

/** 启动 3 秒周期定时器（幂等，已启动则忽略）。 */
void hello_timer_start(void);

/** 停止定时器（幂等，未启动则忽略）。 */
void hello_timer_stop(void);
