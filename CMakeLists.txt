cmake_minimum_required(VERSION 3.24)

# GrinForge — open-source GRIN Cuckatoo32 GPU miner, no developer fee.
# Target: Windows x64, NVIDIA Ada (sm_89), CUDA 12/13, MSVC.
project(grinforge LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Ada Lovelace. Kept as a cache variable so a future port can add 86/75/61.
set(CMAKE_CUDA_ARCHITECTURES 89 CACHE STRING "CUDA architectures to build for")

set(TRAMP_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tromp-cuckoo")
set(CUCKATOO_INC "${TRAMP_DIR}/src/cuckatoo")
set(CRYPTO_INC   "${TRAMP_DIR}/src/crypto")

if(NOT EXISTS "${CUCKATOO_INC}/lean.cu")
  message(FATAL_ERROR
    "tromp/cuckoo sources not found at ${TRAMP_DIR}.\n"
    "Run: git clone --depth 1 https://github.com/tromp/cuckoo.git third_party/tromp-cuckoo")
endif()

# ---------------------------------------------------------------------------
# Warnings / platform definitions (applied to host and device compilations)
# ---------------------------------------------------------------------------
add_library(grin_warnings INTERFACE)
if(MSVC)
  target_compile_options(grin_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W4 /permissive- /Zc:__cplusplus /utf-8>
    # AVX2 is required by __popcnt64 used in the host-side popcount of the
    # alive bitmap; every GPU host we target (Ryzen 7 1800X and newer) has it.
    $<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,-permissive-,-Zc:__cplusplus,-utf-8,-arch:AVX2>
  )
  target_compile_definitions(grin_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
else()
  target_compile_options(grin_warnings INTERFACE -Wall -Wextra -march=native)
endif()

# ---------------------------------------------------------------------------
# BLAKE2b (public-domain / CC0 reference implementation, vendored via tromp/cuckoo)
# ---------------------------------------------------------------------------
add_library(grin_blake2b STATIC "${CRYPTO_INC}/blake2b-ref.c")
target_include_directories(grin_blake2b PUBLIC "${CRYPTO_INC}")
target_link_libraries(grin_blake2b PRIVATE grin_warnings)
if(MSVC)
  # The vendored reference C file is not /W4-clean and we do not want to fork it.
  target_compile_options(grin_blake2b PRIVATE /W0)
endif()
set_target_properties(grin_blake2b PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Cuckatoo32 lean solver (CUDA)
# ---------------------------------------------------------------------------
add_library(grin_solver STATIC
  src/solver/lean_solver.cu
)
target_include_directories(grin_solver PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/solver"
  "${CUCKATOO_INC}"
  "${CRYPTO_INC}"
)
target_link_libraries(grin_solver PUBLIC grin_blake2b PRIVATE grin_warnings)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
add_executable(solver_bench tools/solver_bench.cpp)
target_link_libraries(solver_bench PRIVATE grin_solver grin_warnings)
set_target_properties(solver_bench PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/stratum_client_test.cpp")
  add_executable(stratum_client_test
    tools/stratum_client_test.cpp
    src/stratum/stratum_client.cpp
  )
  target_include_directories(stratum_client_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(stratum_client_test PRIVATE ws2_32 grin_warnings)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor/telemetry.cpp")
  add_library(grin_monitor STATIC
    src/monitor/telemetry.cpp
    src/monitor/gpu_control.cpp
  )
  target_include_directories(grin_monitor PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor"
  )
  target_link_libraries(grin_monitor PUBLIC grin_warnings)
endif()

# The miner binary needs both the host loop and the monitor module. Guarding on
# both keeps `cmake --build` green while a module is still being written.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/main.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor/telemetry.cpp")
  add_executable(grinforge
    src/host/main.cpp
    src/stratum/stratum_client.cpp
  )
  target_include_directories(grinforge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(grinforge PRIVATE grin_solver ws2_32 grin_warnings)
  if(TARGET grin_monitor)
    target_link_libraries(grinforge PRIVATE grin_monitor)
  endif()
endif()

message(STATUS "grinforge: CUDA architectures = ${CMAKE_CUDA_ARCHITECTURES}")
