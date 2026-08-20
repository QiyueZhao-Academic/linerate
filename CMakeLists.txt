cmake_minimum_required(VERSION 3.20)
project(linerate VERSION 2.0.0 LANGUAGES C CXX)

# C++17 rather than something newer: the same source has to build under GCC on
# the measurement host and AppleClang on the development machine, and nothing
# here needs a later standard. C11 for the SIMD file, which is C so that its
# target attributes and intrinsics never leak into the C++ side.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# -march=native is deliberate and its consequence is stated in the report:
# absolute cycle counts are specific to the microarchitecture they were measured
# on. Portable binaries would understate what a tuned data plane achieves.
set(LR_ARCH_FLAGS "")
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" LR_HAS_MARCH_NATIVE)
if(LR_HAS_MARCH_NATIVE)
  set(LR_ARCH_FLAGS "-march=native")
else()
  # Apple silicon clang rejects -march=native; -mcpu=native is the equivalent.
  check_cxx_compiler_flag("-mcpu=native" LR_HAS_MCPU_NATIVE)
  if(LR_HAS_MCPU_NATIVE)
    set(LR_ARCH_FLAGS "-mcpu=native")
  endif()
endif()

add_compile_options(-O3 -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer)
if(LR_ARCH_FLAGS)
  add_compile_options(${LR_ARCH_FLAGS})
endif()

# ---------------------------------------------------------------------------
# OpenSSL 3. Resolution order, most specific first, so that a self-contained
# build under Networks-HPC/linerate-env wins over anything installed globally.
# ---------------------------------------------------------------------------
if(DEFINED ENV{LR_OPENSSL_ROOT} AND EXISTS "$ENV{LR_OPENSSL_ROOT}")
  set(OPENSSL_ROOT_DIR "$ENV{LR_OPENSSL_ROOT}")
  message(STATUS "OpenSSL from LR_OPENSSL_ROOT: ${OPENSSL_ROOT_DIR}")
