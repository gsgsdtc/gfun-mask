/*
 * BLE L2CAP Stub for VAD Demo
 * 简化版，不需要实际 BLE 连接
 */

#pragma once

#include <stdint.h>

void ble_l2cap_stub_init(void);
bool ble_l2cap_is_tx_ready(void);
int  ble_l2cap_send_frame(uint8_t type, const uint8_t *data, uint16_t len);
