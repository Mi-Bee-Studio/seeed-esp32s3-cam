cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(parrot_cam)

# Embed web UI files into SPIFFS partition
spiffs_create_partition_image(storage main/web_ui FLASH_IN_PROJECT)