elseif(APPLE)
  # macOS ships LibreSSL headers only; Homebrew's openssl@3 is the usual source.
  execute_process(COMMAND brew --prefix openssl@3
                  OUTPUT_VARIABLE LR_BREW_SSL
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  if(LR_BREW_SSL AND EXISTS "${LR_BREW_SSL}")
    set(OPENSSL_ROOT_DIR "${LR_BREW_SSL}")
    message(STATUS "OpenSSL from Homebrew: ${OPENSSL_ROOT_DIR}")
  endif()
endif()

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Optional dependencies. Each one that is absent removes an experiment from the
# sweep and records why, rather than failing the build: the laptop has none of
# them and still has to compile, self-test and typeset.
# ---------------------------------------------------------------------------
find_path(LR_URING_INCLUDE liburing.h)
find_library(LR_URING_LIB uring)
set(LR_HAVE_URING OFF)
if(LR_URING_INCLUDE AND LR_URING_LIB)
  set(LR_HAVE_URING ON)
endif()

find_package(OpenMP QUIET)
find_package(MPI QUIET COMPONENTS CXX)

include(CheckLanguage)
check_language(CUDA)
set(LR_HAVE_CUDA OFF)
if(CMAKE_CUDA_COMPILER)
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    # Covers Turing through Hopper, which is every GPU a GCE accelerator
    # instance offers at the time of writing.
    set(CMAKE_CUDA_ARCHITECTURES 75 80 86 89 90)
  endif()
  set(LR_HAVE_CUDA ON)
endif()

# ---------------------------------------------------------------------------
# Provenance baked into the binary.
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(LR_GIT_COMMIT "unversioned")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE LR_GIT_COMMIT
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE LR_GIT_DIRTY
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  if(LR_GIT_DIRTY)
    set(LR_GIT_COMMIT "${LR_GIT_COMMIT}-dirty")
  endif()
endif()
set(LR_COMPILER   "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(LR_CXX_FLAGS  "-O3 ${LR_ARCH_FLAGS} -fno-omit-frame-pointer")
set(LR_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
set(LR_VERSION    "${PROJECT_VERSION}")
configure_file(${CMAKE_SOURCE_DIR}/cmake/build_info.hpp.in
               ${CMAKE_BINARY_DIR}/generated/build_info.hpp @ONLY)

# ---------------------------------------------------------------------------
# Library and binaries.
# ---------------------------------------------------------------------------
add_library(lr_core STATIC
  src/core/simd_crypto.c
  src/core/platform.cpp
  src/core/clock.cpp
  src/core/aead_openssl.cpp
  src/core/aead_simd.cpp
  src/core/aead_factory.cpp
  src/core/proto.cpp
  src/core/transport_posix.cpp
  src/core/transport_mmsg.cpp
  src/core/transport_uring.cpp
  src/core/transport_factory.cpp
  src/core/worker.cpp
  src/core/ctrl.cpp
  src/core/stats.cpp
  src/core/json.cpp
)
target_include_directories(lr_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(lr_core PUBLIC OpenSSL::Crypto Threads::Threads)
if(LR_HAVE_URING)
  target_compile_definitions(lr_core PUBLIC LR_HAVE_URING=1)
  target_include_directories(lr_core PUBLIC ${LR_URING_INCLUDE})
  target_link_libraries(lr_core PUBLIC ${LR_URING_LIB})
endif()
if(OpenMP_CXX_FOUND)
  target_link_libraries(lr_core PUBLIC OpenMP::OpenMP_CXX)
endif()

add_executable(lr_selftest src/apps/lr_selftest.cpp)
add_executable(lr_sink     src/apps/lr_sink.cpp)
add_executable(lr_gen      src/apps/lr_gen.cpp)
add_executable(lr_loadgend src/apps/lr_loadgend.cpp)
add_executable(lr_bench
  src/bench/bench_main.cpp
  src/bench/bench.cpp
  src/bench/emit_json.cpp
  src/bench/exp_e1_cost_model.cpp
  src/bench/exp_e2_scaling.cpp
  src/bench/exp_e3_iopath.cpp
  src/bench/exp_e4_memroof.cpp
  src/bench/exp_e5_latency.cpp
  src/bench/exp_e6_baseline.cpp
  src/bench/exp_e7_virtualization.cpp
  src/bench/exp_controls.cpp
)
target_include_directories(lr_bench PRIVATE ${CMAKE_SOURCE_DIR}/src/bench)

foreach(t lr_selftest lr_sink lr_gen lr_loadgend lr_bench)
  target_link_libraries(${t} PRIVATE lr_core)
endforeach()

# E2's cross-node half. Built only where an MPI compiler exists; the experiment
# records its absence rather than the build failing.
if(MPI_CXX_FOUND)
  add_executable(lr_mpi_scale src/mpi/lr_mpi_scale.cpp)
  target_link_libraries(lr_mpi_scale PRIVATE lr_core MPI::MPI_CXX)
  message(STATUS "  MPI        ${MPI_CXX_COMPILER}")
endif()

# E8, the GPU branch. Same story: absent without a CUDA toolkit.
if(LR_HAVE_CUDA)
  add_executable(lr_gpu_aead src/cuda/lr_gpu_aead.cu)
  target_include_directories(lr_gpu_aead PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(lr_gpu_aead PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  target_link_libraries(lr_gpu_aead PRIVATE lr_core)
  message(STATUS "  CUDA       ${CMAKE_CUDA_COMPILER}")
endif()

enable_testing()
add_test(NAME selftest COMMAND lr_selftest)

message(STATUS "linerate ${LR_VERSION} (${LR_GIT_COMMIT})")
message(STATUS "  compiler   ${LR_COMPILER}")
message(STATUS "  flags      ${LR_CXX_FLAGS}")
message(STATUS "  openssl    ${OPENSSL_VERSION} (${OPENSSL_INCLUDE_DIR})")
message(STATUS "  io_uring   ${LR_HAVE_URING}")
message(STATUS "  openmp     ${OpenMP_CXX_FOUND}")
message(STATUS "  mpi        ${MPI_CXX_FOUND}")
message(STATUS "  cuda       ${LR_HAVE_CUDA}")
