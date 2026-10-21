# XRDAP-Core 的电脑端测试。xrdap_core_unit 只编译不依赖硬件和 libxr 的纯逻辑，判据来自
# docs/protocol.md、CMSIS-DAP 规范和用户的决定。给出 -DLIBXR_DIR=<libxr> 时另外把全部
# Core 源文件按 libxr 的 Linux 系统层编译一遍（只检查能否编译）。
#
# Host tests of XRDAP-Core. xrdap_core_unit builds only the pure logic that depends on
# neither hardware nor libxr; its oracles come from docs/protocol.md, the CMSIS-DAP
# specification and the user's decisions. With -DLIBXR_DIR=<libxr> every Core source is
# also compiled against libxr's Linux system layer (compile check only).
cmake_minimum_required(VERSION 3.16)
project(xrdap_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(XRDAP_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
include("${XRDAP_CORE_ROOT}/xrdap_core.cmake")

file(GLOB XRDAP_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/unit/*.cpp")
add_executable(xrdap_core_unit ${XRDAP_UNIT_TESTS} ${XRDAP_CORE_PURE_SOURCES})
target_include_directories(xrdap_core_unit PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                   ${XRDAP_CORE_INCLUDE_DIRS})
target_compile_options(xrdap_core_unit PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME xrdap_core_unit COMMAND xrdap_core_unit)

if(DEFINED LIBXR_DIR)
  set(LIBXR_SYSTEM linux)
  add_subdirectory("${LIBXR_DIR}" libxr EXCLUDE_FROM_ALL)
  add_library(xrdap_core_compile OBJECT ${XRDAP_CORE_SOURCES}
                                        "${CMAKE_CURRENT_LIST_DIR}/compile/all_headers.cpp")
  target_include_directories(xrdap_core_compile PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                        ${XRDAP_CORE_INCLUDE_DIRS})
  target_link_libraries(xrdap_core_compile PRIVATE xr)
  target_compile_options(xrdap_core_compile PRIVATE -Wall -Wextra)
endif()
