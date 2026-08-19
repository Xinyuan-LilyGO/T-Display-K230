cmake_minimum_required(VERSION 3.10)
project(k230_phone_ui C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RADIOLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/RadioLib/src)
set(NOFRENDO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nofrendo/src)
set(LIBTMT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libtmt)
set(QRCODEGEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
set(QUIRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quirc)
set(FACE_DETECT_SRC_DIR "")
set(FACE_DETECT_SRC_CANDIDATES
    ${CMAKE_CURRENT_SOURCE_DIR}/../face_detect/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../face_detect/src
)
foreach(face_detect_candidate ${FACE_DETECT_SRC_CANDIDATES})
    if(EXISTS ${face_detect_candidate}/face_detection.cc)
        set(FACE_DETECT_SRC_DIR ${face_detect_candidate})
        break()
    endif()
endforeach()
set(K230_PHONE_UI_FACE_DETECT_ENABLED 0)
file(GLOB NOFRENDO_SOURCES
    ${NOFRENDO_DIR}/*.c
    ${NOFRENDO_DIR}/cpu/*.c
    ${NOFRENDO_DIR}/libsnss/*.c
    ${NOFRENDO_DIR}/mappers/*.c
    ${NOFRENDO_DIR}/nes/*.c
    ${NOFRENDO_DIR}/sndhrdw/*.c
)
set(RADIOLIB_REQUIRED_SOURCES
    ${RADIOLIB_DIR}/Hal.cpp
    ${RADIOLIB_DIR}/Module.cpp
    ${RADIOLIB_DIR}/modules/SX126x/SX1262.cpp
    ${RADIOLIB_DIR}/modules/SX126x/SX126x.cpp
    ${RADIOLIB_DIR}/modules/SX126x/SX126x_commands.cpp
    ${RADIOLIB_DIR}/modules/SX126x/SX126x_config.cpp
    ${RADIOLIB_DIR}/modules/SX126x/SX126x_LR_FHSS.cpp
    ${RADIOLIB_DIR}/modules/LR11x0/LR_common.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_config.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_chip_control.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_flrc.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_gfsk.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_lora.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_misc.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_ook.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_oqpsk.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_radio.cpp
    ${RADIOLIB_DIR}/modules/LR2021/LR2021_cmds_ranging.cpp
    ${RADIOLIB_DIR}/protocols/PhysicalLayer/PhysicalLayer.cpp
    ${RADIOLIB_DIR}/protocols/LoRaWAN/LoRaWAN.cpp
    ${RADIOLIB_DIR}/protocols/LoRaWAN/LoRaWANBands.cpp
    ${RADIOLIB_DIR}/protocols/LoRaWAN/LoRaWANPackageTS003.cpp
    ${RADIOLIB_DIR}/protocols/LoRaWAN/LoRaWANPackageTS009.cpp
    ${RADIOLIB_DIR}/protocols/LoRaWAN/LoRaWANPacMan.cpp
    ${RADIOLIB_DIR}/utils/BCH.cpp
    ${RADIOLIB_DIR}/utils/CRC.cpp
    ${RADIOLIB_DIR}/utils/ConvCode.cpp
    ${RADIOLIB_DIR}/utils/Cryptography.cpp
    ${RADIOLIB_DIR}/utils/Utils.cpp
)

set(K230_PHONE_UI_SOURCES
    main.c
    ui_common.c
    ui_prefs.c
    ui_i18n.c
    ui_input.c
    ui_nrf9151_manager.c
    ui_multitouch.c
    ui_hardware.c
    ui_display_test.c
    ui_hdmi_test.c
    ui_i2c_scan.c
    ui_i2s_test.c
    ui_settings.c
    ui_language.c
    ui_time_settings.c
    ui_terminal.c
    ui_wifi.c
    ui_wifi_iperf.c
    ui_ethernet.c
    ui_halow.c
    ui_ble.c
    ui_nrf52840_dfu.c
    ui_cellular.c
    ui_usb_modem.c
    ui_ai_demo.c
    ui_rtsp.c
    ui_video_player.c
    ui_audio.c
    ui_mic_spectrum.c
    ui_usb_storage.c
    ui_meshtastic.c
    ui_lora.cpp
    ui_nes.cpp
    ${LIBTMT_DIR}/tmt.c
    ${QRCODEGEN_DIR}/qrcodegen.c
    ${NOFRENDO_SOURCES}
    ${RADIOLIB_REQUIRED_SOURCES}
)

if(FACE_DETECT_SRC_DIR AND EXISTS ${FACE_DETECT_SRC_DIR}/face_detection.cc)
    set(K230_PHONE_UI_FACE_DETECT_ENABLED 1)
    list(APPEND K230_PHONE_UI_SOURCES
        camera_face_detect.cpp
        ${FACE_DETECT_SRC_DIR}/ai_base.cc
        ${FACE_DETECT_SRC_DIR}/anchors_320.cc
        ${FACE_DETECT_SRC_DIR}/anchors_640.cc
        ${FACE_DETECT_SRC_DIR}/face_detection.cc
        ${FACE_DETECT_SRC_DIR}/utils.cc
    )
endif()

add_executable(k230_phone_ui ${K230_PHONE_UI_SOURCES})
add_executable(k230_phone_ui_fullswitch_test ${K230_PHONE_UI_SOURCES})
add_executable(k230_camera_capture camera_capture.c)
add_executable(k230_lora_flrc_bench lora_flrc_bench.cpp ${RADIOLIB_REQUIRED_SOURCES})
add_executable(k230_lora_flrc_video lora_flrc_bench.cpp ${RADIOLIB_REQUIRED_SOURCES})
add_executable(k230_lora_flrc_tile_stream lora_flrc_bench.cpp ${RADIOLIB_REQUIRED_SOURCES})
add_executable(k230_lorawan_test lorawan_test.cpp ${RADIOLIB_REQUIRED_SOURCES})
add_executable(k230_halow_udp_stream halow_udp_stream.c)
add_executable(k230_meshtastic_probe
    k230_meshtastic_probe.cpp
    ui_nrf9151_manager.c
    ${RADIOLIB_REQUIRED_SOURCES}
)
add_executable(k230_qr_scan
    qr_scan.c
    ${QUIRC_DIR}/quirc.c
    ${QUIRC_DIR}/identify.c
    ${QUIRC_DIR}/decode.c
    ${QUIRC_DIR}/version_db.c
)
add_executable(k230_pcm_volume pcm_volume.c)
add_executable(k230_nrf52840_dfu k230_nrf52840_dfu.cpp)
add_executable(drm_motion_probe drm_motion_probe.c)
add_executable(drm_motion_probe_rgb565 drm_motion_probe.c)

foreach(ui_target k230_phone_ui k230_phone_ui_fullswitch_test)
target_include_directories(${ui_target} PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${CMAKE_SYSROOT}/usr/include/libevdev-1.0
    ${RADIOLIB_DIR}
    ${NOFRENDO_DIR}
    ${LIBTMT_DIR}
    ${QRCODEGEN_DIR}
    ${CMAKE_SYSROOT}/usr/include/opencv4
)
if(K230_PHONE_UI_FACE_DETECT_ENABLED)
    target_include_directories(${ui_target} PRIVATE
        ${FACE_DETECT_SRC_DIR}
        ${CMAKE_SYSROOT}/usr/include/ai_demo_common
    )
endif()
endforeach()

target_include_directories(drm_motion_probe PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/libdrm
)

target_include_directories(drm_motion_probe_rgb565 PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/libdrm
)

target_include_directories(k230_camera_capture PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/libdrm
)

target_include_directories(k230_lora_flrc_bench PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${RADIOLIB_DIR}
)

target_include_directories(k230_lora_flrc_video PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${RADIOLIB_DIR}
)

target_include_directories(k230_lora_flrc_tile_stream PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${RADIOLIB_DIR}
)

target_include_directories(k230_lorawan_test PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${RADIOLIB_DIR}
)

target_include_directories(k230_meshtastic_probe PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/codec2
    ${RADIOLIB_DIR}
)

target_include_directories(k230_qr_scan PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${QUIRC_DIR}
)

target_compile_definitions(k230_meshtastic_probe PRIVATE
    RADIOLIB_GODMODE=1
)

target_compile_definitions(k230_phone_ui PRIVATE
    LV_CONF_PATH="${CMAKE_SYSROOT}/usr/include/lvgl/lv_conf.h"
    K230_R58_COMPACT_SWITCH_TEST=0
    K230_R59_PERSISTENT_PANEL_UI=0
    K230_CAMERA_FACE_KPU=${K230_PHONE_UI_FACE_DETECT_ENABLED}
)

target_compile_definitions(k230_phone_ui_fullswitch_test PRIVATE
    LV_CONF_PATH="${CMAKE_SYSROOT}/usr/include/lvgl/lv_conf.h"
    K230_R58_COMPACT_SWITCH_TEST=0
    K230_R59_PERSISTENT_PANEL_UI=0
    K230_CAMERA_FACE_KPU=${K230_PHONE_UI_FACE_DETECT_ENABLED}
)

foreach(ui_target k230_phone_ui k230_phone_ui_fullswitch_test)
target_link_directories(${ui_target} PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_libraries(${ui_target} PRIVATE
    lvgl_linux
    lvgl
    lvgl_linux
    drm
    evdev
    v4l2-drm
    display
    gpiod
    avformat
    avcodec
    avutil
    swscale
    jpeg
    m
    pthread
)
if(K230_PHONE_UI_FACE_DETECT_ENABLED)
    target_link_libraries(${ui_target} PRIVATE
        -Wl,--start-group
        Nncase.Runtime.Native
        nncase.rt_modules.k230
        functional_k230
        opencv_imgcodecs
        opencv_imgproc
        opencv_core
        sharpyuv
        jpeg
        webp
        png
        z
        mmz
        -Wl,--end-group
    )
endif()
endforeach()

target_link_directories(drm_motion_probe PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_directories(drm_motion_probe_rgb565 PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_compile_definitions(drm_motion_probe_rgb565 PRIVATE
    K230_DRM_PROBE_RGB565=1
)

target_link_libraries(drm_motion_probe PRIVATE
    drm
)

target_link_libraries(drm_motion_probe_rgb565 PRIVATE
    drm
)

target_link_directories(k230_camera_capture PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_libraries(k230_camera_capture PRIVATE
    v4l2-drm
    display
    jpeg
)

target_link_libraries(k230_lora_flrc_bench PRIVATE
    gpiod
    m
    pthread
)

target_link_libraries(k230_lora_flrc_video PRIVATE
    gpiod
    m
    pthread
)

target_compile_definitions(k230_lora_flrc_tile_stream PRIVATE
    K230_FLRC_TILE_STREAM=1
)

target_link_libraries(k230_lora_flrc_tile_stream PRIVATE
    gpiod
    m
    pthread
)

target_link_libraries(k230_lorawan_test PRIVATE
    gpiod
    m
    pthread
)

target_include_directories(k230_halow_udp_stream PRIVATE
    ${CMAKE_SYSROOT}/usr/include
)

target_link_directories(k230_halow_udp_stream PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_libraries(k230_halow_udp_stream PRIVATE
    jpeg
)

target_link_directories(k230_meshtastic_probe PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_libraries(k230_meshtastic_probe PRIVATE
    gpiod
    crypto
    jpeg
    opus
    codec2
    m
    pthread
)

target_link_directories(k230_qr_scan PRIVATE
    ${CMAKE_SYSROOT}/usr/lib
)

target_link_libraries(k230_qr_scan PRIVATE
    v4l2-drm
    display
    drm
    m
)

install(TARGETS k230_phone_ui k230_phone_ui_fullswitch_test k230_camera_capture k230_lora_flrc_bench k230_lora_flrc_video k230_lora_flrc_tile_stream k230_lorawan_test k230_halow_udp_stream k230_meshtastic_probe k230_qr_scan k230_pcm_volume k230_nrf52840_dfu drm_motion_probe drm_motion_probe_rgb565 RUNTIME DESTINATION /root/app/k230_phone_ui)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/k230_flrc_camera_stream.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/k230_halow_camera_stream.sh
        DESTINATION /root/app/k230_phone_ui)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fonts DESTINATION /root/app/k230_phone_ui)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION /root/app/k230_phone_ui)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models/face_detection_320.kmodel)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/models/face_detection_320.kmodel
            DESTINATION /root/app/face_detect)
endif()
