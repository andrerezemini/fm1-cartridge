cmake_minimum_required(VERSION 3.16)

# The only USB controller is reserved for the FM-1 connection.
set(IDF_TARGET esp32s3 CACHE STRING "Target for ESP32-S3 cartridge")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fm1_cartridge)
