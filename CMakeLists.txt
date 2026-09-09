cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
file(GLOB MIBEE_UI_FILES main/web_ui/*)
# 2026-09-03：目录级依赖不随文件内容编辑触发（build/spiffs.bin 比新 app.js 旧），
# 必须显式 DEPENDS 文件列表，否则固件烧的是旧 UI
# 2026-09-09 PIT-043：压缩进构建。compress_ui.py 原为手工步骤——改 SPA 源文件后忘跑，
# 旧 .gz（gitignored、family_check 不可见）随镜像进固件，设备静默服务旧 UI
# （实例：备用 WiFi 字段不显示）。gz 与源文件同 GLOB 入 DEPENDS，Ninja 保序。
file(GLOB MIBEE_UI_SRC main/web_ui/*.js main/web_ui/*.html main/web_ui/*.css main/web_ui/*.svg)
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/main/web_ui/app.js.gz
           ${CMAKE_SOURCE_DIR}/main/web_ui/index.html.gz
           ${CMAKE_SOURCE_DIR}/main/web_ui/style.css.gz
           ${CMAKE_SOURCE_DIR}/main/web_ui/i18n.js.gz
           ${CMAKE_SOURCE_DIR}/main/web_ui/favicon.svg.gz
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/compress_ui.py
    DEPENDS ${MIBEE_UI_SRC} ${CMAKE_SOURCE_DIR}/tools/compress_ui.py
    COMMENT "gzip web UI (auto, PIT-043)"
    VERBATIM)
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT DEPENDS ${MIBEE_UI_FILES})

# NOTE: espp__rtsp is vendored under components/ (digest auth + TCP interleaved +
# max_sessions patches). Project components/ shadow managed_components/, so the
# patches/ copy-at-configure mechanism was removed — builds are deterministic
# locally and in CI. Do NOT re-add espp/rtsp to idf_component.yml.
