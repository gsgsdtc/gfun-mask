/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md §2.2 §5
 * @purpose 验证 PSM=128 的 2 字节 Little-Endian 编解码逻辑。
 * @context PSM Characteristic 向 iOS 返回 2 字节 LE 整数。
 *          若此测试失败，iOS 将解析出错误的 PSM，导致 L2CAP 连接建立失败。
 *
 * 运行方式（ESP-IDF host-side 单测）：
 *   idf.py -C test build && ./build/test_psm_encode.elf
 */

#include "unity.h"
#include <stdint.h>
#include <string.h>

/* ── 被测逻辑（与 ble_gatts.c 中相同的编码方式）────────── */
static void psm_encode(uint16_t psm, uint8_t out[2])
{
    out[0] = (uint8_t)(psm & 0xFF);         /* Low byte  */
    out[1] = (uint8_t)((psm >> 8) & 0xFF);  /* High byte */
}

static uint16_t psm_decode(const uint8_t in[2])
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

/* ── 测试用例 ────────────────────────────────────────────── */

/*
 * @purpose 验证 PSM=128 编码为正确的 2 字节 LE 序列
 * @context PSM=128(0x0080)，Low byte=0x80，High byte=0x00
 */
TEST_CASE("psm_encode: PSM=128 produces [0x80, 0x00]", "[psm]")
{
    uint8_t buf[2];
    psm_encode(128, buf);
    TEST_ASSERT_EQUAL_HEX8(0x80, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);
}

/*
 * @purpose 验证 2 字节 LE 序列解码回 PSM=128
 * @context iOS 端解析方式相同；若解码错误，openL2CAPChannel 将使用错误 PSM
 */
TEST_CASE("psm_decode: [0x80, 0x00] decodes to 128", "[psm]")
{
    const uint8_t buf[2] = { 0x80, 0x00 };
    TEST_ASSERT_EQUAL_UINT16(128, psm_decode(buf));
}

/*
 * @purpose 验证编解码往返一致（round-trip）
 * @context 确保编码与解码互为逆操作，PSM 值不因传输而失真
 */
TEST_CASE("psm encode/decode round-trip", "[psm]")
{
    const uint16_t original = 128;
    uint8_t buf[2];
    psm_encode(original, buf);
    TEST_ASSERT_EQUAL_UINT16(original, psm_decode(buf));
}

/*
 * @purpose 验证 PSM 边界值 0xFF（255）编解码正确
 * @context 动态 PSM 最大值为 0xFF；覆盖边界防止位运算溢出
 */
TEST_CASE("psm_encode: PSM=255 boundary", "[psm]")
{
    uint8_t buf[2];
    psm_encode(255, buf);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(255, psm_decode(buf));
}
