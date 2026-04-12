# Main component for VAD demo

idf_component_register(
    SRCS
        # 基础测试（单轮）: "vad_test.c"
        # 多轮测试（唤醒词 + 多轮对话）:
        "vad_test_multi.c"
        "gmf_vad_el.c"
        "gmf_pcm_enc_el.c"
        "gmf_mic_io.c"
        "audio_driver.c"
        "audio_driver_es7210.c"
        "ble_l2cap_stub.c"
    INCLUDE_DIRS
        "."
        "../../../main/audio"
        "../../../main"
        "../../../main/boards"
        "../../../managed_components/espressif__esp-sr/src/include"
    REQUIRES
        espressif__gmf_core
        espressif__gmf_io
        espressif__gmf_ai_audio
        espressif__esp-sr
        driver
        esp_timer
        freertos
        nvs_flash
)
