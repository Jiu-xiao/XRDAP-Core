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

set(CMAKE_CXX_STANDARD 20)
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
  # 读时间走虚拟时钟（unit/radio_clock.cpp）/ Reading the time goes to the virtual clock
  # (unit/radio_clock.cpp).
  target_link_options(xrdap_core_unit PRIVATE
                      -Wl,--wrap=_ZN5LibXR8Timebase15GetMicrosecondsEv)
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

  # 执行器，部分用例用真线程，时钟和厂商扩展的测试是同一个 / The executor, some cases with
  # real threads, on the same clock as the vendor extension tests.
  file(GLOB XRDAP_THREAD_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/threads/*.cpp")
  add_executable(xrdap_core_threads ${XRDAP_THREAD_TESTS}
                                    "${CMAKE_CURRENT_LIST_DIR}/vendor/test_clock.cpp"
                                    "${CMAKE_CURRENT_LIST_DIR}/unit/main.cpp"
                                    ${XRDAP_CORE_SOURCES})
  target_include_directories(xrdap_core_threads PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                        ${XRDAP_CORE_INCLUDE_DIRS}
                                                        ${XRDAP_LIBXR_USB_DIRS})
  target_link_libraries(xrdap_core_threads PRIVATE xr pthread)
  target_compile_options(xrdap_core_threads PRIVATE -Wall -Wextra)
  add_test(NAME xrdap_core_threads COMMAND xrdap_core_threads)

  # 厂商扩展：MEM-AP、采样和命令，对着 SWD 模型和只由测试拨动的时钟（test_clock.cpp）跑 /
  # The vendor extension: MEM-AP, sampling and commands, against an SWD model and a clock
  # only the tests move (test_clock.cpp).
  file(GLOB XRDAP_VENDOR_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/vendor/*.cpp")
  add_executable(xrdap_core_vendor ${XRDAP_VENDOR_TESTS}
                                   "${CMAKE_CURRENT_LIST_DIR}/unit/main.cpp"
                                   ${XRDAP_CORE_SOURCES})
  target_include_directories(xrdap_core_vendor PRIVATE "${CMAKE_CURRENT_LIST_DIR}/stub"
                                                       ${XRDAP_CORE_INCLUDE_DIRS})
  target_link_libraries(xrdap_core_vendor PRIVATE xr pthread)
  target_compile_options(xrdap_core_vendor PRIVATE -Wall -Wextra -Werror)
  add_test(NAME xrdap_core_vendor COMMAND xrdap_core_vendor)
endif()
