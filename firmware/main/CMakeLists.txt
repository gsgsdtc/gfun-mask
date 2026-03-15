idf_component_register(
    SRCS
        "main.c"
        "ble_gap.c"
        "ble_gatts.c"
        "ble_l2cap.c"
        "hello_timer.c"
        "audio/audio_driver.c"
        "audio/audio_driver_es7210.c"
        "audio/opus_encoder.c"
        "audio/audio_pipeline.c"
        "audio/gmf_mic_io.c"
        "audio/gmf_pcm_enc_el.c"
    INCLUDE_DIRS "." "audio" "boards"
    REQUIRES nvs_flash bt driver espressif__gmf_core espressif__gmf_io
)
