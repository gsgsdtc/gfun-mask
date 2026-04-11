# Main component for VAD demo

idf_component_register(
    SRCS
        "vad_test.c"
        "gmf_vad_el.c"
        "gmf_pcm_enc_el.c"
        "gmf_mic_io.c"
        "audio_driver.c"
        "audio_driver_es7210.c"
        "opus_encoder.c"
        "ble_l2cap_stub.c"
    INCLUDE_DIRS
        "."
        "../../main/audio"
        "../../main"
        "../../managed_components/espressif__esp-sr/src/include"
    REQUIRES
        esp_gmf_core
        esp_gmf_ai_audio
        esp-sr
        driver
        esp_timer
        freertos
        nimble
        esp-nimble-cpp
        nvs_flash
)

# Add opus encoder library
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/libopus.a
    COMMAND echo "Using prebuilt opus library"
    VERBATIM
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../main/opus/lib/libopus.a
)
