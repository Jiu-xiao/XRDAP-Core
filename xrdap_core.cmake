# XRDAP-Core 的源文件和包含路径。固件工程 include 本文件后把 XRDAP_CORE_SOURCES 加进
# 自己的目标、把 XRDAP_CORE_INCLUDE_DIRS 加进包含路径（ESP-IDF 交给
# idf_component_register），并让这些源文件能找到 libxr 的头文件和平台的 xrdap_port.hpp。
#
# Sources and include paths of XRDAP-Core. A firmware project includes this file, adds
# XRDAP_CORE_SOURCES to its own target and XRDAP_CORE_INCLUDE_DIRS to its include path
# (ESP-IDF: pass them to idf_component_register), and makes the libxr headers visible to
# these sources together with the platform's xrdap_port.hpp.
set(XRDAP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}")
# 不依赖 libxr 的纯逻辑，电脑端测试也编译它们 / Pure logic without libxr, also built
# by the host tests.
set(XRDAP_CORE_PURE_SOURCES
    "${XRDAP_CORE_DIR}/xrdap/light/status_light.cpp"
    "${XRDAP_CORE_DIR}/xrdap/probe/probe_identity.cpp"
    "${XRDAP_CORE_DIR}/xrdap/session/busy_reply.cpp"
    "${XRDAP_CORE_DIR}/xrdap/session/session_owner.cpp"
    "${XRDAP_CORE_DIR}/xrdap/vendor/frame_ring.cpp"
    "${XRDAP_CORE_DIR}/xrdap/vendor/reply_space.cpp")
set(XRDAP_CORE_SOURCES
    ${XRDAP_CORE_PURE_SOURCES}
    "${XRDAP_CORE_DIR}/xrdap/vendor/dap_vendor.cpp"
    "${XRDAP_CORE_DIR}/xrdap/vendor/mem_ap.cpp"
    "${XRDAP_CORE_DIR}/xrdap/vendor/sampler.cpp")
# 头文件按 "xrdap/<模块>/<文件>.hpp" 引用 / Headers are included as
# "xrdap/<module>/<file>.hpp".
set(XRDAP_CORE_INCLUDE_DIRS "${XRDAP_CORE_DIR}")
