cmake_minimum_required(VERSION 3.20)
project(raccoon_v2_firmware C)

# ---------------------------------------------------------------------------
# RACCOON V2 firmware build.
#
# One portable core, several back ends.  `rc_core` is the shipping engine-control
# code with no hardware dependency; it is what the unit tests, the SIL and the plant
# model exercise, so the tests test the product rather than a copy of it
# (docs/TESTING_ARCHITECTURE.md section 1).
#
#   host  : unit tests, SIL, sanitizers, static analysis
#   target: STM32H733 via arm-none-eabi-gcc (set -DRC_TARGET=stm32h733)
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RC_TARGET "" "Target: host (default) or stm32h733")
option(RC_SANITIZE "Build with ASan + UBSan" ON)
option(RC_WERROR "Treat warnings as errors" ON)
option(RC_BUILD_TESTS "Build the unit test suite" ON)

set(RC_FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware)

# Warning set.  Zero unexplained warnings is a release gate, not an aspiration.
set(RC_WARNINGS
    -Wall
    -Wextra
    -Werror
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wcast-qual
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wpointer-arith
    -Wundef
    -Wdouble-promotion
    -Wformat=2
)

add_library(rc_warnings INTERFACE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rc_warnings INTERFACE ${RC_WARNINGS})
  if(RC_WERROR)
    target_compile_options(rc_warnings INTERFACE -Wno-error=double-promotion)
  endif()
endif()

# ---------------------------------------------------------------- portable core
add_library(rc_core STATIC
  ${RC_FW_DIR}/src/core/rc_math.c
  ${RC_FW_DIR}/src/core/rc_crc32.c
  ${RC_FW_DIR}/src/flash/rc_flash.c
  ${RC_FW_DIR}/src/link/rc_link.c
  ${RC_FW_DIR}/src/link/rc_crypto.c
  ${RC_FW_DIR}/src/link/rc_link_service.c
  ${RC_FW_DIR}/src/core/rc_reset.c
  ${RC_FW_DIR}/src/engine/engine.c
  ${RC_FW_DIR}/src/engine/control.c
  ${RC_FW_DIR}/src/trigger/trigger.c
  ${RC_FW_DIR}/src/scheduler/scheduler.c
  ${RC_FW_DIR}/src/sensors/sensors.c
  ${RC_FW_DIR}/src/fuel/fuel.c
  ${RC_FW_DIR}/src/ignition/ignition.c
  ${RC_FW_DIR}/src/idle/idle.c
  ${RC_FW_DIR}/src/torque/torque.c
  ${RC_FW_DIR}/src/safety/safety.c
  ${RC_FW_DIR}/src/performance/performance.c
  ${RC_FW_DIR}/src/calibration/cal.c
  ${RC_FW_DIR}/src/calibration/cal_validate.c
  ${RC_FW_DIR}/generated/cal_defaults.c
  ${RC_FW_DIR}/generated/cal_descriptors.c
)
target_include_directories(rc_core PUBLIC
  ${RC_FW_DIR}/include
  ${RC_FW_DIR}/generated
  ${RC_FW_DIR}/src
)
target_link_libraries(rc_core PRIVATE rc_warnings m)

# The wire-format parity test needs the Python reference to have been run, so the two
# implementations of the link protocol are compared rather than merely believed to agree.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_parity.py
            "${CMAKE_BINARY_DIR}/parity_generated.c"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE RC_PARITY_RESULT
    OUTPUT_QUIET)
  if(NOT RC_PARITY_RESULT EQUAL 0)
    message(FATAL_ERROR "the Python link reference failed to generate; parity cannot be checked")
  endif()
else()
  message(FATAL_ERROR "Python 3 is required to build the parity test")
endif()

# ------------------------------------------------------- virtual hardware back end
# Logical time, synthesised inputs, captured outputs.  Used by the unit tests, the SIL
# and the crank simulator.
add_library(rc_hal_virtual STATIC ${RC_FW_DIR}/hardware/hal_virtual.c)
target_include_directories(rc_hal_virtual PUBLIC ${RC_FW_DIR}/include)
target_link_libraries(rc_hal_virtual PUBLIC rc_core)
target_link_libraries(rc_hal_virtual PRIVATE rc_warnings)

# ----------------------------------------------------------------- unit tests
if(RC_BUILD_TESTS)
  enable_testing()
  set(RC_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
  set(RC_SIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware/sim)
  set(RC_TEST_NAMES
    test_main.c
    test_math.c
    test_engine.c
    test_trigger.c
    test_scheduler.c
    test_sensors.c
    test_fuel.c
    test_ignition.c
    test_torque.c
    test_safety.c
    test_idle.c
    test_performance.c
    test_calibration.c
    test_fault_injection.c
    test_flash.c
    test_link.c
    test_crypto.c
    test_link_service.c
    rc_test.c
  )
  # The software-in-the-loop simulator: a virtual engine and a real ECU, wired together.
  add_executable(rc_sim
    firmware/sim/rc_sim.c
    firmware/sim/rc_sim_harness.c
    firmware/sim/rc_sim_scenarios.c
    firmware/sim/rc_sim_fuzz.c
    firmware/sim/rc_sim_link.c
    firmware/sim/rc_sim_main.c
    tests/unit/rc_test.c
  )
  target_include_directories(rc_sim PRIVATE ${RC_SIM_DIR} ${RC_TEST_DIR})
  target_link_libraries(rc_sim PRIVATE rc_core rc_hal_virtual rc_warnings m)
  add_test(NAME rc_sim COMMAND rc_sim)

  set(RC_TEST_SOURCES "")
  foreach(name IN LISTS RC_TEST_NAMES)
    list(APPEND RC_TEST_SOURCES "${RC_TEST_DIR}/${name}")
  endforeach()
  # Added outside the name loop, because the loop prefixes a directory onto every entry
  # and these two are absolute paths from the build tree.
  list(APPEND RC_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/parity/test_parity.c"
    "${CMAKE_BINARY_DIR}/parity_generated.c")
  add_executable(rc_tests ${RC_TEST_SOURCES})
  target_include_directories(rc_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity
  )
  target_link_libraries(rc_tests PRIVATE rc_hal_virtual rc_warnings)
  if(RC_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rc_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(rc_tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME rc_tests COMMAND rc_tests)
  set_tests_properties(rc_tests PROPERTIES TIMEOUT 300)
endif()

# ----------------------------------------------------------------- target build
if(RC_TARGET STREQUAL "stm32h733")
  set(CMAKE_SYSTEM_NAME Generic)
  set(CMAKE_SYSTEM_PROCESSOR arm)
  set(CMAKE_C_COMPILER arm-none-eabi-gcc)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  add_compile_definitions(RC_TARGET_STM32H733=1)
  add_library(rc_hal_target STATIC ${RC_FW_DIR}/hardware/hal_stm32h7.c)
  target_link_libraries(rc_hal_target PUBLIC rc_core)
  target_link_libraries(rc_hal_target PRIVATE rc_warnings)
  # The CMSIS device header is supplied by the STM32CubeH7 package.  Its absence is a
  # configuration error, not a silently missing driver.
  target_compile_definitions(rc_hal_target PUBLIC RC_HAL_STM32H733=1)
endif()
