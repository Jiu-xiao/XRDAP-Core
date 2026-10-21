# XRDAP-Core 的源文件和包含路径。固件工程 include 本文件后把 XRDAP_CORE_SOURCES 加进
# 自己的目标、把 XRDAP_CORE_INCLUDE_DIRS 加进包含路径（ESP-IDF 交给
# idf_component_register），并让这些源文件能找到 libxr 的头文件。
#
# Sources and include paths of XRDAP-Core. A firmware project includes this file, adds
# XRDAP_CORE_SOURCES to its own target and XRDAP_CORE_INCLUDE_DIRS to its include path
# (ESP-IDF: pass them to idf_component_register), and makes the libxr headers visible to
# these sources.
set(XRDAP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(XRDAP_CORE_SOURCES
    "${XRDAP_CORE_DIR}/light/status_light.cpp"
    "${XRDAP_CORE_DIR}/probe/probe_identity.cpp"
    "${XRDAP_CORE_DIR}/vendor/dap_vendor.cpp"
    "${XRDAP_CORE_DIR}/vendor/frame_ring.cpp"
    "${XRDAP_CORE_DIR}/vendor/mem_ap.cpp"
    "${XRDAP_CORE_DIR}/vendor/sampler.cpp")
set(XRDAP_CORE_INCLUDE_DIRS
    "${XRDAP_CORE_DIR}/light"
    "${XRDAP_CORE_DIR}/probe"
    "${XRDAP_CORE_DIR}/vendor")
