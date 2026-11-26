# Zigbee Ethernet bridge: Waveshare ESP32-P4-POE-ETH hosting a USB Zigbee coordinator (Sonoff
# Dongle Plus MG24) and serving its serial port to Zigbee2MQTT over TCP, with a web dashboard.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Defaults only apply when sdkconfig is (re)created. sdkconfig.secrets (git-ignored), if present, is added last.
# There is no default password: the user creates it on the dashboard at first boot.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
# Test build (watchdog test hooks): idf.py -B build-test -D BRIDGE_TEST=1 -D SDKCONFIG=sdkconfig.test build
if(BRIDGE_TEST)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.test.defaults")
endif()
# Extra defaults files (OTA tests: test/ota/sdkconfig.*), separated by ';'
if(BRIDGE_EXTRA_DEFAULTS)
    list(APPEND SDKCONFIG_DEFAULTS ${BRIDGE_EXTRA_DEFAULTS})
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.secrets")
endif()

# Re-run CMake when the version changes, so the image's app description carries it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
if(BRIDGE_VER)  # test images: -D BRIDGE_VER=9.9.1
    set(PROJECT_VER "${BRIDGE_VER}")
endif()
if(BRIDGE_TEST)
    string(APPEND PROJECT_VER "+test")
endif()
project(zigbee_eth_bridge)
