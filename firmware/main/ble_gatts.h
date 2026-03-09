#pragma once

/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §2.2
 * @purpose GATT Server（NimBLE）：暴露 PSM Service + PSM Characteristic（只读），
 *          iOS 读取后得到 L2CAP PSM 值（128 = 0x80），再调用 openL2CAPChannel。
 */

#define BLE_L2CAP_PSM  128u   /* LE L2CAP 动态 PSM，范围 0x80–0xFF */

/**
 * 向 NimBLE 注册 PSM GATT Service。
 * 必须在 nimble_port_freertos_init() 之前调用。
 */
void vm_gatts_init(void);
