cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT)

# NOTE: espp__rtsp is vendored under components/ (digest auth + TCP interleaved +
# max_sessions patches). Project components/ shadow managed_components/, so the
# patches/ copy-at-configure mechanism was removed — builds are deterministic
# locally and in CI. Do NOT re-add espp/rtsp to idf_component.yml.
