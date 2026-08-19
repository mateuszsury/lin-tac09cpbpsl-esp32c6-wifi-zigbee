cmake_minimum_required(VERSION 3.16)

# Build only the application component and the dependencies it explicitly
# selects for the requested transport. Without this, ESP-IDF discovers the
# local Zigbee component even in a MQTT-only image.
if(NOT DEFINED KLIMA_HA_TRANSPORT)
    set(KLIMA_HA_TRANSPORT "MQTT")
endif()
if(NOT DEFINED KLIMA_BRIDGE_MODE)
    # Safe by default.  MITM_NTS is an explicit, separately built artifact
    # and must never be selected implicitly on a device without HIL evidence.
    set(KLIMA_BRIDGE_MODE "SNIFFER")
endif()
string(TOUPPER "${KLIMA_HA_TRANSPORT}" KLIMA_HA_TRANSPORT_NORMALIZED)
string(TOUPPER "${KLIMA_BRIDGE_MODE}" KLIMA_BRIDGE_MODE_NORMALIZED)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(KLIMA_BRIDGE_MODE_NORMALIZED STREQUAL "MITM_NTS")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.mitm_nts.defaults")
endif()
if(KLIMA_HA_TRANSPORT_NORMALIZED STREQUAL "ZIGBEE")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/optional_components/zigbee_service")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.zigbee.defaults")
endif()
set(COMPONENTS main)
if(KLIMA_HA_TRANSPORT_NORMALIZED STREQUAL "ZIGBEE")
    list(APPEND COMPONENTS zigbee_service)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(klima_wifi)
message(STATUS "Klima canonical bridge profile: ${KLIMA_BRIDGE_MODE_NORMALIZED}")
