cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
file(GLOB MIBEE_UI_FILES main/web_ui/*)
# 2026-09-03：目录级依赖不随文件内容编辑触发（build/spiffs.bin 比新 app.js 旧），
# 必须显式 DEPENDS 文件列表，否则固件烧的是旧 UI
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT DEPENDS ${MIBEE_UI_FILES})

# NOTE: espp__rtsp is vendored under components/ (digest auth + TCP interleaved +
# max_sessions patches). Project components/ shadow managed_components/, so the
# patches/ copy-at-configure mechanism was removed — builds are deterministic
# locally and in CI. Do NOT re-add espp/rtsp to idf_component.yml.
