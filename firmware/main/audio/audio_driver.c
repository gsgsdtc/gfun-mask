/*
 * @doc     docs/modules/audio-capture/design/01-esp32-audio-capture-design.md §4.1
 * @purpose 音频驱动抽象层实现
 */

#include "audio_driver.h"
#include <stdbool.h>

/* ── 当前注册的驱动 ─────────────────────────────────────────── */

static const audio_driver_ops_t *s_current_driver = NULL;

/* ── 接口实现 ─────────────────────────────────────────────── */

void audio_driver_register(const audio_driver_ops_t *ops)
{
    s_current_driver = ops;
}

const audio_driver_ops_t* audio_driver_get(void)
{
    return s_current_driver;
}

bool audio_driver_is_registered(void)
{
    return s_current_driver != NULL;
}