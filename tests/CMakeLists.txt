# XRDAP-Core 的电脑端测试，必须给出 -DLIBXR_DIR=<libxr>：Core 的命令号取自 libxr 的
# CMSIS-DAP 定义。xrdap_core_unit 只编译不依赖硬件的纯逻辑，判据来自 docs/protocol.md、
# CMSIS-DAP 规范和用户的决定；另外把全部 Core 源文件按 libxr 的 Linux 系统层编译一遍。
#
# Host tests of XRDAP-Core; -DLIBXR_DIR=<libxr> is required because Core takes its command
# ids from libxr's CMSIS-DAP definitions. xrdap_core_unit builds only the pure logic that
# needs no hardware; its oracles come from docs/protocol.md, the CMSIS-DAP specification and
# the user's decisions. Every Core source is also compiled against libxr's Linux system
# layer.
cmake_minimum_required(VERSION 3.16)
project(xrdap_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(XRDAP_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
include("${XRDAP_CORE_ROOT}/xrdap_core.cmake")
if(NOT DEFINED LIBXR_DIR)
  message(FATAL_ERROR "pass -DLIBXR_DIR=<libxr>")
endif()

enable_testing()
add_test(NAME xrdap_core_unit COMMAND xrdap_core_unit)
# 原子变量只用 32 位整数（见 word_atomics.cmake）/ Atomics are 32-bit integers only (see
# word_atomics.cmake).
add_test(NAME xrdap_core_word_atomics
         COMMAND ${CMAKE_COMMAND} -DROOT=${XRDAP_CORE_ROOT} -P
                 "${CMAKE_CURRENT_LIST_DIR}/word_atomics.cmake")

if(TRUE)
  set(LIBXR_SYSTEM linux)
  set(LIBXR_DRIVER linux)
  add_subdirectory("${LIBXR_DIR}" libxr EXCLUDE_FROM_ALL)
  add_library(xrdap_core_compile OBJECT ${XRDAP_CORE_SOURCES}
                                        "${CMAKE_CURRENT_LIST_DIR}/compile/all_headers.cpp")
  target_include_directories(xrdap_core_compile PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                        ${XRDAP_CORE_INCLUDE_DIRS})
  target_link_libraries(xrdap_core_compile PRIVATE xr)

  file(GLOB XRDAP_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/unit/*.cpp")
  add_executable(xrdap_core_unit ${XRDAP_UNIT_TESTS} ${XRDAP_CORE_PURE_SOURCES})
  target_include_directories(xrdap_core_unit PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                     ${XRDAP_CORE_INCLUDE_DIRS})
  target_link_libraries(xrdap_core_unit PRIVATE xr)
  target_compile_options(xrdap_core_unit PRIVATE -Wall -Wextra -Werror -Wno-c++20-compat -g
                                                -fsanitize=address,undefined
                                                -fno-sanitize-recover=all)
  target_link_options(xrdap_core_unit PRIVATE -fsanitize=address,undefined)
  target_compile_options(xrdap_core_compile PRIVATE -Wall -Wextra)
  # libxr 的 Linux 系统层不带 USB 设备驱动，这里只为取得 DAP 类的声明加上它们的目录 /
  # libxr's Linux system layer leaves out the USB device drivers; their directories are
  # added only to get the DAP class declaration.
  file(GLOB_RECURSE XRDAP_LIBXR_USB_HEADERS "${LIBXR_DIR}/src/driver/usb/*.hpp"
       "${LIBXR_DIR}/src/driver/debug/*.hpp")
  foreach(header ${XRDAP_LIBXR_USB_HEADERS})
    get_filename_component(dir "${header}" DIRECTORY)
    list(APPEND XRDAP_LIBXR_USB_DIRS "${dir}")
  endforeach()
  list(REMOVE_DUPLICATES XRDAP_LIBXR_USB_DIRS)
  target_include_directories(xrdap_core_compile PRIVATE ${XRDAP_LIBXR_USB_DIRS})
  target_include_directories(xrdap_core_unit PRIVATE ${XRDAP_LIBXR_USB_DIRS})

  # 多线程下的执行器，用 libxr 的 Linux 系统层运行 / The executor with threads, run on
  # libxr's Linux system layer.
  add_executable(xrdap_core_threads "${CMAKE_CURRENT_LIST_DIR}/threads/executor_wait_test.cpp"
                                    ${XRDAP_CORE_SOURCES})
  target_include_directories(xrdap_core_threads PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                        ${XRDAP_CORE_INCLUDE_DIRS}
                                                        ${XRDAP_LIBXR_USB_DIRS})
  target_link_libraries(xrdap_core_threads PRIVATE xr pthread)
  target_compile_options(xrdap_core_threads PRIVATE -Wall -Wextra)
  add_test(NAME xrdap_core_threads COMMAND xrdap_core_threads)

  # MEM-AP 写路径：假 SWD 模拟 posted 写和迟到的故障（V305-2）/ MEM-AP write path against a
  # fake SWD that models a posted write and a late fault (V305-2).
  add_executable(xrdap_mem_ap_test
                 "${CMAKE_CURRENT_LIST_DIR}/mem_ap/mem_ap_posted_write_test.cpp"
                 "${XRDAP_CORE_ROOT}/xrdap/vendor/mem_ap.cpp")
  target_include_directories(xrdap_mem_ap_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                       ${XRDAP_CORE_INCLUDE_DIRS}
                                                       ${XRDAP_LIBXR_USB_DIRS})
  target_link_libraries(xrdap_mem_ap_test PRIVATE xr pthread)
  target_compile_options(xrdap_mem_ap_test PRIVATE -Wall -Wextra)
  add_test(NAME xrdap_mem_ap_test COMMAND xrdap_mem_ap_test)
  add_executable(xrdap_sampler_recovery
      "${CMAKE_CURRENT_LIST_DIR}/threads/sampler_recovery_test.cpp"
      ${XRDAP_CORE_SOURCES})
  target_include_directories(xrdap_sampler_recovery PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/stub" ${XRDAP_CORE_INCLUDE_DIRS})
  target_link_libraries(xrdap_sampler_recovery PRIVATE xr pthread)
  target_compile_options(xrdap_sampler_recovery PRIVATE -Wall -Wextra -Werror)
  add_test(NAME xrdap_sampler_recovery COMMAND xrdap_sampler_recovery)
endif()
