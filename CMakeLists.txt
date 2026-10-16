cmake_minimum_required(VERSION 3.20.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmsis_nn_s4_rowtiling C)

if(NOT CONFIG_CPU_CORTEX_M33)
  message(FATAL_ERROR "This experiment targets Cortex-M33 with DSP extensions")
endif()

target_sources(app PRIVATE src/main.c src/kernels.c)
target_include_directories(app PRIVATE
  src
  ${ZEPHYR_CMSIS_NN_MODULE_DIR}/Source/NNSupportFunctions
)
# Source options are emitted after Zephyr's inherited -O2.
set_property(SOURCE src/main.c src/kernels.c APPEND PROPERTY
  COMPILE_OPTIONS "$<TARGET_PROPERTY:compiler,optimization_fast>")

execute_process(COMMAND git -C ${ZEPHYR_CMSIS_NN_MODULE_DIR} rev-parse HEAD
  OUTPUT_VARIABLE BENCH_CMSIS_REV OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT BENCH_CMSIS_REV)
  set(BENCH_CMSIS_REV "unknown")
endif()
configure_file(src/build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/build_info.h @ONLY)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
