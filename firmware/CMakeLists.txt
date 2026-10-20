# Perfect Vision ECU - CAN/BLE Bridge Firmware
# ESP-IDF Project
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(perfect_vision_ecu)
