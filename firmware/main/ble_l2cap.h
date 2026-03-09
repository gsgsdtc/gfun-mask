#pragma once

/*
 * @doc     docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
 * @purpose L2CAP CoC 服务端（NimBLE）：监听 PSM=128，管理信道生命周期，
 *          支持帧协议传输（音频帧、控制指令）。
 */

#include <stdint.h>
#include <stdbool.h>

/* ── 帧类型定义 ───────────────────────────────────────────── */

typedef enum {
    FRAME_TYPE_HEARTBEAT         = 0x00,  /* 心跳保活（Phase 3） */
    FRAME_TYPE_AUDIO             = 0x01,  /* Opus 音频帧 */
    FRAME_TYPE_VAD_PREWARM       = 0xFF,  /* VAD 预警（Phase 3） */
    FRAME_TYPE_END_OF_UTTERANCE  = 0xFE,  /* 说话结束标记（Phase 3） */
    FRAME_TYPE_CMD_START_RECORD  = 0x10,  /* 开始录音指令（iOS → ESP32） */
    FRAME_TYPE_CMD_STOP_RECORD   = 0x11,  /* 停止录音指令（iOS → ESP32） */
    FRAME_TYPE_RECORD_END        = 0x12,  /* 录音结束确认（ESP32 → iOS） */
} frame_type_t;

/* ── 初始化与连接管理 ─────────────────────────────────────── */

/**
 * 初始化 L2CAP CoC 内存池（mbuf pool）。
 * 必须在 nimble_port_freertos_init() 之前调用。
 */
void vm_l2cap_init(void);

/**
 * BLE 连接建立时由 GAP 层调用，注册 L2CAP CoC 服务端监听 PSM=128。
 * @param conn_handle  新建立的 BLE 连接句柄
 */
void ble_l2cap_on_ble_connect(uint16_t conn_handle);

/**
 * BLE 连接断开时由 GAP 层调用，清理信道状态。
 */
void ble_l2cap_on_ble_disconnect(void);

/* ── 数据发送 ─────────────────────────────────────────────── */

/**
 * 通过已建立的 L2CAP 信道发送带帧头的数据。
 * @param type     帧类型
 * @param payload  数据负载（可为 NULL）
 * @param len      负载长度
 * @return 0 成功, -1 失败
 */
int ble_l2cap_send_frame(frame_type_t type, const uint8_t *payload, uint16_t len);

/**
 * 兼容 Phase 1：发送 "hello world" 字符串（无帧头）。
 * 若信道未建立则静默忽略。
 */
void ble_l2cap_send_hello(void);

/* ── 控制指令回调 ─────────────────────────────────────────── */

/**
 * 控制指令回调函数类型。
 * @param frame_type  指令类型（FRAME_TYPE_CMD_*）
 */
typedef void (*ble_l2cap_cmd_callback_t)(frame_type_t frame_type);

/**
 * 注册控制指令回调。
 * @param callback  回调函数
 */
void ble_l2cap_set_cmd_callback(ble_l2cap_cmd_callback_t callback);

/**
 * 查询 L2CAP 发送通道是否就绪（未被流控阻塞）。
 * 用于编码任务在 stall 期间主动等待，避免无效重试。
 * @return true 可发送, false 信道 stall 或未连接
 */
bool ble_l2cap_is_tx_ready(void);
