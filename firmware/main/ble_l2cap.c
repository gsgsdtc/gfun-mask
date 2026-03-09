/*
 * @doc     docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
 * @purpose L2CAP CoC 服务端（NimBLE）：
 *   - BLE 连接后调用 ble_l2cap_create_server(PSM=128) 等待 iOS 发起 CoC 连接
 *   - 支持帧协议传输（音频帧、控制指令）
 *   - 断连后状态清零（广播重启由 GAP 层负责）
 *
 * 异常处理：
 *   - 发送失败（mbuf 不足/拥塞）：丢弃当次帧，不阻塞
 *   - 多客户端：仅接受第一个 CoC 连接，断连后再次开放
 */

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
#include "os/os_mbuf.h"
#include "ble_gatts.h"   /* BLE_L2CAP_PSM */
#include "ble_l2cap.h"

#define TAG          "BLE_L2CAP"
#define COC_MTU      1024   /* PCM 直传帧 = 640B，加帧头 643B，须大于此值 */
#define COC_BUF_COUNT 10   /* 接收缓冲区数量 */

/* ── mbuf 内存池（接收缓冲区） ──────────────────────────── */
static struct os_mempool   s_rx_mempool;
static struct os_mbuf_pool s_rx_mbuf_pool;
static os_membuf_t         s_rx_membuf[OS_MEMPOOL_SIZE(COC_BUF_COUNT, COC_MTU)];

/* ── 信道状态 ────────────────────────────────────────────── */
static struct ble_l2cap_chan *s_chan      = NULL;
static bool                   s_connected = false;
static volatile bool          s_tx_stalled = false;  /* 流控：TX 被对端 credit 阻塞 */

/* ── 控制指令回调 ─────────────────────────────────────────── */
static ble_l2cap_cmd_callback_t s_cmd_callback = NULL;

/* ── 内部辅助 ─────────────────────────────────────────────── */

/* 接受 CoC 连接（或收到数据后重新准备接收缓冲区） */
static int coc_accept(uint16_t conn_handle, uint16_t peer_mtu,
                      struct ble_l2cap_chan *chan)
{
    struct os_mbuf *sdu_rx = os_mbuf_get_pkthdr(&s_rx_mbuf_pool, 0);
    if (!sdu_rx) {
        ESP_LOGE(TAG, "No rx mbuf available");
        return BLE_HS_ENOMEM;
    }
    return ble_l2cap_recv_ready(chan, sdu_rx);
}

/* 解析并处理控制帧 */
static void handle_control_frame(const uint8_t *data, uint16_t len)
{
    if (len < 3) {
        ESP_LOGW(TAG, "Control frame too short: %d", len);
        return;
    }

    uint8_t frame_type = data[0];
    uint16_t payload_len = data[1] | (data[2] << 8);

    /* 验证长度 */
    if (len < 3 + payload_len) {
        ESP_LOGW(TAG, "Control frame incomplete: expected %d, got %d",
                 3 + payload_len, len);
        return;
    }

    /* 处理控制指令 */
    switch (frame_type) {
    case FRAME_TYPE_CMD_START_RECORD:
    case FRAME_TYPE_CMD_STOP_RECORD:
        ESP_LOGI(TAG, "Received command: 0x%02X", frame_type);
        if (s_cmd_callback) {
            s_cmd_callback((frame_type_t)frame_type);
        }
        break;

    default:
        ESP_LOGW(TAG, "Unknown control frame: 0x%02X", frame_type);
        break;
    }
}

/* L2CAP CoC 事件回调 */
static int coc_event_cb(struct ble_l2cap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_L2CAP_EVENT_COC_ACCEPT:
        /* iOS 发起连接请求，提供接收缓冲区表示接受 */
        return coc_accept(event->accept.conn_handle,
                          event->accept.peer_sdu_size,
                          event->accept.chan);

    case BLE_L2CAP_EVENT_COC_CONNECTED:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "L2CAP CoC connect error: %d",
                     event->connect.status);
            return 0;
        }
        s_chan      = event->connect.chan;
        s_connected = true;
        ESP_LOGI(TAG, "L2CAP CoC connected");
        break;

    case BLE_L2CAP_EVENT_COC_DISCONNECTED:
        ESP_LOGI(TAG, "L2CAP CoC disconnected");
        s_connected  = false;
        s_tx_stalled = false;
        s_chan        = NULL;
        break;

    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED:
        {
            struct os_mbuf *om = event->receive.sdu_rx;
            uint16_t len = OS_MBUF_PKTLEN(om);
            uint8_t *data = om->om_data;

            /* 检查是否为控制帧（帧类型 >= 0x10） */
            if (len >= 1 && data[0] >= 0x10) {
                /* 复制数据到临时缓冲区（避免 mbuf 操作复杂性） */
                uint8_t buf[64];
                uint16_t copy_len = len < sizeof(buf) ? len : sizeof(buf);
                os_mbuf_copydata(om, 0, copy_len, buf);
                handle_control_frame(buf, copy_len);
            } else {
                /* 普通数据帧（Phase 1 兼容：无帧头的 hello world） */
                ESP_LOGI(TAG, "Data received, len=%d", len);
            }

            os_mbuf_free_chain(om);
            /* 重新准备接收缓冲区 */
            coc_accept(event->receive.conn_handle, COC_MTU, event->receive.chan);
        }
        break;

    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
        /* 对端发来新的 credit，TX 解除阻塞 */
        s_tx_stalled = false;
        ESP_LOGD(TAG, "L2CAP TX unstalled");
        break;

    default:
        break;
    }
    return 0;
}

