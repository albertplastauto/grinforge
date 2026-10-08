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
target_compile_definitions(solver_bench PRIVATE EDGEBITS=32 PROOFSIZE=42)
set_target_properties(solver_bench PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)

# ---------------------------------------------------------------------------
# Diagnostic build: the identical solver at EDGEBITS=29.
#
# Cuckoo Cycle at 2^32 edges produces solutions rarely, so "found nothing" over
# a handful of nonces cannot distinguish a bug from statistics. At 2^29 the same
# code path (header -> BLAKE2b -> siphash keys -> trimming -> cycle search ->
# verify) runs ~8x faster and yields solutions often enough to validate the whole
# pipeline in seconds. This binary is a test instrument, not a miner.
# ---------------------------------------------------------------------------
add_library(grin_solver29 STATIC src/solver/lean_solver.cu)
target_compile_definitions(grin_solver29 PRIVATE EDGEBITS=29)
target_include_directories(grin_solver29 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/solver"
  "${CUCKATOO_INC}"
  "${CRYPTO_INC}"
)
target_link_libraries(grin_solver29 PUBLIC grin_blake2b PRIVATE grin_warnings)

add_executable(solver_bench29 tools/solver_bench.cpp)
target_link_libraries(solver_bench29 PRIVATE grin_solver29 grin_warnings)
# The harness itself must use the same parameters as the solver it drives,
# otherwise its host-side re-verification and masks are wrong.
target_compile_definitions(solver_bench29 PRIVATE EDGEBITS=29 PROOFSIZE=42)
set_target_properties(solver_bench29 PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)

# ---------------------------------------------------------------------------
# End-to-end pipeline self-test at deliberately tiny parameters.
#
# 42-cycles at 2^29/2^32 edges are rare, so "found nothing" cannot distinguish a
# correct solver from a broken one. At EDGEBITS=20 with a 6-cycle the same code
# path must find many solutions immediately, and every one of them must pass
# grin_verify. If this binary finds cycles, the whole pipeline (header -> BLAKE2b
# -> siphash -> trimming -> cycle search -> verification) is correct and the
# absence of 42-cycles is purely a probability question.
# ---------------------------------------------------------------------------
add_library(grin_solver_tiny STATIC src/solver/lean_solver.cu)
# IDXSHIFT sizes the cycle-finder graph (MAXEDGES = NEDGES >> IDXSHIFT). The C32
# default of 8 would cap a 2^20 graph at 4096 edges while its 2-core is ~12000.
target_compile_definitions(grin_solver_tiny PRIVATE EDGEBITS=20 PROOFSIZE=6 IDXSHIFT=4)
target_include_directories(grin_solver_tiny PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/solver"
  "${CUCKATOO_INC}"
  "${CRYPTO_INC}"
)
target_link_libraries(grin_solver_tiny PUBLIC grin_blake2b PRIVATE grin_warnings)

add_executable(solver_bench_tiny tools/solver_bench.cpp)
target_compile_definitions(solver_bench_tiny PRIVATE EDGEBITS=20 PROOFSIZE=6)
target_link_libraries(solver_bench_tiny PRIVATE grin_solver_tiny grin_warnings)
set_target_properties(solver_bench_tiny PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)

# The independent CPU reference: builds the full graph with no trimming at all.
add_executable(simple_ref_tiny tools/simple_ref.cpp)
# print_log emits one line per cycle of ANY length, and short cycles are extremely
# numerous, so console I/O would dominate the run. SQUASH_OUTPUT=1 makes print_log
# a no-op (grin_params.hpp).
target_compile_definitions(simple_ref_tiny PRIVATE EDGEBITS=20 PROOFSIZE=6 SQUASH_OUTPUT=1)
target_include_directories(simple_ref_tiny PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/solver"
  "${CUCKATOO_INC}"
  "${CRYPTO_INC}"
)
target_link_libraries(simple_ref_tiny PRIVATE grin_blake2b grin_warnings)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/stratum_client_test.cpp")
  add_executable(stratum_client_test
    tools/stratum_client_test.cpp
    src/stratum/stratum_client.cpp
  )
  target_include_directories(stratum_client_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(stratum_client_test PRIVATE ws2_32 grin_warnings)
endif()

set(GRIN_HAVE_MONITOR FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor/telemetry.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor/gpu_control.cpp")
  add_library(grin_monitor STATIC
    src/monitor/telemetry.cpp
    src/monitor/gpu_control.cpp
  )
  target_include_directories(grin_monitor PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/monitor"
  )
  target_link_libraries(grin_monitor PUBLIC grin_warnings)
  set(GRIN_HAVE_MONITOR TRUE)
else()
  message(STATUS "grinforge: monitor module incomplete, skipping the miner binary")
endif()

# The miner binary needs both the host loop and the monitor module.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/main.cpp" AND GRIN_HAVE_MONITOR)
  add_executable(grinforge
    src/host/main.cpp
    src/host/http_api.cpp
    src/stratum/stratum_client.cpp
  )
  target_include_directories(grinforge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(grinforge PRIVATE grin_solver ws2_32 grin_warnings grin_monitor)
endif()

message(STATUS "grinforge: CUDA architectures = ${CMAKE_CUDA_ARCHITECTURES}")
