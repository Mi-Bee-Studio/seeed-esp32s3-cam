cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT)

# Apply local patches to managed components (managed_components/ is gitignored)
set(RTSP_PATCH_DIR ${CMAKE_SOURCE_DIR}/patches/espp__rtsp)
set(RTSP_MANAGED_DIR ${CMAKE_SOURCE_DIR}/managed_components/espp__rtsp)
if(EXISTS ${RTSP_MANAGED_DIR}/src/rtsp_session.cpp AND EXISTS ${RTSP_PATCH_DIR}/src/rtsp_session.cpp)
    file(COPY ${RTSP_PATCH_DIR}/src/rtsp_session.cpp DESTINATION ${RTSP_MANAGED_DIR}/src)
    file(COPY ${RTSP_PATCH_DIR}/include/rtsp_session.hpp DESTINATION ${RTSP_MANAGED_DIR}/include)
    message(STATUS "Applied RTSP component patches (TCP interleaved + digest auth fix)")
endif()