/* ── 公开接口 ─────────────────────────────────────────────── */

void vm_l2cap_init(void)
{
    int rc;

    rc = os_mempool_init(&s_rx_mempool, COC_BUF_COUNT, COC_MTU,
                         s_rx_membuf, "l2cap_rx_pool");
    assert(rc == 0);

    rc = os_mbuf_pool_init(&s_rx_mbuf_pool, &s_rx_mempool,
                           COC_MTU, COC_BUF_COUNT);
    assert(rc == 0);

    ESP_LOGI(TAG, "L2CAP CoC mbuf pool initialized (PSM=%d)", BLE_L2CAP_PSM);
}

void ble_l2cap_on_ble_connect(uint16_t conn_handle)
{
    /* BLE 连接建立后注册 L2CAP CoC 服务端，等待 iOS 的 openL2CAPChannel() */
    int rc = ble_l2cap_create_server(BLE_L2CAP_PSM, COC_MTU,
                                     coc_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_l2cap_create_server failed: %d", rc);
    }
}

void ble_l2cap_on_ble_disconnect(void)
{
    /* BLE 连接断开时清理 L2CAP 状态（CoC 会随之关闭） */
    s_connected = false;
    s_chan       = NULL;
}

void ble_l2cap_set_cmd_callback(ble_l2cap_cmd_callback_t callback)
{
    s_cmd_callback = callback;
}

int ble_l2cap_send_frame(frame_type_t type, const uint8_t *payload, uint16_t len)
{
    if (!s_connected || s_chan == NULL) {
        return -1;  /* 信道未建立 */
    }

    /* 计算总帧大小：帧头(3) + 负载 */
    uint16_t total_len = 3 + len;

    /* 从系统 mbuf 池分配发送缓冲区 */
    struct os_mbuf *om = os_msys_get_pkthdr(total_len, 0);
    if (!om) {
        ESP_LOGW(TAG, "No tx mbuf available");
        return -1;
    }

    /* 构建帧头 */
    uint8_t header[3] = { (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };

    int rc = os_mbuf_append(om, header, 3);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        ESP_LOGW(TAG, "mbuf append header failed (%d)", rc);
        return -1;
    }

    /* 追加负载 */
    if (payload != NULL && len > 0) {
        rc = os_mbuf_append(om, payload, len);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            ESP_LOGW(TAG, "mbuf append payload failed (%d)", rc);
            return -1;
        }
    }

    /* 发送帧 */
    rc = ble_l2cap_send(s_chan, om);
    if (rc == BLE_HS_ESTALLED) {
        /* 对端 credit 耗尽，标记 stall，等 TX_UNSTALLED 事件后恢复 */
        s_tx_stalled = true;
        ESP_LOGD(TAG, "L2CAP TX stalled (no credits), type=0x%02X", type);
        return -1;
    }
    if (rc != 0) {
        /* 其他发送失败（mbuf 不足、信道关闭等），不置 stall */
        ESP_LOGW(TAG, "Send frame failed (%d), type=0x%02X", rc, type);
        return -1;
    }

    ESP_LOGD(TAG, "Sent frame: type=0x%02X, len=%d", type, len);
    return 0;
}

void ble_l2cap_send_hello(void) {
    if (!s_connected || s_chan == NULL) {
        return;  /* 信道未建立，静默忽略 */
    }

    const char *msg = "hello world";
    uint16_t    len = (uint16_t)strlen(msg);

    /* 从系统 mbuf 池分配发送缓冲区 */
    struct os_mbuf *om = os_msys_get_pkthdr(len, 0);
    if (!om) {
        ESP_LOGW(TAG, "No tx mbuf available, will retry next cycle");
        return;
    }

    int rc = os_mbuf_append(om, msg, len);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        ESP_LOGW(TAG, "mbuf append failed (%d), will retry next cycle", rc);
        return;
    }

    /* ble_l2cap_send 成功后 om 所有权移交给协议栈，不需要 free */
    rc = ble_l2cap_send(s_chan, om);
    if (rc != 0) {
        /* 发送失败时协议栈已释放 om，无需再 free */
        ESP_LOGW(TAG, "Send failed (%d), will retry next cycle", rc);
    } else {
        ESP_LOGI(TAG, "Sent: hello world");
    }
}

bool ble_l2cap_is_tx_ready(void)
{
    return s_connected && s_chan != NULL && !s_tx_stalled;
}
