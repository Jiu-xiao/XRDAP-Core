# xrdap/ 里的原子变量只用 32 位整数。QingKe V4（CH32V307，-march=rv32imcf_zaamo）只有
# 字宽的原子指令，单字节或半字的读改写（exchange、compare_exchange、fetch_*）会变成
# 链接不到的 libatomic 调用；RV32 也没有 64 位原子指令。用法：cmake -DROOT=<仓库> -P 本文件
#
# Atomics in xrdap/ are 32-bit integers only. QingKe V4 (CH32V307,
# -march=rv32imcf_zaamo) has word-sized atomic instructions only, so a byte or halfword
# read-modify-write (exchange, compare_exchange, fetch_*) becomes a libatomic call that
# does not link, and RV32 has no 64-bit atomics either. Usage: cmake -DROOT=<repo> -P
# this file

file(GLOB_RECURSE sources "${ROOT}/xrdap/*.hpp" "${ROOT}/xrdap/*.cpp")
set(bad "")
foreach(source ${sources})
  file(STRINGS "${source}" lines REGEX "std::atomic<")
  foreach(line ${lines})
    string(REGEX MATCHALL "std::atomic<[^>]*>" uses "${line}")
    foreach(use ${uses})
      if(NOT use MATCHES "^std::atomic<(uint32_t|int32_t)>$")
        file(RELATIVE_PATH rel "${ROOT}" "${source}")
        list(APPEND bad "${rel}: ${use}")
      endif()
    endforeach()
  endforeach()
endforeach()
if(bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "atomics other than uint32_t/int32_t:\n  ${text}")
endif()
message(STATUS "word atomics only: ok")
