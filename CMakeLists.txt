cmake_minimum_required(VERSION 3.14)
project(p_net_bridge C)

# provenance baked into the binary (printed at startup)
execute_process(
  COMMAND git describe --tags --always --dirty
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE PNET_BRIDGE_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/p-net
  OUTPUT_VARIABLE PNET_BRIDGE_PNET_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT PNET_BRIDGE_VERSION)
  set(PNET_BRIDGE_VERSION "unknown")
endif()
if(NOT PNET_BRIDGE_PNET_SHA)
  set(PNET_BRIDGE_PNET_SHA "unknown")
endif()

# the p-net stack (GPLv3), pinned as a git submodule
set(PNET_OPTION_DRIVER_ENABLE OFF CACHE BOOL "" FORCE)
add_subdirectory(p-net)

add_executable(p_net_bridge pnet_bridge.c relay_core.c)
target_link_libraries(p_net_bridge PRIVATE profinet)
target_compile_definitions(p_net_bridge PRIVATE
  PNET_BRIDGE_VERSION="${PNET_BRIDGE_VERSION}"
  PNET_BRIDGE_PNET_SHA="${PNET_BRIDGE_PNET_SHA}"
  PNET_BRIDGE_SOURCE_URL="https://github.com/autometricstech/p_net_bridge")
