#pragma once

/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §2.1
 * @purpose BLE GAP 模块：广播管理、连接/断连事件处理（NimBLE）。
 */

/**
 * NimBLE on_sync 回调中调用：推断地址类型后启动广播。
 * 由 main.c 的 on_sync() 调用，不要在其他地方调用。
 */
void ble_gap_on_sync(void);

/** 重新开始广播（断连后由 GAP 层自动调用，也可外部调用）。 */
void ble_gap_start_advertising(void);
